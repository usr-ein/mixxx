#include "network/prolink/server/prolinkstatusserver.h"

#include <kaitai/exceptions.h>
#include <kaitai/kaitaistream.h>

#include <QTimer>
#include <QUdpSocket>
#include <exception>
#include <sstream>

#include "moc_prolinkstatusserver.cpp"
#include "prolink_status.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkStatusServer");

using namespace mixxx::prolink;
using namespace mixxx::prolink::server;

/// Status-packet cadence. Measured at 199 ms mean (min 63, max 207) across a
/// real CDJ-2000nexus session.
constexpr int kStatusIntervalMs = 200;

/// A real CDJ-2000nexus status packet on firmware 1.44, with a stick loaded, and
/// the identifying fields zeroed: name, device number, media states, link flag
/// and packet counter. Everything else is preserved exactly as the deck sent it,
/// because only six of these 284 bytes ever changed across 749 consecutive
/// packets and we cannot name most of the rest.
const char kStatusTemplate[] =
        "5173707431576d4a4f4c0a000000000000000000000000000000000000000001040000f8"
        "000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000000000000001000604"
        "00000000000000000100000000000000312e343400000000000000030084fffe000f8312"
        "7fffffff7fffffff00000000000000ff0000000401ff0000000000000000000000000000"
        "000001000000000000000000000f831200000000000000000f0100001234567800000001"
        "010101010201000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000001500000753000005b4";

/// A real type-0x06 media response, with the identifying and per-medium fields
/// zeroed and substituted below.
const char kMediaResponseTemplate[] =
        "5173707431576d4a4f4c060000000000000000000000000000000000000000010000009c"
        "000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000000000000000000000"
        "0032003000320035002d00300036002d0032003400000000003100300030003000000000"
        "000000000000000000000000000000000000000000000000000001010000000000000007"
        "28ca8000000000057d06c000";

// The 50002 header: the name runs 0x0b-0x1e, one byte earlier and one shorter
// than on port 50000, with a structural 0x01 at 0x1f (C14).
constexpr int kNameOffset = 0x0B;
constexpr int kNameLength = 20;
constexpr int kStructuralOne = 0x1F;
constexpr int kSenderDevice = 0x21;

// Status packet.
constexpr int kStatusSubtype = 0x20;
constexpr int kStatusDeviceAgain = 0x24;
constexpr int kStatusUsbState = 0x6F;
constexpr int kStatusSdState = 0x73;
constexpr int kStatusLinkAvailable = 0x75;
constexpr int kStatusPacketCounter = 0xC8;

// Media response.
constexpr int kResponseDevice = 0x24;
constexpr int kResponseSlot = 0x28;
constexpr int kResponseName = 0x2C;
constexpr int kResponseNameLength = 0x40;
constexpr int kResponseTrackCount = 0xA4;
constexpr int kResponsePlaylistCount = 0xAC;

// Settings response.
constexpr int kSettingsLengthField = 0x22;
constexpr int kSettingsRequester = 0x24;
constexpr int kSettingsSlot = 0x25;
constexpr int kSettingsMagicField = 0x28;
constexpr int kSettingsUnknown = 0x2C;
constexpr int kSettingsPayload = 0x30;
/// Leads the settings block. Constant in the one exchange captured, and the same
/// value that leads the payload of `PIONEER/MYSETTING.DAT` — which is what ties
/// the file to the wire.
constexpr quint32 kSettingsMagic = 0x12345678;
constexpr int kSettingsLength = 32;

/// A slot's local state, as published at 0x6f and 0x73.
constexpr quint8 kMediaLoaded = 0x00;
constexpr quint8 kMediaEmpty = 0x04;

void writeU32(QByteArray* pOut, int offset, quint32 value) {
    for (int i = 0; i < 4; ++i) {
        (*pOut)[offset + i] = static_cast<char>((value >> (24 - 8 * i)) & 0xFF);
    }
}

void writeU16(QByteArray* pOut, int offset, quint16 value) {
    (*pOut)[offset] = static_cast<char>((value >> 8) & 0xFF);
    (*pOut)[offset + 1] = static_cast<char>(value & 0xFF);
}

/// Stamp the shared 50002 header: name, structural byte, sender number.
void writeHeader(QByteArray* pOut, const QString& deviceName, int deviceNumber) {
    const QByteArray ascii = deviceName.toLatin1().left(kNameLength);
    pOut->replace(kNameOffset, kNameLength, QByteArray(kNameLength, '\0'));
    pOut->replace(kNameOffset, ascii.size(), ascii);
    (*pOut)[kStructuralOne] = 0x01;
    (*pOut)[kSenderDevice] = static_cast<char>(deviceNumber & 0xFF);
}

} // namespace

