#include "network/prolink/rpc/rpcclient.h"

#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>

#include "moc_rpcclient.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkRpc");
} // namespace

namespace mixxx {
namespace prolink {
namespace rpc {

RpcClient::RpcClient(const QHostAddress& peer,
        const QHostAddress& localAddress,
        QObject* parent)
        : QObject(parent),
          m_peer(peer),
          m_localAddress(localAddress),
          // Random start rather than 1, so a reply left over from a previous run
          // of Mixxx cannot be mistaken for an answer to a current call. Real
          // players do start at 1, but they also reboot with their peers.
          m_nextXid(QRandomGenerator::global()->generate()) {
}

RpcClient::~RpcClient() {
    // Deliberately not close(), which would abort outstanding calls *with*
    // their callbacks. Those callbacks capture the objects that own us -- the
    // NFS client, the transfer, the service -- and during our destruction at
    // least one of them is already partly gone. Dropping them silently is the
    // only safe thing to do here.
    if (m_pRetryTimer) {
        m_pRetryTimer->stop();
    }
    m_pending.clear();
    if (m_pSocket) {
        m_pSocket->close();
    }
}

bool RpcClient::open() {
    if (isOpen()) {
        return true;
    }
    if (!m_pSocket) {
        m_pSocket = new QUdpSocket(this);
        connect(m_pSocket, &QUdpSocket::readyRead, this, &RpcClient::readReplies);
    }

    // Bind an ephemeral port on the interface facing the peer. Deliberately not
    // a reserved source port below 1024: a CDJ's export is open to the whole
    // link-local subnet, so there is nothing to gain, and binding one would need
    // privilege we do not want to require for the consume side.
    if (!m_pSocket->bind(m_localAddress, 0)) {
        kLogger.warning() << "could not bind an RPC socket on" << m_localAddress
                          << ":" << m_pSocket->errorString();
        return false;
    }

    if (!m_pRetryTimer) {
        m_pRetryTimer = new QTimer(this);
        connect(m_pRetryTimer, &QTimer::timeout, this, &RpcClient::onRetryTick);
    }
    m_pRetryTimer->setInterval(m_retryIntervalMs);
    return true;
}

void RpcClient::close() {
    if (m_pRetryTimer) {
        m_pRetryTimer->stop();
    }
    // Fail anything outstanding *before* closing, so no caller is left holding a
    // callback that will never fire.
    abortAll();
    if (m_pSocket) {
        m_pSocket->close();
    }
}

bool RpcClient::isOpen() const {
    return m_pSocket && m_pSocket->state() == QAbstractSocket::BoundState;
}

bool RpcClient::call(quint16 port,
        quint32 program,
        quint32 version,
        quint32 procedure,
        const QByteArray& arguments,
        Callback callback) {
    if (!isOpen() && !open()) {
        return false;
    }

    AuthUnix credentials;
    // Fresh nonce per call. 7917 distinct stamps across ~8500 real calls in our
    // captures say that is what a player does (C8).
    credentials.stamp = randomStamp();

    const quint32 xid = m_nextXid++;
    Pending pending;
    pending.datagram = buildCall(xid, program, version, procedure, credentials, arguments);
    pending.port = port;
    pending.attempts = 1;
    pending.callback = std::move(callback);

    if (m_pSocket->writeDatagram(pending.datagram, m_peer, port) < 0) {
        kLogger.warning() << "could not send RPC call to" << m_peer << port << ":"
                          << m_pSocket->errorString();
        return false;
    }

    m_pending.insert(xid, std::move(pending));
    if (!m_pRetryTimer->isActive()) {
        m_pRetryTimer->start();
    }
    return true;
}

void RpcClient::readReplies() {
    while (m_pSocket && m_pSocket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_pSocket->receiveDatagram();

        Reply reply;
        if (!parseReply(datagram.data(), &reply)) {
            kLogger.debug() << "discarding" << datagram.data().size()
                            << "bytes that are not an RPC reply";
            continue;
        }

        auto pending = m_pending.find(reply.xid);
        if (pending == m_pending.end()) {
            // A duplicate answer to a call we already retired, which is normal:
            // we retry on a timer, so a slow peer can answer twice. Dropping it
            // is correct; invoking the callback again would not be.
            continue;
        }
        const Callback callback = pending->callback;
        m_pending.erase(pending);
        if (m_pending.isEmpty() && m_pRetryTimer) {
            m_pRetryTimer->stop();
        }

        // Called last, and after the entry is removed, because a callback
        // routinely issues the next call in a chain -- LOOKUP then READ -- and
        // must be free to mutate m_pending while doing so.
        if (callback) {
            Result result;
            result.reply = reply;
            callback(result);
        }
    }
}

void RpcClient::onRetryTick() {
    if (m_pending.isEmpty()) {
        if (m_pRetryTimer) {
            m_pRetryTimer->stop();
        }
        return;
    }

    // Collect first, act second: a callback may insert or erase entries, and
    // mutating the hash while iterating it would invalidate the iterator.
    QList<quint32> exhausted;
    QList<quint32> toResend;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (it->attempts >= m_maxAttempts) {
            exhausted.append(it.key());
        } else {
            toResend.append(it.key());
        }
    }

    for (const quint32 xid : toResend) {
        auto pending = m_pending.find(xid);
        if (pending == m_pending.end()) {
            continue;
        }
        pending->attempts++;
        m_pSocket->writeDatagram(pending->datagram, m_peer, pending->port);
    }

    for (const quint32 xid : exhausted) {
        auto pending = m_pending.find(xid);
        if (pending == m_pending.end()) {
            continue;
        }
        const Callback callback = pending->callback;
        m_pending.erase(pending);
        kLogger.debug() << "RPC call" << xid << "to" << m_peer << "gave up after"
                        << m_maxAttempts << "attempts";
        if (callback) {
            Result result;
            result.timedOut = true;
            callback(result);
        }
    }

    if (m_pending.isEmpty() && m_pRetryTimer) {
        m_pRetryTimer->stop();
    }
}

void RpcClient::abortAll() {
    // Swapped out first so callbacks cannot see a half-emptied table, and so one
    // that issues a fresh call does not have it immediately aborted.
    QHash<quint32, Pending> pending;
    pending.swap(m_pending);
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        if (it->callback) {
            Result result;
            result.timedOut = true;
            it->callback(result);
        }
    }
}

} // namespace rpc
} // namespace prolink
} // namespace mixxx
