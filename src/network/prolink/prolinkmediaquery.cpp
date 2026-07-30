#include "network/prolink/prolinkmediaquery.h"

#include <QNetworkDatagram>
#include <QUdpSocket>

#include "moc_prolinkmediaquery.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkMediaQuery");

/// Type bytes at offset 0x0a, on the 50002 layout.
constexpr quint8 kTypeMediaQuery = 0x05;
constexpr quint8 kTypeMediaResponse = 0x06;

/// The 50002 header is *not* the 50000 one: it puts the 20-byte name at
/// 0x0b–0x1e with a structural 0x01 at 0x1f, one byte earlier than the discovery
/// header does (C14). Reusing the discovery offsets here reads the name one byte
/// out and everything after it shifts.
constexpr int kStatusNameOffset = 0x0B;
constexpr int kStatusNameLength = 20;
constexpr int kStatusStructuralOne = 0x1F;
constexpr int kStatusSenderNumber = 0x21;

// Query body.
constexpr int kQueryLength = 0x30;
constexpr int kQueryLengthField = 0x22;
constexpr int kQueryRequesterIp = 0x24;
constexpr int kQueryTargetDevice = 0x28;
constexpr int kQuerySlot = 0x2C;
/// Bytes following 0x24: the IP, the target and the slot.
constexpr quint16 kQueryBodyLength = 0x0C;

// Response body.
constexpr int kResponseMinLength = 0xB0;
constexpr int kResponseDevice = 0x24;
constexpr int kResponseSlot = 0x28;
constexpr int kResponseName = 0x2C;
constexpr int kResponseNameLength = 0x40;
constexpr int kResponseTrackCount = 0xA4;
constexpr int kResponsePlaylistCount = 0xAC;

// CDJ status (type 0x0a), for working out who the tempo master is.
constexpr quint8 kTypeCdjStatus = 0x0A;
constexpr int kStatusMinLength = 0x76;
constexpr int kStatusDevice = 0x21;
/// "Master meaningful": 00 not master, 01 master on a rekordbox track, 02
/// master on a track with no usable tempo. Anything non-zero is the master.
constexpr int kStatusMasterMeaningful = 0x9E;

quint32 readU32(const QByteArray& data, int offset) {
    return (static_cast<quint32>(static_cast<quint8>(data.at(offset))) << 24) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 8) |
            static_cast<quint32>(static_cast<quint8>(data.at(offset + 3)));
}

