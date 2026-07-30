#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QThread>
#include <memory>

#include "network/prolink/nfs/nfsfiletransfer.h"
#include "network/prolink/prolinkdevice.h"

namespace mixxx {
namespace prolink {

namespace nfs {
class NfsFileTransfer;
}
namespace dbserver {
class DbServerClient;
}

class ProLinkDiscovery;
} // namespace prolink
} // namespace mixxx

class ControlPushButton;
class QTimer;

namespace mixxx {
namespace prolink {

/// The single object the rest of Mixxx talks to about Pro DJ Link.
///
/// Owns the network thread and everything on it. Nothing above this class holds
/// a socket, a timer or a pointer into the network layer, which is what keeps
/// the `src/network/prolink/` → `src/library/` direction from ever reversing:
/// the library side connects to signals and calls slots, and never dereferences
/// anything that lives on the other thread.
///
/// **Why a dedicated thread rather than the GUI thread or QtConcurrent.** The
/// serve side, when it lands, does blocking reads off a USB stick to answer NFS
/// requests, and it must not delay a keep-alive: five missed keep-alives and we
/// disappear from every deck on the network, mid-set. Establishing the thread
/// boundary now, while there is only a listener behind it, is much cheaper than
/// retrofitting it later. QtConcurrent is wrong for a different reason — a
/// long-lived socket conversation is not a CPU job, and parking a pool thread on
/// one would starve the analyzer.
class ProLinkNetworkService : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkNetworkService(QObject* parent = nullptr);
    ~ProLinkNetworkService() override;

    /// Start the network thread and begin listening. Safe to call twice.
    void start();

    /// Stop listening, quit the thread and wait for it.
    ///
    /// Must be called before the owning feature starts tearing itself down,
    /// not from its destructor body: a queued signal already in flight would
    /// otherwise land on a half-destroyed receiver, and that crash only
    /// reproduces with a CDJ actually on the network.
    void shutdown();

    /// Snapshot of the peer table. Thread-safe by construction: it is served
    /// from a copy the network thread pushes to us, not by reaching across.
    QList<ProLinkDevice> devices() const {
        return m_devices;
    }
    int deviceCount() const {
        return m_devices.size();
    }

    bool isListening() const {
        return m_listening;
    }
    /// The player number we have claimed, or 0 while the handshake is still
    /// running or if it failed. Everything dbserver needs this to be non-zero.
    int announcedNumber() const {
        return m_announcedNumber;
    }
    /// Human-readable announcer state, for the preferences pane.
    QString announceDetail() const {
        return m_announceDetail;
    }
    /// Empty unless the socket could not be bound.
    QString lastError() const {
        return m_lastError;
    }

  public slots:
    /// Drop devices that are currently offline, rather than waiting out the
    /// removal grace period.
    void refresh();

    /// Pull `export.pdb` off the first player that has one, and log what
    /// happened: byte count, SHA-1, elapsed time, throughput.
    ///
    /// This is a diagnostic, not the real import path -- nothing is parsed or
    /// kept. It exists because it is the strongest check available on the whole
    /// RPC/NFS stack and it needs no GUI: the digest it logs can be compared
    /// against the same file read off the physically ejected stick, and
    /// byte-identical is the only acceptable answer. Triggered by the
    /// `[ProLink],pull_db` control.
    void pullDatabase(MediaSlot slot = MediaSlot::Usb);

    /// Fetch a player's `export.pdb` for the browse path.
    ///
    /// Asynchronous: the answer arrives as databaseFetched(). Keyed on MAC
    /// rather than on a device copy, so a request outliving the device it named
    /// resolves to "no longer on the network" instead of dereferencing anything.
    void fetchDatabase(const QByteArray& mac, MediaSlot slot);

    /// Fetch one file from a medium into *localPath*.
    ///
    /// *remotePath* is taken verbatim from `export.pdb`, which stores paths
    /// relative to the medium root with a leading slash. The answer arrives as
    /// fileFetched().
    /// *priority* jumps the queue. A track the user is waiting on must not sit
    /// behind a few hundred queued cover images.
    void fetchFile(const QByteArray& mac,
            MediaSlot slot,
            const QString& remotePath,
            const QString& localPath,
            bool priority = false);

    /// Fetch one cover image by its rekordbox artwork id, over dbserver.
    ///
    /// **Not over NFS**, deliberately. A real CDJ never asks NFS for an image
    /// (F49), and doing it anyway fails in a way that looks like a protocol
    /// mystery: the player's filehandle table churns under the extra lookups and
    /// it starts answering NFSERR_STALE to handles a millisecond old. dbserver
    /// answers by id, so no handle is minted and nothing accumulates.
    ///
    /// Silently does nothing if *localPath* already exists.
    void fetchArtwork(const QByteArray& mac,
            MediaSlot slot,
            quint32 artworkId,
            const QString& localPath);