namespace mixxx {
namespace prolink {
namespace server {

QByteArray buildMediaResponse(int deviceNumber,
        const QString& deviceName,
        MediaSlot slot,
        const SlotAdvertisement& advertisement) {
    QByteArray packet = QByteArray::fromHex(QByteArray(kMediaResponseTemplate));
    writeHeader(&packet, deviceName, deviceNumber);
    writeU32(&packet, kResponseDevice, static_cast<quint32>(deviceNumber));
    writeU32(&packet, kResponseSlot, static_cast<quint32>(slot));

    // UTF-16 **big**-endian, like the dbserver strings and unlike the NFS
    // layer's UTF-16LE. Fixed 64 bytes, NUL-padded.
    QByteArray volume;
    for (const QChar character : advertisement.volumeName) {
        const ushort unit = character.unicode();
        volume.append(static_cast<char>((unit >> 8) & 0xFF));
        volume.append(static_cast<char>(unit & 0xFF));
    }
    volume.truncate(kResponseNameLength);
    packet.replace(kResponseName, kResponseNameLength, QByteArray(kResponseNameLength, '\0'));
    packet.replace(kResponseName, volume.size(), volume);

    writeU32(&packet, kResponseTrackCount, advertisement.trackCount);
    writeU32(&packet, kResponsePlaylistCount, advertisement.playlistCount);
    return packet;
}

QByteArray buildSettingsResponse(int deviceNumber,
        const QString& deviceName,
        int requester,
        MediaSlot slot,
        const QByteArray& settings) {
    QByteArray body = settings.left(kSettingsLength);
    body.append(kSettingsLength - body.size(), '\0');

    QByteArray packet(kSettingsPayload + kSettingsLength, '\0');
    packet.replace(0, kMagicLength, QByteArray::fromRawData(kMagic, kMagicLength));
    packet[kOffsetType] = static_cast<char>(0x36);
    writeHeader(&packet, deviceName, deviceNumber);
    writeU16(&packet,
            kSettingsLengthField,
            static_cast<quint16>(packet.size() - kSettingsRequester));
    packet[kSettingsRequester] = static_cast<char>(requester & 0xFF);
    packet[kSettingsSlot] = static_cast<char>(static_cast<int>(slot) & 0xFF);
    writeU32(&packet, kSettingsMagicField, kSettingsMagic);
    writeU32(&packet, kSettingsUnknown, 2);
    packet.replace(kSettingsPayload, kSettingsLength, body);
    return packet;
}

ProLinkStatusServer::ProLinkStatusServer(QUdpSocket* pSocket, QObject* parent)
        : QObject(parent), m_pSocket(pSocket) {
}

ProLinkStatusServer::~ProLinkStatusServer() = default;

void ProLinkStatusServer::start() {
    if (!m_pTimer) {
        m_pTimer = new QTimer(this);
        connect(m_pTimer, &QTimer::timeout, this, &ProLinkStatusServer::emitStatus);
    }
    if (!m_pTimer->isActive()) {
        m_pTimer->start(kStatusIntervalMs);
    }
}

void ProLinkStatusServer::stop() {
    if (m_pTimer) {
        m_pTimer->stop();
    }
}

void ProLinkStatusServer::setDeviceNumber(int number) {
    m_deviceNumber = number;
}

void ProLinkStatusServer::setSlot(MediaSlot slot, const SlotAdvertisement& advertisement) {
    m_slots.insert(static_cast<int>(slot), advertisement);
}

void ProLinkStatusServer::clearSlot(MediaSlot slot) {
    m_slots.remove(static_cast<int>(slot));
}

QByteArray ProLinkStatusServer::buildStatus() const {
    QByteArray packet = QByteArray::fromHex(QByteArray(kStatusTemplate));
    writeHeader(&packet, QString::fromLatin1(kVirtualCdjName), m_deviceNumber);
    // 0x20 is left as the template has it (0x04). It looks like a subtype and
    // zeroing it looks harmless, which is exactly why it is called out here:
    // every byte we cannot name stays as the real deck sent it, and "probably
    // ignored" is not a reason to differ from hardware we are impersonating.
    // The device number appears twice, at 0x21 and 0x24.
    packet[kStatusDeviceAgain] = static_cast<char>(m_deviceNumber & 0xFF);

    const auto stateOf = [this](MediaSlot slot) {
        const auto found = m_slots.constFind(static_cast<int>(slot));
        return found != m_slots.constEnd() && found->occupied ? kMediaLoaded : kMediaEmpty;
    };
    packet[kStatusUsbState] = static_cast<char>(stateOf(MediaSlot::Usb));
    packet[kStatusSdState] = static_cast<char>(stateOf(MediaSlot::Sd));
    // 0x74 is left exactly as the real deck sent it. It takes 0 and 1 and is
    // clearly media-related, but it does not track 0x75 -- three of the four
    // combinations occur -- so it is a separate flag we cannot yet name.
    // Guessing would be worse than copying.
    packet[kStatusLinkAvailable] = m_slots.isEmpty() ? 0x00 : 0x01;
    writeU32(&packet, kStatusPacketCounter, m_packetCounter);
    return packet;
}

void ProLinkStatusServer::emitStatus() {
    // Five seconds' worth of ticks. Emission is the one part of the serve side
    // with no natural log line -- it succeeds silently 5 times a second -- so
    // when a deck does not see us there is otherwise nothing to distinguish
    // "not sending" from "sending and ignored". Cheap enough to keep.
    constexpr int kLogEvery = 25;
    const bool shouldLog = (m_packetCounter % kLogEvery) == 0;

    if (m_deviceNumber == 0 || !m_pSocket) {
        // No number means the handshake has not finished, and a status packet
        // from device 0 is worse than none.
        if (shouldLog) {
            kLogger.debug() << "not emitting status yet: device number"
                            << m_deviceNumber << "socket" << (m_pSocket != nullptr);
        }
        ++m_packetCounter;
        return;
    }
    const QByteArray packet = buildStatus();
    ++m_packetCounter;
    // Unicast per peer, not broadcast: not one of 1507 captured status packets
    // was broadcast (F21), and a peer that has not announced itself gets
    // nothing -- which mirrors why we received nothing until we announced.
    for (const QHostAddress& peer : m_peers) {
        if (m_pSocket->writeDatagram(packet, peer, kStatusPort) < 0) {
            kLogger.warning() << "status to" << peer.toString()
                              << "failed:" << m_pSocket->errorString();
        }
    }
    if (shouldLog) {
        kLogger.info() << "emitting status as device" << m_deviceNumber << "to"
                       << m_peers.size() << "peer(s);" << m_slots.size()
                       << "slot(s) advertised";
    }
}

bool ProLinkStatusServer::handleDatagram(
        const QByteArray& datagram, const QHostAddress& sender) {
    if (m_deviceNumber == 0) {
        return false;
    }
    if (datagram.size() <= kOffsetType ||
            !datagram.startsWith(QByteArray::fromRawData(kMagic, kMagicLength))) {
        return false;
    }
    const quint8 type = static_cast<quint8>(datagram.at(kOffsetType));
    if (type == static_cast<quint8>(prolink_status_t::PACKET_TYPE_MEDIA_QUERY)) {
        answerMediaQuery(datagram, sender);
        return true;
    }
    if (type == static_cast<quint8>(prolink_status_t::PACKET_TYPE_SETTINGS_QUERY)) {
        answerSettingsQuery(datagram, sender);
        return true;
    }
    if (type == static_cast<quint8>(prolink_status_t::PACKET_TYPE_CDJ_STATUS)) {
        notePeerStatus(datagram, sender);
        // Deliberately false: we read it, we did not answer it, and the caller
        // still needs it to work out who holds tempo master.
        return false;
    }
    return false;
}

QList<ServeConsumer> ProLinkStatusServer::consumers() const {
    return m_consumers.values();
}

void ProLinkStatusServer::notePeerStatus(
        const QByteArray& datagram, const QHostAddress& sender) {
    std::string buffer(datagram.constData(), static_cast<size_t>(datagram.size()));
    std::istringstream stream(buffer);
    ServeConsumer consumer;
    int sourcePlayer = 0;
    try {
        kaitai::kstream ks(&stream);
        prolink_status_t packet(&ks);
        consumer.deviceNumber = packet.sender_device();
        consumer.deviceName = QString::fromStdString(packet.device_name());
        consumer.address = sender;
        sourcePlayer = packet.status_source_player();
        consumer.slot = static_cast<MediaSlot>(packet.status_source_slot());
        consumer.trackId = packet.status_track_id();
        // Play states above 0x06 are the stopped/cued/ended family; the playing
        // ones are 0x03 (playing), 0x04 (looping) and 0x05 (nearing the end).
        const quint8 playState = packet.status_play_state();
        consumer.playing = playState >= 0x03 && playState <= 0x05;
    } catch (const std::exception&) {
        // A status packet shorter than the fields we want. Not worth a log line
        // four times a second per deck.
        return;
    }
    if (consumer.deviceNumber == 0) {
        return;
    }

    // Only tracks whose source is *us*. A deck playing off its own stick, or off
    // the other deck's, is not our business and would otherwise fill this list.
    const bool oursIsLoaded = sourcePlayer == m_deviceNumber && consumer.trackId != 0 &&
            m_slots.contains(static_cast<int>(consumer.slot));

    const auto existing = m_consumers.constFind(consumer.deviceNumber);
    if (!oursIsLoaded) {
        if (existing != m_consumers.constEnd()) {
            m_consumers.remove(consumer.deviceNumber);
            emit consumersChanged();
        }
        return;
    }
    // Four packets a second per deck arrive saying the same thing. Only signal
    // when something a reader would notice has actually changed.
    if (existing != m_consumers.constEnd() && existing->trackId == consumer.trackId &&
            existing->slot == consumer.slot && existing->playing == consumer.playing) {
        return;
    }
    m_consumers.insert(consumer.deviceNumber, consumer);
    emit consumersChanged();
}

void ProLinkStatusServer::answerMediaQuery(
        const QByteArray& datagram, const QHostAddress& sender) {
    // Parsed by the generated Kaitai reader from prolink_status.ksy.
    std::string buffer(datagram.constData(), static_cast<size_t>(datagram.size()));
    std::istringstream stream(buffer);
    int target = 0;
    int slotValue = 0;
    QHostAddress requesterIp = sender;
    try {
        kaitai::kstream ks(&stream);
        prolink_status_t packet(&ks);
        target = static_cast<int>(packet.query_target_device());
        slotValue = static_cast<int>(packet.query_slot());
        const std::string& raw = packet.query_requester_ip();
        if (raw.size() == 4) {
            quint32 address = 0;
            for (const char byte : raw) {
                address = (address << 8) | static_cast<quint8>(byte);
            }
            requesterIp = QHostAddress(address);
        }
    } catch (const std::exception& e) {
        kLogger.debug() << "undecodable media query:" << e.what();
        return;
    }

    if (target != m_deviceNumber) {
        return; // addressed to another player
    }
    const auto found = m_slots.constFind(slotValue);
    if (found == m_slots.constEnd()) {
        // A slot we do not serve. Saying nothing is right: an empty reply would
        // tell the deck the slot exists and holds no tracks, and it would then
        // offer an empty medium (F24).
        return;
    }

    const auto slot = static_cast<MediaSlot>(slotValue);
    m_pSocket->writeDatagram(
            buildMediaResponse(
                    m_deviceNumber, QString::fromLatin1(kVirtualCdjName), slot, *found),
            requesterIp,
            kStatusPort);
    ++m_mediaQueriesAnswered;
    kLogger.info() << "answered media query from" << requesterIp.toString() << "for slot"
                   << slotValue << "-" << found->volumeName << found->trackCount
                   << "tracks," << found->playlistCount << "playlists";
}

void ProLinkStatusServer::answerSettingsQuery(
        const QByteArray& datagram, const QHostAddress& sender) {
    std::string buffer(datagram.constData(), static_cast<size_t>(datagram.size()));
    std::istringstream stream(buffer);
    int requester = 0;
    int slotValue = 0;
    try {
        kaitai::kstream ks(&stream);
        prolink_status_t packet(&ks);
        requester = packet.settings_requester();
        slotValue = static_cast<int>(packet.settings_slot());
    } catch (const std::exception& e) {
        kLogger.debug() << "undecodable settings query:" << e.what();
        return;
    }

    // There is no target field: the packet is unicast, so the destination
    // address is what identifies whose medium is being asked about.
    const auto found = m_slots.constFind(slotValue);
    const QByteArray settings = found != m_slots.constEnd() ? found->settings : QByteArray();
    m_pSocket->writeDatagram(buildSettingsResponse(m_deviceNumber,
                                     QString::fromLatin1(kVirtualCdjName),
                                     requester,
                                     static_cast<MediaSlot>(slotValue),
                                     settings),
            sender,
            kStatusPort);
    ++m_settingsQueriesAnswered;
    kLogger.info() << "answered settings query from" << sender.toString() << "for slot"
                   << slotValue << "-" << settings.size() << "bytes";
}

} // namespace server
} // namespace prolink
} // namespace mixxx
