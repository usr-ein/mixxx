#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QThread>
#include <memory>

#include "network/prolink/nfs/nfsfiletransfer.h"
#include "network/prolink/prolinkdevice.h"

namespace mixxx {
namespace prolink {

namespace nfs {
class NfsFileTransfer;
}

class ProLinkDiscovery;
} // namespace prolink
} // namespace mixxx

class ControlPushButton;

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

  signals:
    void deviceFound(const mixxx::prolink::ProLinkDevice& device);
    void deviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void deviceLost(const QByteArray& mac);
    /// Emitted once the socket is up, or once it has failed to come up, so the
    /// UI can say which without polling.
    void listeningChanged(bool listening, const QString& error);
    /// The result of fetchDatabase(). *error* is empty on success.
    void databaseFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QByteArray& data,
            const QString& error);
    /// Result of fetchFile(). *error* is empty on success.
    void fileFetched(const QString& localPath, const QString& error);
    /// Bytes so far and total, for whatever is waiting on that file.
    void fileFetchProgress(const QString& localPath, quint32 done, quint32 total);

  private slots:
    void onDeviceFound(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceLost(const QByteArray& mac);

  private:
    void fetchOnNetworkThread(const ProLinkDevice& device, MediaSlot slot);
    struct FileRequest {
        ProLinkDevice device;
        MediaSlot slot = MediaSlot::Usb;
        QString remotePath;
        QString localPath;
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
    /// Keyed mac|slot. Lives on the network thread.
    QHash<QString, MountedMedium> m_mounts;

    bool m_listening = false;
    QString m_lastError;
};

} // namespace prolink
} // namespace mixxx
