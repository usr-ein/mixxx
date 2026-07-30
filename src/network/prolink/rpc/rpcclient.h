#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <functional>

#include "network/prolink/rpc/rpcmessage.h"

class QUdpSocket;
class QTimer;

namespace mixxx {
namespace prolink {
namespace rpc {

/// An ONC RPC v2 client over UDP, for one peer.
///
/// Lives on the ProLink network thread. Fully asynchronous — no call blocks, and
/// there is no blocking recv anywhere. Results arrive through the callback each
/// call is given.
///
/// **Why hand-rolled rather than libnfs or libtirpc.** Pioneer encodes mount
/// paths and every `LOOKUP` filename as length-prefixed UTF-16LE where the
/// standard says ASCII, so libnfs would need its wire encoder patched. libtirpc
/// is effectively Linux-only with a blocking API that fits a Qt event loop
/// badly. And neither helps on the side that matters most later: serving, where
/// we are the RPC *server*, which is not what either library is for. Against
/// that, this is a few hundred lines for six procedures.
///
/// **Retries.** UDP, so datagrams are lost and there is no stream to notice it.
/// Every outstanding call is re-sent on a fixed tick until it is answered or its
/// attempts run out. One timer for all calls rather than a timer per call: a
/// whole-file fetch has several reads in flight at once, and a QTimer each would
/// be both wasteful and less predictable.
class RpcClient : public QObject {
    Q_OBJECT

  public:
    /// A reply, or a failure. `reply.isOk()` distinguishes them; on timeout the
    /// reply is default-constructed and `timedOut` is true.
    struct Result {
        Reply reply;
        bool timedOut = false;
        bool isOk() const {
            return !timedOut && reply.isOk();
        }
        QString errorString() const {
            return timedOut ? QStringLiteral("timed out") : reply.errorString();
        }
    };
    using Callback = std::function<void(const Result&)>;

    /// *localAddress* is the source address to bind.
    ///
    /// Not optional in practice and not cosmetic: the deck Pi is multi-homed,
    /// and on a link-local network the kernel's route lookup can pick the wrong
    /// interface, after which every call times out with nothing to indicate why.
    /// Pass the address of the interface the peer's announcements arrived on.
    RpcClient(const QHostAddress& peer,
            const QHostAddress& localAddress,
            QObject* parent = nullptr);
    ~RpcClient() override;

    bool open();
    void close();
    bool isOpen() const;

    /// Issue a call against *port* on the peer. Returns false if the datagram
    /// could not even be sent, in which case *callback* is not invoked.
    bool call(quint16 port,
            quint32 program,
            quint32 version,
            quint32 procedure,
            const QByteArray& arguments,
            Callback callback);

    /// Abandon every outstanding call, invoking each callback with a timeout.
    /// Used when a peer disappears, so nothing is left waiting forever.
    ///
    /// **Never called during destruction.** A callback routinely captures the
    /// objects that own this client, and by the time ~RpcClient runs those are
    /// already being torn down -- invoking one then is a use-after-free. The
    /// destructor drops pending calls silently instead; a caller that needs its
    /// callbacks to fire must call this while everything is still alive.
    void abortAll();

    int outstandingCalls() const {
        return m_pending.size();
    }

    void setTimeout(int intervalMs, int attempts) {
        m_retryIntervalMs = intervalMs;
        m_maxAttempts = attempts;
    }

  private slots:
    void readReplies();
    void onRetryTick();

  private:
    struct Pending {
        QByteArray datagram;
        quint16 port = 0;
        int attempts = 0;
        Callback callback;
    };

    const QHostAddress m_peer;
    const QHostAddress m_localAddress;

    QUdpSocket* m_pSocket = nullptr;
    QTimer* m_pRetryTimer = nullptr;

    /// Keyed on XID, which is what correlates a reply to its call. Ours are
    /// drawn from a counter with a random start, so a stale reply from a previous
    /// run of Mixxx cannot be mistaken for an answer to a current call.
    QHash<quint32, Pending> m_pending;
    quint32 m_nextXid;

    int m_retryIntervalMs = 250;
    int m_maxAttempts = 8;
};

} // namespace rpc
} // namespace prolink
} // namespace mixxx
