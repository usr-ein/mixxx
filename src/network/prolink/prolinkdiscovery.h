#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include "network/prolink/prolinkdevice.h"
#include "network/prolink/prolinkmediaquery.h"

class QUdpSocket;
class QTimer;

namespace mixxx {
namespace prolink {

class ProLinkVirtualCdj;
class ProLinkMediaQuery;
class ProLinkBeatListener;
struct MediaInfo;

namespace server {
class ProLinkServer;
}

/// Listens on UDP 50000 and maintains the table of devices on the network.
///
/// Listening is passive: discovery works off broadcasts every player already
/// sends, so the table costs nothing and risks nothing. Transmitting is the
/// separate concern of ProLinkVirtualCdj, which this class owns and lends its
/// socket to — replies to a claim are unicast to port 50000, so the announcer
/// must send from the port we are listening on or it never hears the answer.
///
/// Announcing is not optional for browsing another player's media: dbserver
/// validates the device number in every request, and a borrowed one fails
/// silently (the target accepts the handshake and then ignores everything).
///
/// Lives on the network thread. Everything it emits is a value type, so the
/// signals cross to the GUI thread as plain copies.
class ProLinkDiscovery : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkDiscovery(QObject* parent = nullptr);
    ~ProLinkDiscovery() override = default;

    /// Bind the socket and start listening. Must be called on the thread this
    /// object lives on -- the socket is created here rather than in the
    /// constructor precisely so that it belongs to the right thread.
    ///
    /// Returns false if the port could not be bound, which is a normal thing to
    /// happen: rekordbox, prolink-tools or a second Mixxx may already hold it.
    /// The caller reports that to the user rather than treating it as fatal.
    bool start();
    void stop();

    bool isListening() const;
    /// Why the bind failed, for the preferences status area. Empty on success.
    QString lastError() const {
        return m_lastError;
    }

    QList<ProLinkDevice> devices() const;
    int deviceCount() const {
        return m_devices.size();
    }

    /// Drop every device that is currently stale, without waiting out the
    /// removal grace period. Bound to the `[ProLink],refresh` control.
    void forgetStaleDevices();

    /// Start claiming a player number, on the interface the first player was
    /// seen on. A no-op if already announcing, or if no player has been seen
    /// yet — we will not broadcast DJ-Link onto a network we have no evidence
    /// is the rig's.
    void startAnnouncing();
    /// Our claimed number, or 0 if we have not (yet) got one.
    int announcedNumber() const;
    QString announceStatus() const;

    /// Ask every online player what is in both of its slots. A no-op until we
    /// have a number: a player answers on UDP 50002 only to peers that have
    /// announced themselves (F21).
    ///
    /// *force* re-asks slots already answered, for the case where the DJ swaps
    /// media without the device going anywhere.
    void queryAllMedia(bool force = false);

    /// Where the tempo master is within its bar right now, for the phase meter.
    /// Bar phase 0..1, or -1 when there is no master or it is not playing.
    double masterBarPhase() const;
    /// The master's effective BPM, or 0.
    double masterBpm() const;
    int masterDevice() const;

    /// What we are serving to other players, for the ProLink page. Empty until
    /// we have announced, since the serve side comes up with the announcement.
    QString serverSummary() const;

  signals:
    /// The announcer changed state: claiming, active with a number, or failed.
    void announceStateChanged(int state, int deviceNumber, const QString& detail);
    /// A player told us what is in one of its slots. Keyed on MAC, because the
    /// device number in the response can be reassigned.
    void mediaInfoFound(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const mixxx::prolink::MediaInfo& info);
    /// A device we had not seen before, or one that had been removed and is back.
    void deviceFound(const mixxx::prolink::ProLinkDevice& device);
    /// An existing device whose number, address or online state changed. Not
    /// emitted for a plain keep-alive, which would be four signals a second per
    /// deck for no new information.
    void deviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void deviceLost(const QByteArray& mac);

  private slots:
    void readPendingDatagrams();
    void reapDevices();

  private:
    /// Which local interface an address belongs to, for the multi-homed case.
    static QString interfaceForAddress(const QHostAddress& address);
    /// Hand the serve side the current peer list. Status is unicast per peer, so
    /// a deck that is not in this list is a deck that never learns we hold media.
    void updateServerPeers();

    /// Raw pointers parented to `this`, not parented_ptr: both are created
    /// lazily in start(), and parented_ptr is deliberately non-assignable.
    /// Ownership is Qt's -- they die with this object, on its own thread.
    QUdpSocket* m_pSocket = nullptr;
    QTimer* m_pReaper = nullptr;
    /// Created with the socket, started separately. Owned here because it needs
    /// that socket and must not outlive it.
    ProLinkVirtualCdj* m_pVirtualCdj = nullptr;
    ProLinkMediaQuery* m_pMediaQuery = nullptr;
    ProLinkBeatListener* m_pBeatListener = nullptr;
    /// The serve side. Created once we announce, because everything it does
    /// needs a player number and its socket is the media query's.
    server::ProLinkServer* m_pServer = nullptr;
    /// Retries the media query. One round is not enough: a player that is busy,
    /// or that joined after we claimed our number, simply does not answer, and
    /// there is no reply that means "empty" to distinguish from a lost one.
    QTimer* m_pMediaRetry = nullptr;
    /// mac|slot -> attempts made. An entry is removed once answered, and a slot
    /// that never answers is given up on after kMediaQueryAttempts, so an empty
    /// slot costs a handful of datagrams rather than two every few seconds for
    /// the rest of the session.
    QHash<QString, int> m_mediaQueryAttempts;
    /// Keyed on MAC. See the note in ProLinkDevice about why not on number.
    QHash<QByteArray, ProLinkDevice> m_devices;
    /// Which devices we have already told the world are stale, so the "went
    /// offline" signal fires once rather than once per reaper tick.
    QSet<QByteArray> m_reportedStale;
    QString m_lastError;
};

} // namespace prolink
} // namespace mixxx
