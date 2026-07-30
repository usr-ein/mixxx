#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include "network/prolink/prolinkdefs.h"

class QTimer;
class QUdpSocket;

namespace mixxx {
namespace prolink {

/// Announces Mixxx on the Pro DJ Link network and claims a real player number.
///
/// **This is the only part of the feature that transmits on a DJ-Link port.**
/// Everything else — discovery, NFS, dbserver — is either passive or a
/// conversation we initiate with one peer, so a bug here is the one way this
/// code can disturb a rig that was working fine before Mixxx was plugged in.
///
/// It exists because a number cannot be borrowed. Every dbserver request carries
/// one in its descriptor and the player validates it: 1–4, belonging to a device
/// on the network, and not the deck being asked. Claiming another deck's number
/// half-works and fails in the worst possible way — the target accepts the
/// handshake and then silently ignores every request, because that number
/// already has a session with it. Two decks, one browsing the other, and the
/// covers never load with nothing in the log but a timeout.
///
/// The handshake, all broadcast, 300 ms apart:
///
///     3× hello(0a) → 3× claim_mac(00) → 3× claim_ip(02) → n× claim_number(04)
///     → keep_alive(06) every 2 s, forever
///
/// The stage-4 repeat count is three when we were first on the network and one
/// when we joined peers — not governed by the assignment mode, as the
/// pre-hardware research had it (C13).
///
/// **Shares the discovery socket rather than opening its own.** Replies to a
/// claim — conflicts, and mixer assignments — are unicast to port 50000, so we
/// must send *from* the port we are listening on. A second socket would mean
/// never hearing the answer, and silently losing every contest.
///
/// AUTO numbering is: watch for kPrescanMs, take the highest free number in
/// 1–4, back off to the next candidate if anyone objects. Highest-first leaves 1
/// and 2 — the decks a DJ reaches for — alone as long as possible.
///
/// Lives on the ProLink network thread, alongside ProLinkDiscovery.
class ProLinkVirtualCdj : public QObject {
    Q_OBJECT

  public:
    enum class State {
        Idle,
        /// Listening before claiming anything. Nothing is transmitted yet.
        Prescan,
        Hello,
        ClaimMac,
        ClaimIp,
        ClaimNumber,
        /// Number held, keep-alives running. Only now is our number usable.
        Active,
        /// Every candidate was contested, or there was nothing to announce on.
        Failed,
    };

    explicit ProLinkVirtualCdj(QUdpSocket* pSocket, QObject* parent = nullptr);
    ~ProLinkVirtualCdj() override;

    /// Begin the pre-scan. *peersAlreadyPresent* latches the "was I first here?"
    /// answer, which a real deck decides once at boot and holds for the session.
    ///
    /// Needs a local interface to announce on; without one — no CDJ has been
    /// seen yet, so we do not know which NIC faces the rig — it fails rather
    /// than guessing. Broadcasting DJ-Link onto the house LAN would be rude at
    /// best.
    void start(const QString& interfaceName, int peersAlreadyPresent);
    void stop();

    State state() const {
        return m_state;
    }
    /// Our claimed number, or 0 until the handshake completes. **Callers must
    /// check this rather than assuming**: a request sent under a number we have
    /// not finished claiming is exactly the failure this class exists to fix.
    int deviceNumber() const {
        return m_state == State::Active ? m_deviceNumber : 0;
    }
    QByteArray mac() const {
        return m_mac;
    }
    QHostAddress address() const {
        return m_address;
    }
    QString stateName() const;

    /// Offer a received datagram to the announcer. Returns true if it was one we
    /// acted on, purely for logging — discovery processes it either way.
    ///
    /// Two things matter here: somebody objecting to the number we are claiming,
    /// and somebody proposing a number we already hold. The second obliges us to
    /// answer with a conflict packet; a device that takes a number and never
    /// defends it loses it to the next player that boots.
    bool handleDatagram(const QByteArray& datagram, const QHostAddress& sender);

    /// Numbers seen in use by anyone, so the pre-scan has something to work
    /// from. Fed by discovery as devices appear.
    void noteNumberInUse(int number);
    /// Peer count for the keep-alive, which includes ourselves.
    void setPeerCount(int peers) {
        m_peerCount = peers;
    }

  signals:
    void stateChanged(int state, int deviceNumber, const QString& detail);

  private:
    void enter(State state, const QString& detail);
    void finishPrescan();
    /// Send the next handshake packet, or fall through to keep-alives.
    void advance();
    QByteArray buildStagePacket() const;
    void send(const QByteArray& datagram, const QHostAddress& to = QHostAddress());
    void sendKeepAlive();
    /// Give up the number under contest and try the next candidate.
    void backOff(int contested, const QHostAddress& challenger);
    /// Resolve our own MAC and IPv4 address on *interfaceName*.
    bool resolveInterface(const QString& interfaceName);

    QUdpSocket* const m_pSocket;
    QTimer* m_pStageTimer = nullptr;
    QTimer* m_pKeepAlive = nullptr;

    State m_state = State::Idle;
    int m_deviceNumber = 0;
    QList<int> m_candidates;
    QSet<int> m_numbersInUse;

    QByteArray m_mac;
    QHostAddress m_address;
    QHostAddress m_broadcast;
    QString m_interfaceName;

    /// Latched in start() and never revisited: it drives the stage-4 repeat
    /// count and keep-alive byte 0x25 for the whole session (F9, C13).
    bool m_firstOnNetwork = true;
    int m_peerCount = 1;

    int m_stage = 0;
    int m_iteration = 0;
};

} // namespace prolink
} // namespace mixxx
