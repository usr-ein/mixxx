#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include "network/prolink/prolinkdefs.h"
#include "network/prolink/server/prolinkservestatus.h"

class QTimer;
class QUdpSocket;

namespace mixxx {
namespace prolink {
namespace server {

/// What we tell a player is in one of our slots.
struct SlotAdvertisement {
    bool occupied = false;
    QString volumeName;
    quint32 trackCount = 0;
    quint32 playlistCount = 0;
    /// The medium's `MYSETTING.DAT` payload, answered inline to a type-0x35
    /// query. Empty is a legitimate answer — a medium with no saved settings.
    QByteArray settings;
};

/// The serve half of UDP 50002: says we exist and that our slots hold media.
///
/// **Announcing on 50000 is not enough.** Keep-alives only say a device is
/// present; media presence is published in status packets and *nowhere else*
/// (F20), and those are unicast to peers that have announced themselves (F21).
/// A device that never sends them looks, to a CDJ, like a player with two empty
/// slots — which is a player with nothing worth browsing.
///
/// Nor is status alone enough. A deck that had otherwise fully accepted us — 927
/// status packets unicast to us over four minutes, a completed portmap and MNT
/// against our NFS server — still refused to list us as a source, because it
/// asks "what is in slot N?" with a type-0x05 query and we never answered (F24).
/// Both halves are needed, which is why they live in one class.
///
/// A status packet is 284 bytes of which, across 749 consecutive packets from an
/// idle CDJ-2000nexus, only **six** ever changed. So rather than construct one
/// field by field from a specification full of unknowns, we start from a real
/// packet and substitute the fields we understand; everything we cannot name is
/// preserved exactly as a deck sends it.
///
/// Shares the 50002 socket rather than opening its own, for the same reason the
/// virtual CDJ shares discovery's: replies have to come from the port we are
/// listening on.
class ProLinkStatusServer : public QObject {
    Q_OBJECT

  public:
    ProLinkStatusServer(QUdpSocket* pSocket, QObject* parent = nullptr);
    ~ProLinkStatusServer() override;

    /// Begin emitting status. Does nothing until a device number is set: an
    /// unannounced device has no number to put in the packet, and a deck would
    /// have no idea who it was from.
    void start();
    void stop();

    void setDeviceNumber(int number);
    void setPeers(const QList<QHostAddress>& peers) {
        m_peers = peers;
    }
    void setSlot(MediaSlot slot, const SlotAdvertisement& advertisement);
    void clearSlot(MediaSlot slot);

    /// Offer a received datagram. True if it was a query we answered, so the
    /// caller knows not to treat it as something else.
    ///
    /// A peer's own status packet returns **false** even though we read it:
    /// answering is not the only reason to look at a datagram, and the caller
    /// still needs it for tempo-master detection. What we take from it is who
    /// has one of our tracks loaded — see ServeConsumer.
    bool handleDatagram(const QByteArray& datagram, const QHostAddress& sender);

    /// Players currently holding a track of ours, newest information first.
    /// Track ids are unresolved here; ProLinkServer names them, since it owns
    /// the libraries.
    QList<ServeConsumer> consumers() const;

    int mediaQueriesAnswered() const {
        return m_mediaQueriesAnswered;
    }
    int settingsQueriesAnswered() const {
        return m_settingsQueriesAnswered;
    }

    /// The packet we emit. Exposed so it can be diffed byte for byte against a
    /// real deck's, which is the acceptance test for impersonation.
    QByteArray buildStatus() const;

  signals:
    /// The set of players holding our tracks changed — someone loaded, ejected,
    /// or started or stopped playing. Not emitted for the four status packets a
    /// second that say nothing new.
    void consumersChanged();

  private slots:
    void emitStatus();

  private:
    void answerMediaQuery(const QByteArray& datagram, const QHostAddress& sender);
    void answerSettingsQuery(const QByteArray& datagram, const QHostAddress& sender);
    /// Read a peer's status packet for whether it is holding a track of ours.
    void notePeerStatus(const QByteArray& datagram, const QHostAddress& sender);

    QUdpSocket* const m_pSocket;
    QTimer* m_pTimer = nullptr;

    int m_deviceNumber = 0;
    QList<QHostAddress> m_peers;
    QMap<int, SlotAdvertisement> m_slots;
    mutable quint32 m_packetCounter = 0;

    int m_mediaQueriesAnswered = 0;
    int m_settingsQueriesAnswered = 0;

    /// Keyed on the consumer's device number.
    QMap<int, ServeConsumer> m_consumers;
};

/// Describe one of our slots, in reply to a type-0x05 query.
///
/// The counts are what a player shows in its Link Info panel, so they must be
/// the real ones: a deck told there are no tracks has no reason to offer the
/// medium for browsing.
QByteArray buildMediaResponse(int deviceNumber,
        const QString& deviceName,
        MediaSlot slot,
        const SlotAdvertisement& advertisement);

/// A type-0x36 reply carrying *settings* verbatim.
QByteArray buildSettingsResponse(int deviceNumber,
        const QString& deviceName,
        int requester,
        MediaSlot slot,
        const QByteArray& settings);

} // namespace server
} // namespace prolink
} // namespace mixxx
