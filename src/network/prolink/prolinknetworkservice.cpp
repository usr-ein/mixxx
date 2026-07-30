#include "network/prolink/prolinknetworkservice.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkInterface>

#include "moc_prolinknetworkservice.cpp"
#include "control/controlpushbutton.h"
#include "util/cmdlineargs.h"
#include "network/prolink/nfs/nfsfiletransfer.h"
#include "network/prolink/nfs/nfsv2client.h"
#include "network/prolink/prolinkdiscovery.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNetworkService");
/// How long shutdown waits for the network thread before giving up on it.
/// Generous: the thread only ever has to finish one datagram or one timer tick,
/// so exceeding this means something is genuinely wedged, and hanging Mixxx's
/// exit would be worse than leaking a thread.
constexpr int kThreadQuitTimeoutMs = 2000;

/// Longest one queued file may take before the queue is assumed stuck. Well
/// above a large track over NFS, and far below anything a user would wait out.
constexpr int kQueueStallTimeoutMs = 150000;

/// Our own address on the interface a device's announcements arrived on.
///
/// Not cosmetic on the deck, which has eth0 on the CDJ network and wlan0 on the
/// house LAN: bind the wrong one and the kernel routes link-local traffic out
/// the wrong NIC, after which every RPC times out with nothing to point at.
QHostAddress localAddressFor(const mixxx::prolink::ProLinkDevice& device) {
    const QNetworkInterface iface = QNetworkInterface::interfaceFromName(device.interfaceName);
    for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
            return entry.ip();
        }
    }
    // Falling back to Any is better than refusing: on a single-homed host it is
    // correct, and on a multi-homed one it at least tries.
    return QHostAddress(QHostAddress::AnyIPv4);
}
/// Write a fetched file, atomically.
///
/// Via a .part and a rename, because SoundSource::getTypeFromFile classifies by
/// *reading bytes* (QMimeDatabase::MatchContent). A half-written file would be
/// classified as unsupported, and the failure would look like an unsupported
/// format rather than an interrupted download.
QString writeFetchedFile(const QString& path, const QByteArray& data) {
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return QStringLiteral("could not create %1").arg(info.absolutePath());
    }
    const QString partPath = path + QStringLiteral(".part");
    QFile part(partPath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QStringLiteral("could not write %1: %2").arg(partPath, part.errorString());
    }
    const qint64 written = part.write(data);
    part.close();
    if (written != data.size()) {
        QFile::remove(partPath);
        return QStringLiteral("short write to %1").arg(partPath);
    }
    QFile::remove(path);
    if (!QFile::rename(partPath, path)) {
        QFile::remove(partPath);
        return QStringLiteral("could not rename %1").arg(partPath);
    }
    return QString();
}
} // namespace

