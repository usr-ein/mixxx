#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>

#include "network/prolink/dbserver/dbservermessage.h"
#include "network/prolink/prolinkdefs.h"
#include "network/prolink/server/prolinkservedmedium.h"

class QTcpServer;
class QTcpSocket;

namespace mixxx {
namespace prolink {
namespace server {

class ProLinkDbServer;

/// One client conversation. Owns the result sets being paged through.
///
/// The protocol is **stateful per connection**: a menu request establishes a
/// result set and the `0x3000` render that follows pages through it. What is not
/// obvious is that a client does not run one menu at a time — a CDJ scrolling a
/// track list interleaves per-track `GetMetadata` lookups with continued
/// scrolling, then resumes rendering the list at the next offset without
/// re-issuing the menu request.
///
/// So result sets are held **keyed on (descriptor, item count)**, not replaced.
/// The count alone is not enough: a metadata reply is exactly 13 items, so
/// browsing a 13-track album destroyed the track list and every later page
/// served metadata instead. The descriptor supplies the missing bit — its
/// menu-target byte separates the list a deck is scrolling from the transient
/// menu it dips into (F41).
class ProLinkDbConnection : public QObject {
    Q_OBJECT

  public:
    ProLinkDbConnection(ProLinkDbServer* pServer, QTcpSocket* pSocket);

  private slots:
    void onReadyRead();
    void onDisconnected();

  private:
    void handle(const dbserver::Message& request);
    void send(const dbserver::Message& reply);
    /// Page through a result set established by an earlier menu request.
    void render(const dbserver::Message& request);

    ProLinkDbServer* const m_pServer;
    QTcpSocket* const m_pSocket;
    QByteArray m_buffer;
    /// Cleared once the peer's five-byte preamble has been consumed and echoed.
    bool m_preambleDone = false;
    int m_clientDeviceNumber = 0;

    QHash<QPair<quint32, int>, QList<dbserver::Message>> m_menus;
    /// Falls back to this when a client pages a set we never established, which
    /// beats handing it an empty page.
    QList<dbserver::Message> m_lastMenu;
};

/// Serves our media to real players over dbserver — the protocol a CDJ's LINK
/// button drives.
///
/// NFS alone makes the files readable; this is what makes the library
/// *browsable*. Both are needed, and in that order: a deck that cannot mount
/// never opens a dbserver connection at all (F46).
///
/// Two listeners, mirroring a real player: TCP 12523 answers the fixed port
/// query with our dbserver port, and TCP 1051 carries the conversation.
///
/// **A player browsing both our slots uses one connection** and names the slot
/// in every request's descriptor (F37), so the medium is resolved per *message*
/// and never cached per connection — caching it would serve the wrong library
/// the moment the DJ switched slots.
class ProLinkDbServer : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkDbServer(QObject* parent = nullptr);
    ~ProLinkDbServer() override;

    bool start();
    void stop();

    quint16 port() const {
        return m_port;
    }
    bool isListening() const {
        return m_port != 0;
    }

    /// Our own player number, echoed in the `Introduce` reply.
    void setDeviceNumber(int number) {
        m_deviceNumber = number;
    }
    int deviceNumber() const {
        return m_deviceNumber;
    }

    void setMedium(MediaSlot slot, const ProLinkServedMedium& medium);
    void removeMedium(MediaSlot slot);
    /// The medium a request refers to, from its descriptor's slot byte, or
    /// nullptr for a slot we do not serve.
    const ProLinkServedMedium* mediumFor(quint32 descriptor) const;

    QHash<QString, int> statistics() const {
        return m_statistics;
    }
    void countRequest(const QString& name) {
        m_statistics[name] = m_statistics.value(name, 0) + 1;
    }

    /// Turn a menu request into the items it should produce.
    ///
    /// *pHandled* is false for a request we do not implement, which becomes a
    /// `0x4003` error rather than a silent empty list — a player showing an
    /// empty folder when it should show a failure is worse than an error.
    QList<dbserver::Message> buildMenu(const dbserver::Message& request,
            const ProLinkServedMedium* pMedium,
            bool* pHandled) const;

  private slots:
    void onDbConnection();
    void onQueryConnection();

  private:
    QTcpServer* m_pListener = nullptr;
    QTcpServer* m_pQueryListener = nullptr;
    quint16 m_port = 0;
    int m_deviceNumber = 0;

    QMap<int, ProLinkServedMedium> m_media;
    QHash<QString, int> m_statistics;
};

} // namespace server
} // namespace prolink
} // namespace mixxx
