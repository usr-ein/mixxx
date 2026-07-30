#include "network/prolink/prolinkdiscovery.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

#include "moc_prolinkdiscovery.cpp"
#include "network/prolink/prolinkpacket.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkDiscovery");
} // namespace

namespace mixxx {
namespace prolink {

ProLinkDiscovery::ProLinkDiscovery(QObject* parent)
        : QObject(parent) {
}

bool ProLinkDiscovery::start() {
    if (isListening()) {
        return true;
    }
    if (!m_pSocket) {
        m_pSocket = new QUdpSocket(this);
        connect(m_pSocket,
                &QUdpSocket::readyRead,
                this,
                &ProLinkDiscovery::readPendingDatagrams);
    }

    // ShareAddress | ReuseAddressHint so we coexist with anything else already
    // listening -- rekordbox, prolink-tools, a second Mixxx. The two hints mean
    // different things per platform (SO_REUSEPORT on macOS, SO_REUSEADDR on
    // Windows) and passing both is the portable way to ask for "let me share".
    //
    // AnyIPv4 rather than a specific address: these are broadcasts, and on a
    // multi-homed host binding one interface's address would miss the others.
    // Which interface each device arrived on is recorded per device instead.
    const bool bound = m_pSocket->bind(QHostAddress::AnyIPv4,
            kDiscoveryPort,
            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound) {
        m_lastError = m_pSocket->errorString();
        kLogger.warning() << "could not bind UDP" << kDiscoveryPort << ":"
                          << m_lastError;
        return false;
    }
    m_lastError.clear();

    if (!m_pReaper) {
        m_pReaper = new QTimer(this);
        m_pReaper->setInterval(kReaperIntervalMs);
        connect(m_pReaper, &QTimer::timeout, this, &ProLinkDiscovery::reapDevices);
    }
    m_pReaper->start();

    kLogger.info() << "listening on UDP" << kDiscoveryPort;
    return true;
}

void ProLinkDiscovery::stop() {
    if (m_pReaper) {
        m_pReaper->stop();
    }
    if (m_pSocket) {
        m_pSocket->close();
    }
    // Deliberately *not* emitting deviceLost for each: stop() runs during
    // shutdown, and delivering signals to a feature that is being destroyed is
    // exactly the crash R5 in the plan warns about.
    m_devices.clear();
    m_reportedStale.clear();
}

bool ProLinkDiscovery::isListening() const {
    return m_pSocket && m_pSocket->state() == QAbstractSocket::BoundState;
}

QList<ProLinkDevice> ProLinkDiscovery::devices() const {
    return m_devices.values();
}

QString ProLinkDiscovery::interfaceForAddress(const QHostAddress& address) {
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            if (address.isInSubnet(entry.ip(), entry.prefixLength())) {
                return iface.name();
            }
        }
    }
    return QString();
}

void ProLinkDiscovery::readPendingDatagrams() {
    VERIFY_OR_DEBUG_ASSERT(m_pSocket) {
        return;
    }
    while (m_pSocket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_pSocket->receiveDatagram();
        const QByteArray data = datagram.data();

        ProLinkDevice device;
        if (!packet::parseAnnouncement(data, &device)) {
            continue;
        }

        // Trust the datagram's source address over the address inside it. They
        // agree in every capture we have, but the sender field is something a
        // device writes about itself, while the source address is what we must
        // actually send replies to.
        if (!datagram.senderAddress().isNull()) {
            device.address = datagram.senderAddress();
        }
        device.interfaceName = interfaceForAddress(device.address);

        auto existing = m_devices.find(device.mac);
        if (existing == m_devices.end()) {
            device.online = true;
            m_devices.insert(device.mac, device);
            kLogger.info() << "found" << device.label() << "at" << device.address
                           << "on" << device.interfaceName;
            emit deviceFound(device);
            continue;
        }

        // Known device. Keep the existing entry and refresh it, so the
        // QElapsedTimer inside it keeps running rather than restarting from a
        // fresh copy -- and only signal when something a view would render has
        // actually changed. A deck sends four status-bearing packets a second;
        // emitting on each would be pure noise.
        const bool wasStale = m_reportedStale.contains(device.mac);
        const bool changed = existing->deviceNumber != device.deviceNumber ||
                existing->address != device.address ||
                existing->name != device.name;
        existing->deviceNumber = device.deviceNumber;
        existing->address = device.address;
        existing->name = device.name;
        existing->nameRaw = device.nameRaw;
        existing->kind = device.kind;
        existing->interfaceName = device.interfaceName;
        existing->touch();

        existing->online = true;
        if (wasStale) {
            m_reportedStale.remove(device.mac);
            kLogger.info() << existing->label() << "is back";
        }
        if (changed || wasStale) {
            emit deviceChanged(*existing);
        }
    }
}

void ProLinkDiscovery::reapDevices() {
    QList<QByteArray> expired;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (it->isExpired()) {
            expired.append(it.key());
            continue;
        }
        // Went stale since the last tick: report it once. The device stays in
        // the table, and its subtree, database rows and cache all stay with it,
        // because a player can drop off for two seconds on a cable jiggle or a
        // switch reconverging and come straight back.
        if (it->isStale() && !m_reportedStale.contains(it.key())) {
            m_reportedStale.insert(it.key());
            it->online = false;
            kLogger.info() << it->label() << "went offline";
            emit deviceChanged(*it);
        }
    }
    for (const QByteArray& mac : expired) {
        const ProLinkDevice device = m_devices.take(mac);
        m_reportedStale.remove(mac);
        kLogger.info() << "removing" << device.label();
        emit deviceLost(mac);
    }
}

void ProLinkDiscovery::forgetStaleDevices() {
    QList<QByteArray> stale;
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        if (it->isStale()) {
            stale.append(it.key());
        }
    }
    for (const QByteArray& mac : stale) {
        m_devices.remove(mac);
        m_reportedStale.remove(mac);
        emit deviceLost(mac);
    }
}

} // namespace prolink
} // namespace mixxx
