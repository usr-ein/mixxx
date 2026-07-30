#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>

#include "network/prolink/prolinkdefs.h"

class QUdpSocket;

namespace mixxx {
namespace prolink {

/// Where one player is within its bar, right now.
///
/// Rebuilt from beat packets, which arrive **on** each beat rather than
/// periodically: a packet is the player saying "I am starting a beat now". So
/// the phase between packets is extrapolated from the tempo, and the arrival of
/// the next one re-anchors it. That is exactly how a CDJ's own sync works, and
/// it is why the estimate stays tight even at 4 packets a second.
struct BeatPhase {
    int deviceNumber = 0;
    /// 1-4, or 0 if the player is not on a rekordbox-analysed track.
    int beatInBar = 0;
    /// Effective BPM: the track's tempo with the pitch fader applied.
    double bpm = 0.0;
    /// Milliseconds since the beat packet landed.
    qint64 sinceBeatMs = 0;

    bool isValid() const {
        return deviceNumber > 0 && bpm > 0.0;
    }

    /// Position within the current beat, 0 at the beat and approaching 1 before
    /// the next. Clamped rather than wrapped: a player that has stopped sending
    /// beats should sit at the end of its beat, not spin.
    double beatPhase() const;
    /// Position within the four-beat bar, 0 on the downbeat.
    double barPhase() const;
};

/// Listens for beat packets on UDP 50001.
///
/// A player sends one on every beat, but **only while playing and only for a
/// rekordbox-analysed track**. A mixer sends them continuously and acts as a
/// metronome when nothing else is counting. Silence therefore means "not
/// playing", which is information rather than a failure.
///
/// Unlike the status port, 50001 is broadcast, so this works without announcing
/// — though in practice we announce anyway by the time it matters.
///
/// Lives on the ProLink network thread.
class ProLinkBeatListener : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkBeatListener(QObject* parent = nullptr);

    bool start();
    void stop();

    /// The phase of *deviceNumber*, or an invalid BeatPhase if it has not sent a
    /// beat packet recently.
    BeatPhase phaseFor(int deviceNumber) const;

  signals:
    /// A player started a beat. Emitted per packet, so about four times a second
    /// per playing deck.
    void beat(const mixxx::prolink::BeatPhase& phase);

  private slots:
    void readPendingDatagrams();

  private:
    QUdpSocket* m_pSocket = nullptr;
    struct Entry {
        BeatPhase phase;
        QElapsedTimer since;
    };
    QHash<int, Entry> m_phases;
};

/// Decode a type-0x28 beat packet. False if the datagram is not one.
bool parseBeatPacket(const QByteArray& datagram, BeatPhase* pPhase);

} // namespace prolink
} // namespace mixxx
