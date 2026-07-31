#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>

#include "network/prolink/prolinkdefs.h"

class QUdpSocket;

namespace mixxx {
namespace prolink {
namespace server {

class ProLinkDbServer;
class ProLinkMediaWatcher;
class ProLinkNfsServer;
class ProLinkStatusServer;

/// Everything that makes a real CDJ able to read our USBs, in one object.
///
/// Four collaborators, and the order they matter in is not the order they look
/// important:
///
/// 1. **status** (UDP 50002) — says our slots hold media. Nothing else happens
///    until a deck believes that, because slot occupancy is published here and
///    nowhere else (F20), and a deck that thinks our slots are empty has nothing
///    to browse.
/// 2. **NFS** (portmap/mountd/nfsd) — a deck asks the portmapper for our mount
///    ports *before* it opens dbserver, and retries forever if nothing answers
///    (F46). So NFS strictly gates browsing.
/// 3. **dbserver** (TCP 1051) — the protocol the LINK button drives.
/// 4. **the media watcher** — which of the Pi's USB ports currently holds a
///    rekordbox stick, and what to call it.
///
/// **Only mounted rekordbox media is served.** Mixxx's own library is not
/// exposed and must not be: it has no `export.pdb`, no ANLZ files and no stable
/// track ids, so a deck told about those tracks would stall at NOW LOADING
/// rather than fail cleanly.
///
/// Lives on the ProLink network thread, alongside discovery, and shares its
/// UDP-50002 socket — replies have to leave from the port we listen on.
class ProLinkServer : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkServer(QUdpSocket* pStatusSocket, QObject* parent = nullptr);
    ~ProLinkServer() override;

    /// Bind the listeners and start watching for media. True if the NFS server
    /// came up; the dbserver and the status emitter are reported separately,
    /// since each can be useful without the others.
    bool start();
    void stop();

    /// Our claimed player number. Everything here is inert until it is non-zero:
    /// a status packet from device 0 is worse than no status packet, and a deck
    /// validates the number in every dbserver request.
    void setDeviceNumber(int number);
    /// Who to unicast status to. Status is never broadcast — not one of 1507
    /// captured packets was (F21).
    void setPeers(const QList<QHostAddress>& peers);

    ProLinkStatusServer* statusServer() const {
        return m_pStatus;
    }

    /// One line per slot, plus the listener ports, for the ProLink page.
    QString summary() const;

  private slots:
    void onMediumMounted(mixxx::prolink::MediaSlot slot,
            const QString& path,
            const QString& volumeName);
    void onMediumUnmounted(mixxx::prolink::MediaSlot slot);

  private:
    QUdpSocket* const m_pStatusSocket;
    ProLinkNfsServer* m_pNfs = nullptr;
    ProLinkDbServer* m_pDb = nullptr;
    ProLinkStatusServer* m_pStatus = nullptr;
    ProLinkMediaWatcher* m_pWatcher = nullptr;
    int m_deviceNumber = 0;
};

} // namespace server
} // namespace prolink
} // namespace mixxx
