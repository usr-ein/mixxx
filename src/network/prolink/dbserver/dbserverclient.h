#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

#include "network/prolink/dbserver/dbservermessage.h"
#include "network/prolink/prolinkdefs.h"

class QTcpSocket;
class QTimer;

namespace mixxx {
namespace prolink {
namespace dbserver {

/// A dbserver client for one player: the protocol behind the LINK button.
///
/// Deliberately partial. It does the handshake and `GET_ARTWORK`, because that
/// is the one thing NFS cannot do: **a real CDJ never fetches album art over
/// NFS.** Two full load-and-play captures contain LOOKUPs for `.mp3`, `.m4a`,
/// `.wav` and `.aiff` and not one image (F49). Asking NFS for covers anyway
/// works for a while and then does not: walking `PIONEER/Artwork/000NN` per
/// image mints four filehandles a time, the player's handle table churns, and it
/// starts answering `NFSERR_STALE` to handles a millisecond old. Artwork travels
/// over this connection instead, where the player answers by id and no handle is
/// involved at all.
///
/// **The device number is the awkward part.** Every request carries ours in the
/// top byte of its descriptor and the server validates it: 1-4, belonging to a
/// device actually on the network, and not the player being asked. Since we do
/// not announce, we borrow -- see ProLinkNetworkService::pickRequesterNumber().
///
/// One connection serves both slots: which medium a request is about is carried
/// by the descriptor's slot byte, not by the connection (F37).
///
/// Lives on the ProLink network thread. Everything is asynchronous, and requests
/// are strictly serialised: the protocol is request/response over one TCP stream
/// with no length framing, so a second request in flight would be answered out
/// of order with no way to tell.
class DbServerClient : public QObject {
    Q_OBJECT

  public:
    /// *image* is empty when the track genuinely has no art -- that is a
    /// success, not a failure, and the player signals it by omitting the blob.
    using ArtworkCallback = std::function<void(bool ok, const QByteArray& image, const QString& error)>;

    DbServerClient(const QHostAddress& peer,
            const QHostAddress& localAddress,
            int requesterNumber,
            QObject* parent = nullptr);
    ~DbServerClient() override;

    /// Queue one artwork fetch. Connects first if not connected yet.
    void requestArtwork(MediaSlot slot, quint32 artworkId, ArtworkCallback callback);

    /// Fail everything queued and drop the connection.
    void abortAll(const QString& reason);

    /// The number we claim to be. Changing it re-handshakes on the next request,
    /// because the server binds it at Introduce time.
    int requesterNumber() const {
        return m_requesterNumber;
    }
    void setRequesterNumber(int number);

  private:
    enum class State {
        Idle,
        QueryingPort,
        Connecting,
        Introducing,
        Ready,
        /// Too many failures in a row. Stays here rather than retrying, so a
        /// player that refuses us costs one round of attempts and not a
        /// reconnect storm for the rest of the session.
        Broken,
    };

    struct Request {
        MediaSlot slot = MediaSlot::Usb;
        quint32 artworkId = 0;
        quint32 transactionId = 0;
        ArtworkCallback callback;
    };

    void ensureConnected();
    void startPortQuery();
    void openConnection(quint16 port);
    void onConnected();
    void onReadyRead();
    void onSocketError(const QString& error);
    void handleMessage(const Message& message);
    void pump();
    void sendMessage(const Message& message);
    void teardown();
    /// Fail the in-flight request and every queued one.
    void failAll(const QString& error);
    void finishCurrent(bool ok, const QByteArray& image, const QString& error);

    const QHostAddress m_peer;
    const QHostAddress m_localAddress;
    int m_requesterNumber;

    State m_state = State::Idle;
    QTcpSocket* m_pPortSocket = nullptr;
    QTcpSocket* m_pSocket = nullptr;
    QByteArray m_rx;
    /// Cleared once the peer has echoed the five-byte preamble.
    bool m_preambleSeen = false;
    quint16 m_port = kDbServerPort;
    quint32 m_nextTransactionId = kFirstTransactionId;
    int m_consecutiveFailures = 0;

    QList<Request> m_queue;
    Request m_current;
    bool m_busy = false;
    QTimer* m_pTimeout = nullptr;
};

} // namespace dbserver
} // namespace prolink
} // namespace mixxx