namespace mixxx {
namespace prolink {

ProLinkNetworkService::ProLinkNetworkService(QObject* parent)
        : QObject(parent),
          m_pPullDbControl(std::make_unique<ControlPushButton>(
                  ConfigKey(QStringLiteral("[ProLink]"), QStringLiteral("pull_db")))) {
    connect(m_pPullDbControl.get(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                if (value > 0) {
                    pullDatabase();
                }
            });
}

ProLinkNetworkService::~ProLinkNetworkService() {
    // Idempotent, so this is a backstop rather than the intended path: callers
    // should shutdown() explicitly while they are still fully constructed.
    shutdown();
}

void ProLinkNetworkService::start() {
    if (m_pThread) {
        return;
    }

    m_pThread = new QThread(this);
    m_pThread->setObjectName(QStringLiteral("ProLink Net"));

    // Constructed here, then moved. Its socket is *not* created yet -- that
    // happens in the lambda below, which runs on the network thread, because a
    // QUdpSocket belongs to whichever thread created it and Qt will not service
    // it from another.
    m_pDiscovery = new ProLinkDiscovery();
    m_pDiscovery->moveToThread(m_pThread);

    // Queued automatically, sender and receiver being on different threads.
    // Every payload is a value type, so each arrives as a plain copy.
    connect(m_pDiscovery,
            &ProLinkDiscovery::deviceFound,
            this,
            &ProLinkNetworkService::onDeviceFound);
    connect(m_pDiscovery,
            &ProLinkDiscovery::deviceChanged,
            this,
            &ProLinkNetworkService::onDeviceChanged);
    connect(m_pDiscovery,
            &ProLinkDiscovery::deviceLost,
            this,
            &ProLinkNetworkService::onDeviceLost);

    m_pThread->start();

    // Bind on the network thread, then hop the outcome back here rather than
    // writing our own members from over there.
    ProLinkDiscovery* pDiscovery = m_pDiscovery;
    QMetaObject::invokeMethod(
            m_pDiscovery,
            [this, pDiscovery] {
                const bool listening = pDiscovery->start();
                const QString error = pDiscovery->lastError();
                QMetaObject::invokeMethod(
                        this,
                        [this, listening, error] {
                            m_listening = listening;
                            m_lastError = error;
                            emit listeningChanged(listening, error);
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
}

void ProLinkNetworkService::shutdown() {
    if (!m_pThread) {
        return;
    }
    if (m_pDiscovery) {
        // Blocking, so the socket is provably closed before the thread is asked
        // to quit. Safe from the GUI thread because the network thread is purely
        // event-driven and never blocks waiting on us, so there is no inversion
        // to deadlock on.
        //
        // Functor form, not a method-name string: checked at compile time, so a
        // rename cannot quietly turn this into a no-op.
        ProLinkDiscovery* pDiscovery = m_pDiscovery;
        QMetaObject::invokeMethod(
                m_pDiscovery,
                [pDiscovery] { pDiscovery->stop(); },
                Qt::BlockingQueuedConnection);
    }

    // Drop queued work before the thread goes: a request that starts during
    // shutdown would build sockets on a thread about to stop servicing them.
    m_fileQueue.clear();
    m_fileBusy = false;
    m_mounts.clear();

    m_pThread->quit();
    const bool exited = m_pThread->wait(kThreadQuitTimeoutMs);
    if (!exited) {
        kLogger.warning() << "network thread did not exit in" << kThreadQuitTimeoutMs
                          << "ms; leaking it rather than hanging shutdown";
    }

    // Delete outright rather than via deleteLater(): the thread's event loop has
    // already exited, so a posted deletion event would never be dispatched and
    // the object would simply leak. Deleting directly is safe precisely because
    // wait() returned -- nothing is running on it any more.
    //
    // If wait() timed out we leak deliberately: destroying an object under a
    // thread still using it is a crash, and a leak on the way out is not.
    if (exited) {
        delete m_pDiscovery;
    }
    m_pDiscovery = nullptr;

    delete m_pThread;
    m_pThread = nullptr;

    m_devices.clear();
    m_listening = false;
}

void ProLinkNetworkService::refresh() {
    if (!m_pDiscovery) {
        return;
    }
    ProLinkDiscovery* pDiscovery = m_pDiscovery;
    QMetaObject::invokeMethod(
            m_pDiscovery,
            [pDiscovery] { pDiscovery->forgetStaleDevices(); },
            Qt::QueuedConnection);
}

void ProLinkNetworkService::fetchDatabase(const QByteArray& mac, MediaSlot slot) {
    ProLinkDevice target;
    for (const ProLinkDevice& device : m_devices) {
        if (device.mac == mac) {
            target = device;
            break;
        }
    }
    if (!target.isValid() || !m_pDiscovery) {
        emit databaseFetched(mac, slot, QByteArray(), tr("player is no longer on the network"));
        return;
    }
    if (!target.online) {
        emit databaseFetched(mac, slot, QByteArray(), tr("player is offline"));
        return;
    }

    // Hop onto the network thread: everything below opens sockets.
    const ProLinkDevice device = target;
    QMetaObject::invokeMethod(
            m_pDiscovery,
            [this, device, slot] { fetchOnNetworkThread(device, slot); },
            Qt::QueuedConnection);
}

void ProLinkNetworkService::pullDatabase(MediaSlot slot) {
    // The [ProLink],pull_db diagnostic: first player, log the result. Kept
    // deliberately separate from the browse path so it stays usable when
    // browsing is broken -- which is exactly when it is worth having.
    for (const ProLinkDevice& device : m_devices) {
        if (device.isPlayer() && device.online) {
            m_diagnosticPull = device.mac;
            fetchDatabase(device.mac, slot);
            return;
        }
    }
    kLogger.warning() << "pull_db: no player on the network to pull from";
}

void ProLinkNetworkService::fetchFile(const QByteArray& mac,
        MediaSlot slot,
        const QString& remotePath,
        const QString& localPath,
        bool priority) {
    ProLinkDevice target;
    for (const ProLinkDevice& device : m_devices) {
        if (device.mac == mac) {
            target = device;
            break;
        }
    }
    if (!target.isValid() || !target.online || !m_pDiscovery) {
        emit fileFetched(localPath, tr("player is no longer on the network"));
        return;
    }

    // Already queued? Asking twice is normal -- the artwork prefetch and a
    // deliberate load can want the same cover -- and enqueuing it twice would
    // fetch it twice and emit two results, one of which nobody is waiting for.
    for (const FileRequest& queued : m_fileQueue) {
        if (queued.localPath == localPath) {
            return;
        }
    }

    FileRequest request;
    request.device = target;
    request.slot = slot;
    request.remotePath = remotePath;
    request.localPath = localPath;
    if (priority) {
        m_fileQueue.prepend(request);
    } else {
        m_fileQueue.append(request);
    }
    pumpFileQueue();
}

void ProLinkNetworkService::pumpFileQueue() {
    if (m_fileBusy || m_fileQueue.isEmpty() || !m_pDiscovery) {
        return;
    }
    // A stalled queue is silent by nature -- work simply stops -- so it gets a
    // watchdog rather than being trusted. If a request neither completes nor
    // fails within this, something below dropped a callback and the queue would
    // otherwise sit idle with hundreds of entries behind it.
    if (!m_pQueueWatchdog) {
        m_pQueueWatchdog = new QTimer(this);
        m_pQueueWatchdog->setSingleShot(true);
        m_pQueueWatchdog->setInterval(kQueueStallTimeoutMs);
        connect(m_pQueueWatchdog, &QTimer::timeout, this, [this]() {
            if (!m_fileBusy) {
                return;
            }
            kLogger.warning() << "file queue stalled with" << m_fileQueue.size()
                              << "requests left; releasing it";
            m_fileBusy = false;
            pumpFileQueue();
        });
    }
    m_pQueueWatchdog->start();
    m_fileBusy = true;
    const FileRequest request = m_fileQueue.takeFirst();
    QMetaObject::invokeMethod(
            m_pDiscovery,
            [this, request] { runFileRequest(request); },
            Qt::QueuedConnection);
}

void ProLinkNetworkService::runFileRequest(const FileRequest& request) {
    const QString key = QStringLiteral("%1|%2")
                                .arg(QString::fromLatin1(request.device.mac.toHex()))
                                .arg(static_cast<int>(request.slot));
    const QString localPath = request.localPath;

    // Report, release the slot, and start the next one. Every exit path goes
    // through here so the queue cannot stall on a failure.
    auto finish = [this, localPath](const QString& error) {
        QMetaObject::invokeMethod(
                this,
                [this, localPath, error] {
                    m_fileBusy = false;
                    if (!error.isEmpty()) {
                        // Logged here rather than left to the caller: most
                        // fetches are fire-and-forget prefetches whose results
                        // nobody looks at, and an unlogged failure there is
                        // indistinguishable from one that never ran.
                        kLogger.warning() << "fetch of" << localPath << "failed:" << error;
                    }
                    emit fileFetched(localPath, error);
                    pumpFileQueue();
                },
                Qt::QueuedConnection);
    };

    auto withRoot = [this, request, key, localPath, finish](const QByteArray& rootHandle) {
        nfs::NfsV2Client* pClient = m_mounts.value(key).pClient;
        auto* pTransfer = new nfs::NfsFileTransfer(pClient, pClient);
        pTransfer->setProgressCallback([this, localPath](quint32 done, quint32 total) {
            QMetaObject::invokeMethod(
                    this,
                    [this, localPath, done, total] {
                        emit fileFetchProgress(localPath, done, total);
                    },
                    Qt::QueuedConnection);
        });
        pClient->resolvePath(rootHandle,
                request.remotePath,
                [this, pClient, pTransfer, localPath, finish](
                        const nfs::NfsV2Client::Outcome<QByteArray>& file) {
                    if (!file.ok) {
                        pTransfer->deleteLater();
                        finish(file.error);
                        return;
                    }
                    pClient->getAttributes(file.value,
                            [this, pTransfer, localPath, file, finish](
                                    const nfs::NfsV2Client::Outcome<
                                            nfs::FileAttributes>& attrs) {
                                if (!attrs.ok) {
                                    pTransfer->deleteLater();
                                    finish(attrs.error);
                                    return;
                                }
                                pTransfer->fetch(file.value,
                                        attrs.value.size,
                                        [pTransfer, localPath, finish](
                                                const nfs::NfsFileTransfer::Result&
                                                        result) {
                                            const QString error = result.ok
                                                    ? writeFetchedFile(localPath,
                                                              result.data)
                                                    : result.error;
                                            pTransfer->deleteLater();
                                            finish(error);
                                        });
                            });
                });
    };

    // Reuse the mount if we have one. Mounting per file was what turned the
    // artwork prefetch into a few hundred simultaneous portmap conversations.
    const auto mounted = m_mounts.constFind(key);
    if (mounted != m_mounts.constEnd() && !mounted->rootHandle.isEmpty()) {
        withRoot(mounted->rootHandle);
        return;
    }

    auto* pClient = new nfs::NfsV2Client(
            request.device.address, localAddressFor(request.device), m_pDiscovery);
    MountedMedium record;
    record.pClient = pClient;
    m_mounts.insert(key, record);

    pClient->mount(exportPathForSlot(request.slot),
            [this, key, withRoot, finish](
                    const nfs::NfsV2Client::Outcome<QByteArray>& result) {
                if (!result.ok) {
                    // Drop the record so the next request retries the mount
                    // rather than inheriting a broken one.
                    auto record = m_mounts.take(key);
                    if (record.pClient) {
                        record.pClient->deleteLater();
                    }
                    finish(result.error);
                    return;
                }
                m_mounts[key].rootHandle = result.value;
                withRoot(result.value);
            });
}

void ProLinkNetworkService::fetchOnNetworkThread(
        const ProLinkDevice& device, MediaSlot slot) {
    const QString exportPath = exportPathForSlot(slot);
    const QByteArray mac = device.mac;
    kLogger.info() << "fetching" << exportPath << "from" << device.label();

    // The transfer is a child of the client, so one deleteLater takes both and
    // Qt destroys the child while the parent is still intact.
    auto* pNfs = new nfs::NfsV2Client(device.address, localAddressFor(device), m_pDiscovery);
    auto* pTransfer = new nfs::NfsFileTransfer(pNfs, pNfs);

    // Every failure path funnels through here, so none can forget to report or
    // to clean up -- with a chain this deep that is not a hypothetical.
    auto fail = [this, pNfs, mac, slot](const QString& error) {
        kLogger.warning() << "fetch failed:" << error;
        QMetaObject::invokeMethod(
                this,
                [this, mac, slot, error] { onFetchFinished(mac, slot, QByteArray(), error); },
                Qt::QueuedConnection);
        pNfs->deleteLater();
    };

    pNfs->mount(exportPath,
            [this, pNfs, pTransfer, exportPath, mac, slot, fail](
                    const nfs::NfsV2Client::Outcome<QByteArray>& mounted) {
                if (!mounted.ok) {
                    fail(mounted.error);
                    return;
                }
                pNfs->resolvePath(mounted.value,
                        QStringLiteral("PIONEER/rekordbox/export.pdb"),
                        [this, pNfs, pTransfer, exportPath, mac, slot, fail](
                                const nfs::NfsV2Client::Outcome<QByteArray>& file) {
                            if (!file.ok) {
                                fail(file.error);
                                return;
                            }
                            pNfs->getAttributes(file.value,
                                    [this, pNfs, pTransfer, exportPath, mac, slot, file, fail](
                                            const nfs::NfsV2Client::Outcome<
                                                    nfs::FileAttributes>& attrs) {
                                        if (!attrs.ok) {
                                            fail(attrs.error);
                                            return;
                                        }
                                        pTransfer->fetch(file.value,
                                                attrs.value.size,
                                                [this, pNfs, exportPath, mac, slot](
                                                        const nfs::NfsFileTransfer::
                                                                Result& result) {
                                                    reportPull(result);
                                                    const QByteArray data =
                                                            result.ok ? result.data
                                                                      : QByteArray();
                                                    const QString error =
                                                            result.ok ? QString()
                                                                      : result.error;
                                                    QMetaObject::invokeMethod(
                                                            this,
                                                            [this, mac, slot, data, error] {
                                                                onFetchFinished(mac,
                                                                        slot,
                                                                        data,
                                                                        error);
                                                            },
                                                            Qt::QueuedConnection);
                                                    pNfs->unmount(exportPath);
                                                    pNfs->deleteLater();
                                                });
                                    });
                        });
            });
}

void ProLinkNetworkService::reportPull(const nfs::NfsFileTransfer::Result& result) {
    if (m_diagnosticPull.isEmpty()) {
        // An ordinary browse fetch. Hashing and writing a megabyte on every
        // expand would be pure cost, so just say what happened.
        if (result.ok) {
            kLogger.info() << "fetched" << result.data.size() << "bytes in"
                           << result.reads << "reads," << result.elapsedMs << "ms";
        }
        return;
    }
    m_diagnosticPull.clear();

    if (!result.ok) {
        kLogger.warning() << "pull_db FAILED after" << result.reads
                          << "reads:" << result.error;
        return;
    }

    // Two digests, because one is not enough to interpret.
    //
    // A player rewrites its own bookkeeping into the pdb header as it operates:
    // a play count or a history entry lands in the sequence counter at 0x14, and
    // pulling the same database twice produced files differing in exactly that
    // window and nothing else (F13). So a raw digest that fails to match the
    // stick proves nothing on its own -- the deck may simply have written to it
    // between the fetch and the eject.
    //
    // The stable digest zeroes 0x10..0x18 before hashing. Raw differs but stable
    // matches -> the player's own bookkeeping, and the transfer was perfect.
    // Both differ -> a real bug. One line each, no file comparison needed.
    constexpr int kVolatileStart = 0x10;
    constexpr int kVolatileEnd = 0x18;
    const QByteArray digest =
            QCryptographicHash::hash(result.data, QCryptographicHash::Sha1).toHex();
    QByteArray stabilised = result.data;
    if (stabilised.size() >= kVolatileEnd) {
        memset(stabilised.data() + kVolatileStart, 0, kVolatileEnd - kVolatileStart);
    }
    const QByteArray stableDigest =
            QCryptographicHash::hash(stabilised, QCryptographicHash::Sha1).toHex();

    // Keep the bytes too: without them a digest mismatch can only be guessed at,
    // and `cmp -l` against the stick is the difference between "the header
    // moved" and "the transfer is broken".
    const QString savedPath = QDir(CmdlineArgs::Instance().getSettingsPath())
                                      .filePath(QStringLiteral("prolink-pull.pdb"));
    QFile saved(savedPath);
    if (saved.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        saved.write(result.data);
        saved.close();
        kLogger.info() << "pull_db saved:" << savedPath;
    } else {
        kLogger.warning() << "pull_db: could not save to" << savedPath << ":"
                          << saved.errorString();
    }

    const double seconds = result.elapsedMs / 1000.0;
    const double kib = result.data.size() / 1024.0;
    kLogger.info() << "pull_db OK:" << result.data.size() << "bytes in" << result.reads
                   << "reads," << result.shortReads << "short," << result.elapsedMs
                   << "ms," << (seconds > 0 ? qRound(kib / seconds) : 0) << "KiB/s";
    kLogger.info() << "pull_db sha1       :" << digest;
    kLogger.info() << "pull_db sha1 stable:" << stableDigest
                   << "(header 0x10-0x18 zeroed; compare this one if the raw "
                      "digests differ)";
}

void ProLinkNetworkService::onFetchFinished(const QByteArray& mac,
        MediaSlot slot,
        const QByteArray& data,
        const QString& error) {
    emit databaseFetched(mac, slot, data, error);
}

void ProLinkNetworkService::onDeviceFound(const ProLinkDevice& device) {
    m_devices.append(device);
    emit deviceFound(device);
}

void ProLinkNetworkService::onDeviceChanged(const ProLinkDevice& device) {
    for (auto& known : m_devices) {
        if (known.sameDeviceAs(device)) {
            known = device;
            emit deviceChanged(device);
            return;
        }
    }
    // Changed before we were told it was found. Should not happen, but treating
    // it as a discovery keeps the mirror from silently drifting out of step with
    // the real table, which would be much harder to notice.
    m_devices.append(device);
    emit deviceFound(device);
}

void ProLinkNetworkService::onDeviceLost(const QByteArray& mac) {
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).mac == mac) {
            m_devices.removeAt(i);
            break;
        }
    }
    emit deviceLost(mac);
}

} // namespace prolink
} // namespace mixxx
