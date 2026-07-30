#include "network/prolink/dbserver/dbserverclient.h"

#include <QTcpSocket>
#include <QTimer>

#include "moc_dbserverclient.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkDbServer");

/// How long to wait for a connection, a handshake or one reply.
///
/// Generous by network standards and deliberately so: the player answers these
/// off the same processor that is decoding audio, and a deck mid-track has been
/// seen to take seconds. Short enough that a wedged connection is noticed within
/// one browse rather than stalling the queue behind it.
constexpr int kRequestTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 5000;

/// Give up on a player after this many consecutive connection failures.
constexpr int kMaxConsecutiveFailures = 3;

/// A guard on the receive buffer. The largest thing this client asks for is one
/// cover image; a player that keeps talking past that is not sending something
/// we want to keep accumulating.
constexpr int kMaxBufferBytes = 8 * 1024 * 1024;
} // namespace

namespace mixxx {
namespace prolink {
namespace dbserver {

DbServerClient::DbServerClient(const QHostAddress& peer,
        const QHostAddress& localAddress,
        int requesterNumber,
        QObject* parent)
        : QObject(parent),
          m_peer(peer),
          m_localAddress(localAddress),
          m_requesterNumber(requesterNumber) {
    m_pTimeout = new QTimer(this);
    m_pTimeout->setSingleShot(true);
    connect(m_pTimeout, &QTimer::timeout, this, [this]() {
        // A timeout is not recoverable in the way a failed request is. The
        // stream carries no length prefix, so a reply that arrives after we have
        // stopped waiting for it would be parsed as the *next* request's reply,
        // and every answer after that would be one out. Dropping the connection
        // is the only way to be sure of what we are reading.
        kLogger.warning() << m_peer.toString() << "timed out in state"
                          << static_cast<int>(m_state);
        failAll(QStringLiteral("dbserver timed out"));
    });
}

DbServerClient::~DbServerClient() {
    // Callbacks are not invoked from here: a destructor running as part of a
    // teardown would call back into objects that may already be half-gone.
    m_queue.clear();
    m_busy = false;
    m_current = Request();
    teardown();
}

void DbServerClient::setRequesterNumber(int number) {
    if (number == m_requesterNumber) {
        return;
    }
    m_requesterNumber = number;
    // The server binds the number at Introduce, so the existing session is now
    // claiming to be somebody else. Drop it; the next request re-handshakes.
    if (m_state != State::Idle) {
        teardown();
        m_state = State::Idle;
    }
}

void DbServerClient::requestArtwork(
        MediaSlot slot, quint32 artworkId, ArtworkCallback callback) {
    if (m_state == State::Broken) {
        if (callback) {
            callback(false, QByteArray(), QStringLiteral("dbserver unreachable"));
        }
        return;
    }
    Request request;
    request.slot = slot;
    request.artworkId = artworkId;
    request.callback = std::move(callback);
    m_queue.append(request);
    pump();
}

void DbServerClient::abortAll(const QString& reason) {
    failAll(reason);
}

void DbServerClient::pump() {
    if (m_busy || m_queue.isEmpty()) {
        return;
    }
    if (m_state != State::Ready) {
        ensureConnected();
        return;
    }

    m_busy = true;
    m_current = m_queue.takeFirst();
    m_current.transactionId = m_nextTransactionId++;
    // Binary loads use their own menu target rather than a display location.
    const quint32 descriptor = makeDescriptor(
            m_requesterNumber, m_current.slot, MenuTarget::Binary);
    sendMessage(makeGetArtwork(
            m_current.transactionId, descriptor, m_current.artworkId));
    m_pTimeout->start(kRequestTimeoutMs);
}

void DbServerClient::ensureConnected() {
    if (m_state != State::Idle) {
        return; // already on the way
    }
    startPortQuery();
}

void DbServerClient::startPortQuery() {
    m_state = State::QueryingPort;
    m_pPortSocket = new QTcpSocket(this);
    if (!m_localAddress.isNull() && m_localAddress != QHostAddress(QHostAddress::AnyIPv4)) {
        // Bind the interface the player was discovered on. On the deck Pi, with
        // eth0 on the CDJ network and wlan0 on the house LAN, letting the kernel
        // choose sends link-local traffic out the wrong NIC.
        m_pPortSocket->bind(m_localAddress);
    }

    connect(m_pPortSocket, &QTcpSocket::connected, this, [this]() {
        m_pPortSocket->write(portQuery());
    });
    connect(m_pPortSocket, &QTcpSocket::readyRead, this, [this]() {
        const QByteArray reply = m_pPortSocket->readAll();
        if (reply.size() < 2) {
            onSocketError(QStringLiteral("short port-query reply"));
            return;
        }
        const quint16 port = static_cast<quint16>(
                (static_cast<quint8>(reply.at(0)) << 8) |
                static_cast<quint8>(reply.at(1)));
        m_pPortSocket->disconnect(this);
        m_pPortSocket->deleteLater();
        m_pPortSocket = nullptr;
        // Always 1051 in every capture, but documented as dynamic, and a zero
        // here would otherwise become a connection to port 0.
        openConnection(port != 0 ? port : kDbServerPort);
    });
    connect(m_pPortSocket,
            &QTcpSocket::errorOccurred,
            this,
            [this](QAbstractSocket::SocketError) {
                onSocketError(m_pPortSocket
                                ? m_pPortSocket->errorString()
                                : QStringLiteral("port query failed"));
            });

    m_pPortSocket->connectToHost(m_peer, kDbServerQueryPort);
    m_pTimeout->start(kConnectTimeoutMs);
}

void DbServerClient::openConnection(quint16 port) {
    m_port = port;
    m_state = State::Connecting;
    m_rx.clear();
    m_preambleSeen = false;

    m_pSocket = new QTcpSocket(this);
    if (!m_localAddress.isNull() && m_localAddress != QHostAddress(QHostAddress::AnyIPv4)) {
        m_pSocket->bind(m_localAddress);
    }
    connect(m_pSocket, &QTcpSocket::connected, this, &DbServerClient::onConnected);
    connect(m_pSocket, &QTcpSocket::readyRead, this, &DbServerClient::onReadyRead);
    connect(m_pSocket,
            &QTcpSocket::errorOccurred,
            this,
            [this](QAbstractSocket::SocketError) {
                onSocketError(m_pSocket ? m_pSocket->errorString()
                                        : QStringLiteral("socket failed"));
            });
    connect(m_pSocket, &QTcpSocket::disconnected, this, [this]() {
        // Only a surprise if we still wanted something. An idle connection the
        // player closes is ordinary housekeeping.
        if (m_busy || !m_queue.isEmpty() || m_state != State::Ready) {
            onSocketError(QStringLiteral("player closed the dbserver connection"));
        } else {
            teardown();
            m_state = State::Idle;
        }
    });

    m_pSocket->connectToHost(m_peer, m_port);
    m_pTimeout->start(kConnectTimeoutMs);
}

void DbServerClient::onConnected() {
    m_state = State::Introducing;
    // The preamble goes first and is echoed verbatim, then Introduce. Both are
    // written straight away: the player answers them in order and there is no
    // reason to wait for the echo before sending the second.
    m_pSocket->write(preamble());
    sendMessage(makeIntroduce(m_requesterNumber));
    m_pTimeout->start(kRequestTimeoutMs);
}

void DbServerClient::sendMessage(const Message& message) {
    if (!m_pSocket) {
        return;
    }
    m_pSocket->write(message.encode());
}

void DbServerClient::onReadyRead() {
    if (!m_pSocket) {
        return;
    }
    m_rx.append(m_pSocket->readAll());
    if (m_rx.size() > kMaxBufferBytes) {
        onSocketError(QStringLiteral("dbserver sent %1 bytes without a whole message")
                              .arg(m_rx.size()));
        return;
    }

    // The five-byte preamble heads the stream in this direction too, and is not
    // a message: a decoder that does not step over it never finds the first
    // magic.
    if (!m_preambleSeen) {
        const QByteArray expected = preamble();
        if (m_rx.size() < expected.size()) {
            return;
        }
        if (!m_rx.startsWith(expected)) {
            onSocketError(QStringLiteral("dbserver answered the preamble with %1")
                                  .arg(QString::fromLatin1(
                                          m_rx.left(expected.size()).toHex())));
            return;
        }
        m_rx.remove(0, expected.size());
        m_preambleSeen = true;
    }

    int offset = 0;
    while (offset < m_rx.size()) {
        Message message;
        const Message::DecodeStatus status = Message::decode(m_rx, &offset, &message);
        if (status == Message::DecodeStatus::Incomplete) {
            break;
        }
        if (status == Message::DecodeStatus::Malformed) {
            onSocketError(QStringLiteral("malformed dbserver message"));
            return;
        }
        // Consume before dispatching: handleMessage can start the next request,
        // whose reply may already be in this same buffer.
        m_rx.remove(0, offset);
        offset = 0;
        handleMessage(message);
        if (!m_pSocket) {
            return; // torn down from inside the handler
        }
    }
    if (offset > 0) {
        m_rx.remove(0, offset);
    }
}

void DbServerClient::handleMessage(const Message& message) {
    if (m_state == State::Introducing) {
        if (message.type() != static_cast<quint16>(MessageType::Success)) {
            onSocketError(QStringLiteral("Introduce rejected (reply 0x%1)")
                                  .arg(message.type(), 4, 16, QChar('0')));
            return;
        }
        // The Introduce reply's second argument is the *server's* player number,
        // not an item count as it is for every other 0x4000.
        kLogger.info() << "connected to" << m_peer.toString() << "port" << m_port
                       << "as device" << m_requesterNumber << "; peer reports device"
                       << message.number(1);
        m_state = State::Ready;
        m_consecutiveFailures = 0;
        m_pTimeout->stop();
        pump();
        return;
    }

    if (!m_busy) {
        // Unsolicited. Nothing this client sends draws more than one reply, so
        // this means the stream is one message out -- which cannot be recovered
        // from, only noticed.
        kLogger.warning() << "unsolicited dbserver message from" << m_peer.toString()
                          << message.toString();
        return;
    }
    if (message.transactionId() != m_current.transactionId) {
        kLogger.warning() << "dbserver reply for transaction 0x"
                          << QString::number(message.transactionId(), 16) << "but we asked 0x"
                          << QString::number(m_current.transactionId, 16);
        failAll(QStringLiteral("dbserver replies are out of step"));
        return;
    }

    if (message.type() == static_cast<quint16>(MessageType::Error)) {
        finishCurrent(false, QByteArray(), QStringLiteral("player refused the request"));
        return;
    }
    if (message.type() != static_cast<quint16>(MessageType::Artwork)) {
        finishCurrent(false,
                QByteArray(),
                QStringLiteral("unexpected reply 0x%1")
                        .arg(message.type(), 4, 16, QChar('0')));
        return;
    }

    // The binary reply envelope is [request type, 0, byte length, blob]. A track
    // with no art yields a zero length and no blob at all, which is a success
    // with an empty image rather than an error. blob() falls back to argument 2
    // because a short envelope has been seen in the wild.
    QByteArray image = message.blob(3);
    if (image.isEmpty()) {
        image = message.blob(2);
    }
    finishCurrent(true, image, QString());
}

void DbServerClient::finishCurrent(
        bool ok, const QByteArray& image, const QString& error) {
    m_pTimeout->stop();
    const ArtworkCallback callback = m_current.callback;
    m_current = Request();
    m_busy = false;
    if (callback) {
        callback(ok, image, error);
    }
    pump();
}

void DbServerClient::onSocketError(const QString& error) {
    ++m_consecutiveFailures;
    failAll(error);
}

void DbServerClient::failAll(const QString& error) {
    m_pTimeout->stop();
    teardown();

    // Everything queued is reported before the state changes, so a callback that
    // enqueues another request finds a client that will try to connect again
    // rather than one still marked busy.
    QList<Request> pending = m_queue;
    m_queue.clear();
    const bool hadCurrent = m_busy;
    const Request current = m_current;
    m_current = Request();
    m_busy = false;

    m_state = m_consecutiveFailures >= kMaxConsecutiveFailures ? State::Broken : State::Idle;
    if (m_state == State::Broken) {
        kLogger.warning() << m_peer.toString() << "gave up after"
                          << m_consecutiveFailures << "dbserver failures:" << error;
    }

    if (hadCurrent && current.callback) {
        current.callback(false, QByteArray(), error);
    }
    for (const Request& request : pending) {
        if (request.callback) {
            request.callback(false, QByteArray(), error);
        }
    }
}

void DbServerClient::teardown() {
    for (QTcpSocket** ppSocket : {&m_pPortSocket, &m_pSocket}) {
        if (*ppSocket == nullptr) {
            continue;
        }
        QTcpSocket* pSocket = *ppSocket;
        *ppSocket = nullptr;
        // Disconnected first: closing emits disconnected(), and our handler for
        // it would otherwise re-enter this teardown.
        pSocket->disconnect(this);
        if (pSocket == m_pSocket || pSocket->state() == QAbstractSocket::ConnectedState) {
            pSocket->abort();
        }
        pSocket->deleteLater();
    }
    m_rx.clear();
    m_preambleSeen = false;
}

} // namespace dbserver
} // namespace prolink
} // namespace mixxx