void writeU32(QByteArray* pOut, int offset, quint32 value) {
    (*pOut)[offset] = static_cast<char>((value >> 24) & 0xFF);
    (*pOut)[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
    (*pOut)[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
    (*pOut)[offset + 3] = static_cast<char>(value & 0xFF);
}
} // namespace

namespace mixxx {
namespace prolink {

QByteArray buildMediaQuery(const QString& name,
        int requesterNumber,
        const QHostAddress& requesterIp,
        int targetDeviceNumber,
        MediaSlot slot) {
    QByteArray out(kQueryLength, '\0');
    out.replace(0, kMagicLength, QByteArray::fromRawData(kMagic, kMagicLength));
    out[kOffsetType] = static_cast<char>(kTypeMediaQuery);

    const QByteArray ascii = name.toLatin1().left(kStatusNameLength);
    out.replace(kStatusNameOffset, ascii.size(), ascii);
    out[kStatusStructuralOne] = 0x01;
    // 0x20 stays zero.
    out[kStatusSenderNumber] = static_cast<char>(requesterNumber);
    out[kQueryLengthField] = static_cast<char>((kQueryBodyLength >> 8) & 0xFF);
    out[kQueryLengthField + 1] = static_cast<char>(kQueryBodyLength & 0xFF);

    writeU32(&out, kQueryRequesterIp, requesterIp.toIPv4Address());
    writeU32(&out, kQueryTargetDevice, static_cast<quint32>(targetDeviceNumber));
    writeU32(&out, kQuerySlot, static_cast<quint32>(slot));
    return out;
}

bool parseMediaResponse(const QByteArray& datagram,
        int* pDeviceNumber,
        MediaSlot* pSlot,
        MediaInfo* pInfo) {
    if (datagram.size() < kResponseMinLength) {
        return false;
    }
    if (!datagram.startsWith(QByteArray::fromRawData(kMagic, kMagicLength))) {
        return false;
    }
    if (static_cast<quint8>(datagram.at(kOffsetType)) != kTypeMediaResponse) {
        return false;
    }

    *pDeviceNumber = static_cast<int>(readU32(datagram, kResponseDevice));
    *pSlot = static_cast<MediaSlot>(readU32(datagram, kResponseSlot) & 0xFF);

    // UTF-16 big-endian, NUL-padded to a fixed 64 bytes. Decoded by hand rather
    // than through a codec because the padding is not a terminator: the field is
    // always 64 bytes and the name simply stops.
    QString name;
    for (int i = 0; i + 1 < kResponseNameLength; i += 2) {
        const char16_t unit = static_cast<char16_t>(
                (static_cast<quint8>(datagram.at(kResponseName + i)) << 8) |
                static_cast<quint8>(datagram.at(kResponseName + i + 1)));
        if (unit == 0) {
            break;
        }
        name.append(QChar(unit));
    }
    pInfo->name = name;
    pInfo->trackCount = readU32(datagram, kResponseTrackCount);
    pInfo->playlistCount = readU32(datagram, kResponsePlaylistCount);
    return true;
}

ProLinkMediaQuery::ProLinkMediaQuery(QObject* parent)
        : QObject(parent) {
}

bool ProLinkMediaQuery::start() {
    if (m_pSocket && m_pSocket->state() == QAbstractSocket::BoundState) {
        return true;
    }
    if (!m_pSocket) {
        m_pSocket = new QUdpSocket(this);
        connect(m_pSocket,
                &QUdpSocket::readyRead,
                this,
                &ProLinkMediaQuery::readPendingDatagrams);
    }
    const bool bound = m_pSocket->bind(QHostAddress::AnyIPv4,
            kStatusPort,
            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound) {
        kLogger.warning() << "could not bind UDP" << kStatusPort << ":"
                          << m_pSocket->errorString();
        return false;
    }
    kLogger.info() << "listening on UDP" << kStatusPort;
    return true;
}

void ProLinkMediaQuery::stop() {
    if (m_pSocket) {
        m_pSocket->close();
    }
}

void ProLinkMediaQuery::query(const QHostAddress& player,
        const QHostAddress& localAddress,
        int targetDeviceNumber,
        MediaSlot slot) {
    if (!m_pSocket || m_requesterNumber == 0) {
        // No number means no announcement, and an unannounced host is not
        // answered at all on this port.
        return;
    }
    const QByteArray datagram = buildMediaQuery(QString::fromLatin1(kVirtualCdjName),
            m_requesterNumber,
            localAddress,
            targetDeviceNumber,
            slot);
    if (m_pSocket->writeDatagram(datagram, player, kStatusPort) < 0) {
        kLogger.warning() << "media query to" << player << "failed:"
                          << m_pSocket->errorString();
    }
}

void ProLinkMediaQuery::readPendingDatagrams() {
    while (m_pSocket && m_pSocket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_pSocket->receiveDatagram();
        int deviceNumber = 0;
        MediaSlot slot = MediaSlot::Empty;
        MediaInfo info;
        const QByteArray data = datagram.data();

        // Ordinary status, four a second per deck now that we are announced.
        // Watched only for who is master, which is published here and nowhere
        // a passive listener can see (F21).
        if (data.size() > kStatusMasterMeaningful &&
                data.startsWith(QByteArray::fromRawData(kMagic, kMagicLength)) &&
                static_cast<quint8>(data.at(kOffsetType)) == kTypeCdjStatus &&
                data.size() >= kStatusMinLength) {
            const int sender = static_cast<quint8>(data.at(kStatusDevice));
            const bool isMaster =
                    static_cast<quint8>(data.at(kStatusMasterMeaningful)) != 0;
            if (isMaster && sender != m_masterDevice) {
                m_masterDevice = sender;
                kLogger.info() << "tempo master is now player" << sender;
                emit masterChanged(sender);
            } else if (!isMaster && sender == m_masterDevice) {
                // It gave up master and nobody has claimed it in this packet.
                m_masterDevice = 0;
                kLogger.info() << "player" << sender << "gave up tempo master";
                emit masterChanged(0);
            }
            continue;
        }

        if (!parseMediaResponse(data, &deviceNumber, &slot, &info)) {
            continue;
        }
        if (info.isOccupied()) {
            kLogger.info() << "player" << deviceNumber << "slot" << static_cast<int>(slot)
                           << "holds" << (info.name.isEmpty()
                                                     ? QStringLiteral("an unnamed volume")
                                                     : info.name)
                           << "-" << info.trackCount << "tracks,"
                           << info.playlistCount << "playlists";
        } else {
            kLogger.info() << "player" << deviceNumber << "slot"
                           << static_cast<int>(slot) << "is empty";
        }
        emit mediaInfoReceived(datagram.senderAddress(), slot, info);
    }
}

} // namespace prolink
} // namespace mixxx
