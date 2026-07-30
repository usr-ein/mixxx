#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include "network/prolink/prolinkdefs.h"
#include "network/prolink/server/prolinkvfs.h"

class QUdpSocket;
/// The generated Kaitai reader for an incoming RPC call, from
/// `prolinks-compat/ksy/prolink_rpc.ksy`. Global namespace, as Kaitai emits it.
class prolink_rpc_t;

namespace mixxx {
namespace prolink {
namespace server {

/// A userspace portmapper, mountd and nfsd, serving our media to real players.
///
/// **This has to work before the dbserver is worth running.** A deck asks the
/// portmapper for the mountd and nfsd ports *before* it ever opens a dbserver
/// connection, retries once a second indefinitely if nothing answers, and never
/// falls back to the well-known ports even when those are bound and idle (F46).
/// So NFS strictly gates browsing, not the other way round.
///
/// **UDP 111 is privileged**, and unavoidably so — see above. On the Pi that is
/// handled outside Mixxx, by `net.ipv4.ip_unprivileged_port_start = 111` in
/// `pi_config/99-prolink-ports.conf`. If the bind fails the server still comes
/// up on mountd and nfsd, and says so: it costs nothing, and it leaves the door
/// open for hardware that skips the portmapper.
///
/// mountd and nfsd take the ports a real player answers on (48276 and 2049,
/// three observations across three devices, F6) when they are free, so a deck
/// that does skip the lookup still finds us. Both are unprivileged.
///
/// Read-only: no write procedures exist here at all. Lives on the ProLink
/// network thread and reads from the medium synchronously — an 8 KiB read off a
/// USB stick is well inside the keep-alive budget, and a request that had to hop
/// threads would cost more in latency than the read does.
class ProLinkNfsServer : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkNfsServer(QObject* parent = nullptr);
    ~ProLinkNfsServer() override;

    /// Bind the three sockets. True if mountd and nfsd came up; the portmapper
    /// is reported through portmapAvailable() rather than failing the start,
    /// because serving without it is degraded but not useless.
    bool start();
    void stop();

    bool portmapAvailable() const {
        return m_portmapPort != 0;
    }
    quint16 mountdPort() const {
        return m_mountdPort;
    }
    quint16 nfsdPort() const {
        return m_nfsdPort;
    }

    /// Expose *directory* as the export a player mounts for *slot*: `/C/` for
    /// USB, `/B/` for SD.
    void addExport(MediaSlot slot, const QString& directory);
    void removeExport(MediaSlot slot);

    /// Procedure call tallies, keyed `mountd:MNT`. The primary evidence that a
    /// deck is actually talking to us, and cheap enough to keep always.
    QHash<QString, int> statistics() const {
        return m_statistics;
    }

  private slots:
    void readPortmap();
    void readMountd();
    void readNfsd();

  private:
    /// Drain one socket, answering each datagram.
    void drain(QUdpSocket* pSocket);
    /// Decode one call and dispatch it. Returns the whole reply datagram, or
    /// empty to stay silent.
    QByteArray handleCall(const QByteArray& datagram);

    QByteArray dispatchPortmap(prolink_rpc_t* pCall);
    QByteArray dispatchMount(prolink_rpc_t* pCall);
    QByteArray dispatchNfs(prolink_rpc_t* pCall);

    /// The VFS subtree an export path maps to, or empty for an unknown export.
    QString subtreeForExport(const QString& exportPath) const;

    QUdpSocket* m_pPortmap = nullptr;
    QUdpSocket* m_pMountd = nullptr;
    QUdpSocket* m_pNfsd = nullptr;
    quint16 m_portmapPort = 0;
    quint16 m_mountdPort = 0;
    quint16 m_nfsdPort = 0;

    ProLinkVfs m_vfs;
    /// Export path (`/C/`) to VFS prefix (`C`). Held separately from the VFS so
    /// that `EXPORT` can list exactly what is mounted right now.
    QHash<QString, QString> m_exports;
    QHash<QString, int> m_statistics;
};

} // namespace server
} // namespace prolink
} // namespace mixxx