  signals:
    void deviceFound(const mixxx::prolink::ProLinkDevice& device);
    void deviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void deviceLost(const QByteArray& mac);
    /// Emitted once the socket is up, or once it has failed to come up, so the
    /// UI can say which without polling.
    void listeningChanged(bool listening, const QString& error);
    /// We claimed a number, lost one, or gave up trying.
    void announceChanged(int deviceNumber, const QString& detail);
    /// The result of fetchDatabase(). *error* is empty on success.
    void databaseFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QByteArray& data,
            const QString& error);
    /// Result of fetchFile(). *error* is empty on success.
    void fileFetched(const QString& localPath, const QString& error);
    /// Result of fetchArtwork(). *error* is empty on success, including the
    /// success where the track simply has no art and nothing was written.
    void artworkFetched(const QString& localPath, const QString& error);
    /// Bytes so far and total, for whatever is waiting on that file.
    void fileFetchProgress(const QString& localPath, quint32 done, quint32 total);

  private slots:
    void onDeviceFound(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceLost(const QByteArray& mac);
    void onAnnounceStateChanged(int state, int deviceNumber, const QString& detail);

  private:
    void fetchOnNetworkThread(const ProLinkDevice& device, MediaSlot slot);
    struct FileRequest {
        ProLinkDevice device;
        MediaSlot slot = MediaSlot::Usb;
        QString remotePath;
        QString localPath;
        /// Set once a NFSERR_STALE has already cost this request a re-mount, so
        /// a genuinely missing file cannot loop.
        bool remounted = false;
    };
    /// Start the next queued request, if the network thread is idle.
    void pumpFileQueue();
    void runFileRequest(const FileRequest& request);
    /// The mount for one medium, kept open across requests.
    struct MountedMedium {
        nfs::NfsV2Client* pClient = nullptr;
        QByteArray rootHandle;
    };
    void onFetchFinished(const QByteArray& mac,
            MediaSlot slot,
            const QByteArray& data,
            const QString& error);
    void reportPull(const nfs::NfsFileTransfer::Result& result);

    /// A device number we may claim when talking to *target*'s dbserver.
    ///
    /// The server validates it: 1-4, belonging to a device actually on the
    /// network, and not the player being asked. We do not announce, so there is
    /// no number of our own to use and one has to be borrowed from another
    /// player — which is exactly what a two-deck rig provides. Returns 0 when
    /// there is nobody to borrow from, in which case artwork is unavailable
    /// until we announce (Phase B step 10).
    int pickRequesterNumber(const ProLinkDevice& target) const;
    /// Get or create the dbserver connection for one player. Network thread.
    dbserver::DbServerClient* dbClientFor(const ProLinkDevice& device, int requesterNumber);
    void runArtworkRequest(const ProLinkDevice& device,
            MediaSlot slot,
            quint32 artworkId,
            const QString& localPath,
            int requesterNumber);

    /// Raw and parented to `this`, not parented_ptr: created lazily in start(),
    /// and parented_ptr is deliberately non-assignable.
    QThread* m_pThread = nullptr;
    /// Lives on the network thread; never dereferenced from the GUI thread.
    ProLinkDiscovery* m_pDiscovery = nullptr;

    /// GUI-thread mirror of the peer table, kept in step by the three slots
    /// above. Duplicating it is cheaper and far safer than locking the real one.
    QList<ProLinkDevice> m_devices;

    /// `[ProLink],pull_db` -- fires the diagnostic above. Owned here rather than
    /// by the library feature so the two layers stay independent, and so this
    /// works even if the feature is not shown.
    std::unique_ptr<ControlPushButton> m_pPullDbControl;

    /// Non-empty while a `[ProLink],pull_db` diagnostic is in flight, so its
    /// extra logging and file-saving apply to that fetch and not to ordinary
    /// browsing — which would otherwise hash and write a megabyte on every
    /// expand.
    QByteArray m_diagnosticPull;

    /// File fetches are strictly serialised.
    ///
    /// They used to get an NfsV2Client each -- its own socket, its own portmap
    /// and mount -- which was survivable for one file and catastrophic for the
    /// artwork prefetch: a few hundred simultaneous clients, and every RPC on
    /// the deck timed out, including the track the user was waiting for. One
    /// mount per medium, one request at a time.
    QList<FileRequest> m_fileQueue;
    bool m_fileBusy = false;
    QTimer* m_pQueueWatchdog = nullptr;
    /// Keyed mac|slot. Lives on the network thread.
    QHash<QString, MountedMedium> m_mounts;

    /// One dbserver connection per player, keyed on MAC hex, living on the
    /// network thread and parented into it. Both slots share it: which medium a
    /// request is about is the descriptor's slot byte, not the connection (F37).
    QHash<QString, dbserver::DbServerClient*> m_dbClients;
    /// Cover fetches already asked for, keyed on local path, so the same image
    /// is not queued twice. An album's tracks share one cover, so without this a
    /// 651-track medium would ask for a few hundred images a dozen times each.
    QSet<QString> m_artworkInFlight;

    bool m_listening = false;
    QString m_lastError;

    /// GUI-thread mirror of the announcer's claimed number. Pushed to us rather
    /// than read across, like the peer table above.
    int m_announcedNumber = 0;
    QString m_announceDetail;
};

} // namespace prolink
} // namespace mixxx
