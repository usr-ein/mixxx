#include "network/prolink/prolinknetworkservice.h"

#include <QMetaObject>

#include "moc_prolinknetworkservice.cpp"
#include "network/prolink/prolinkdiscovery.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNetworkService");
/// How long shutdown waits for the network thread before giving up on it.
/// Generous: the thread only ever has to finish one datagram or one timer tick,
/// so exceeding this means something is genuinely wedged, and hanging Mixxx's
/// exit would be worse than leaking a thread.
constexpr int kThreadQuitTimeoutMs = 2000;
} // namespace

namespace mixxx {
namespace prolink {

ProLinkNetworkService::ProLinkNetworkService(QObject* parent)
        : QObject(parent) {
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
