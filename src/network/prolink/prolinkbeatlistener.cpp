#include "network/prolink/prolinkbeatlistener.h"

#include <QNetworkDatagram>
#include <QUdpSocket>

#include <algorithm>

#include "moc_prolinkbeatlistener.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkBeat");

/// Byte 0x0a of a beat packet.
constexpr quint8 kTypeBeat = 0x28;
/// The whole packet is 0x60 bytes; refuse anything shorter than the last field
/// we read.
constexpr int kMinBeatPacketLength = 0x60;

constexpr int kOffsetDevice = 0x21;
constexpr int kOffsetPitch = 0x54;
constexpr int kOffsetBpm = 0x5A;
constexpr int kOffsetBeatInBar = 0x5C;

/// The pitch field is a 32-bit fixed-point multiplier with 0x00100000 meaning
/// 0%, i.e. ×1. Timing values in the packet are reported *as if at 0% pitch*, so
/// the tempo has to be scaled by this to be the tempo actually playing.
constexpr double kPitchUnity = 0x100000;

/// A player that has said nothing for this long is not playing: beat packets
/// stop the moment the platter does. Three beats at 60 BPM, so even a very slow
/// track keeps its phase alive between packets.
constexpr qint64 kBeatStaleMs = 3000;

quint32 readU32(const QByteArray& data, int offset) {
    return (static_cast<quint32>(static_cast<quint8>(data.at(offset))) << 24) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 8) |
            static_cast<quint32>(static_cast<quint8>(data.at(offset + 3)));
}
} // namespace

namespace mixxx {
namespace prolink {

double BeatPhase::beatPhase() const {
    if (bpm <= 0.0) {
        return 0.0;
    }
    const double beatMs = 60000.0 / bpm;
    return std::clamp(static_cast<double>(sinceBeatMs) / beatMs, 0.0, 1.0);
}

double BeatPhase::barPhase() const {
    // beatInBar is 1-4; 0 means the player has no rekordbox track and therefore
    // no bar to be in, in which case the beat alone is all we can show.
    const int index = beatInBar > 0 ? beatInBar - 1 : 0;
    return (index + beatPhase()) / 4.0;
}

bool parseBeatPacket(const QByteArray& datagram, BeatPhase* pPhase) {
    if (datagram.size() < kMinBeatPacketLength) {
        return false;
    }
    if (!datagram.startsWith(QByteArray::fromRawData(kMagic, kMagicLength))) {
        return false;
    }
    if (static_cast<quint8>(datagram.at(kOffsetType)) != kTypeBeat) {
        return false;
    }

    pPhase->deviceNumber = static_cast<quint8>(datagram.at(kOffsetDevice));
    pPhase->beatInBar = static_cast<quint8>(datagram.at(kOffsetBeatInBar));

    const double rawBpm = ((static_cast<quint8>(datagram.at(kOffsetBpm)) << 8) |
                                  static_cast<quint8>(datagram.at(kOffsetBpm + 1))) /
            100.0;
    const double pitch = readU32(datagram, kOffsetPitch) / kPitchUnity;
    pPhase->bpm = rawBpm * pitch;
    pPhase->sinceBeatMs = 0;
    return pPhase->deviceNumber > 0;
}

ProLinkBeatListener::ProLinkBeatListener(QObject* parent)
        : QObject(parent) {
}

bool ProLinkBeatListener::start() {
    if (m_pSocket && m_pSocket->state() == QAbstractSocket::BoundState) {
        return true;
    }
    if (!m_pSocket) {
        m_pSocket = new QUdpSocket(this);
        connect(m_pSocket,
                &QUdpSocket::readyRead,
                this,
                &ProLinkBeatListener::readPendingDatagrams);
    }
    const bool bound = m_pSocket->bind(QHostAddress::AnyIPv4,
            kBeatPort,
            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound) {
        kLogger.warning() << "could not bind UDP" << kBeatPort << ":"
                          << m_pSocket->errorString();
        return false;
    }
    kLogger.info() << "listening on UDP" << kBeatPort;
    return true;
}

void ProLinkBeatListener::stop() {
    if (m_pSocket) {
        m_pSocket->close();
    }
    m_phases.clear();
}

BeatPhase ProLinkBeatListener::phaseFor(int deviceNumber) const {
    const auto entry = m_phases.constFind(deviceNumber);
    if (entry == m_phases.constEnd()) {
        return BeatPhase();
    }
    const qint64 elapsed = entry->since.elapsed();
    if (elapsed > kBeatStaleMs) {
        return BeatPhase();
    }
    BeatPhase phase = entry->phase;
    phase.sinceBeatMs = elapsed;
    return phase;
}

void ProLinkBeatListener::readPendingDatagrams() {
    while (m_pSocket && m_pSocket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_pSocket->receiveDatagram();
        BeatPhase phase;
        if (!parseBeatPacket(datagram.data(), &phase)) {
            continue;
        }
        Entry& entry = m_phases[phase.deviceNumber];
        entry.phase = phase;
        entry.since.start();
        emit beat(phase);
    }
}

} // namespace prolink
} // namespace mixxx
