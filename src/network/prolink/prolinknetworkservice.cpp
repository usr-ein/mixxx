#include "network/prolink/prolinknetworkservice.h"

#include <QCryptographicHash>
#include <QMetaObject>
#include <QNetworkInterface>

#include "moc_prolinknetworkservice.cpp"
#include "control/controlpushbutton.h"
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

void ProLinkNetworkService::pullDatabase(MediaSlot slot) {
    // Pick the first player, since this is a diagnostic rather than a chooser.
    // Restricted to 1-4: a mixer announces itself too and has no media.
    ProLinkDevice target;
    for (const ProLinkDevice& device : m_devices) {
        if (device.isPlayer() && device.online) {
            target = device;
            break;
        }
    }
    if (!target.isValid()) {
        kLogger.warning() << "pull_db: no player on the network to pull from";
        return;
    }
    if (!m_pDiscovery) {
        return;
    }
    // Hop onto the network thread: everything below opens sockets.
    ProLinkDiscovery* pAnchor = m_pDiscovery;
    const ProLinkDevice device = target;
    QMetaObject::invokeMethod(
            pAnchor,
            [this, device, slot] { pullDatabaseOnNetworkThread(device, slot); },
            Qt::QueuedConnection);
}

void ProLinkNetworkService::pullDatabaseOnNetworkThread(
        const ProLinkDevice& device, MediaSlot slot) {
    const QString exportPath = exportPathForSlot(slot);
    kLogger.info() << "pull_db: mounting" << exportPath << "on" << device.label()
                   << device.address.toString();

    // Bound to the discovery object so they live and die on this thread. Both
    // are deleted when the transfer completes, via deleteLater on this thread's
    // own event loop -- which is running, unlike at shutdown.
    // The transfer is a *child* of the client, not a sibling. Deleting two
    // siblings meant the transfer died first and the client's teardown then
    // fired read callbacks that captured it -- a use-after-free. One owner, one
    // deleteLater, and Qt destroys the child first while the parent is intact.
    auto* pNfs = new nfs::NfsV2Client(device.address, localAddressFor(device), m_pDiscovery);
    auto* pTransfer = new nfs::NfsFileTransfer(pNfs, pNfs);

    pNfs->mount(exportPath,
            [this, pNfs, pTransfer, exportPath](
                    const nfs::NfsV2Client::Outcome<QByteArray>& mounted) {
                if (!mounted.ok) {
                    kLogger.warning() << "pull_db:" << mounted.error;
                    pNfs->deleteLater();
                    return;
                }
                kLogger.info() << "pull_db: mounted, mountd" << pNfs->mountdPort()
                               << "nfsd" << pNfs->nfsdPort();

                const QString pdbPath =
                        QStringLiteral("PIONEER/rekordbox/export.pdb");
                pNfs->resolvePath(mounted.value,
                        pdbPath,
                        [this, pNfs, pTransfer, exportPath, pdbPath](
                                const nfs::NfsV2Client::Outcome<QByteArray>& file) {
                            if (!file.ok) {
                                kLogger.warning() << "pull_db:" << file.error;
                                pNfs->deleteLater();
                                return;
                            }
                            pNfs->getAttributes(file.value,
                                    [this, pNfs, pTransfer, exportPath, file](
                                            const nfs::NfsV2Client::Outcome<
                                                    nfs::FileAttributes>& attrs) {
                                        if (!attrs.ok) {
                                            kLogger.warning() << "pull_db:" << attrs.error;
                                            pNfs->deleteLater();
                                            return;
                                        }
                                        kLogger.info() << "pull_db: export.pdb is"
                                                       << attrs.value.size << "bytes";
                                        pTransfer->fetch(file.value,
                                                attrs.value.size,
                                                [this, pNfs, pTransfer, exportPath](
                                                        const nfs::NfsFileTransfer::
                                                                Result& result) {
                                                    reportPull(result);
                                                    pNfs->unmount(exportPath);
                                                    pNfs->deleteLater();
                                                });
                                    });
                        });
            });
}

void ProLinkNetworkService::reportPull(const nfs::NfsFileTransfer::Result& result) {
    if (!result.ok) {
        kLogger.warning() << "pull_db FAILED after" << result.reads
                          << "reads:" << result.error;
        return;
    }
    // SHA-1 rather than a full parse: the point is to compare against the same
    // file read off the physically ejected stick, and byte-identical is the only
    // acceptable answer.
    //
    // Note the two header bytes a player rewrites as it operates -- a play count
    // or a history entry lands in the sequence counter at 0x14 (F13) -- so a
    // digest taken minutes apart can legitimately differ in those. Compare the
    // whole file first; if only bytes 0x10..0x18 differ, that is the player
    // writing its own bookkeeping, not a transfer error.
    const QByteArray digest =
            QCryptographicHash::hash(result.data, QCryptographicHash::Sha1).toHex();
    const double seconds = result.elapsedMs / 1000.0;
    const double kib = result.data.size() / 1024.0;
    kLogger.info() << "pull_db OK:" << result.data.size() << "bytes in"
                   << result.reads << "reads," << result.shortReads << "short,"
                   << result.elapsedMs << "ms,"
                   << (seconds > 0 ? qRound(kib / seconds) : 0) << "KiB/s";
    kLogger.info() << "pull_db sha1:" << digest;
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
