#include "network/prolink/prolinkdiscovery.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

#include "moc_prolinkdiscovery.cpp"
#include "network/prolink/prolinkpacket.h"
#include "network/prolink/prolinkvirtualcdj.h"
#include "network/prolink/prolinkmediaquery.h"
#include "network/prolink/prolinkbeatlistener.h"
#include "network/prolink/server/prolinkserver.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkDiscovery");

/// How often to re-ask, and how many times, before concluding a slot is empty.
/// Five rounds over fifteen seconds covers a deck that is mid-track-load when we
/// first ask, without asking forever about a slot that has nothing in it.
constexpr int kMediaRetryIntervalMs = 3000;
constexpr int kMediaQueryAttempts = 5;
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
        m_pVirtualCdj = new ProLinkVirtualCdj(m_pSocket, this);
        connect(m_pVirtualCdj,
                &ProLinkVirtualCdj::stateChanged,
                this,
                &ProLinkDiscovery::announceStateChanged);

        m_pMediaQuery = new ProLinkMediaQuery(this);
        connect(m_pVirtualCdj,
                &ProLinkVirtualCdj::stateChanged,
                this,
                [this](int, int deviceNumber, const QString&) {
                    m_pMediaQuery->setRequesterNumber(deviceNumber);
                    // The serve side is inert without a number too, and for the
                    // same reason in reverse: a deck validates the number in
                    // every dbserver request, and a status packet from device 0
                    // is worse than none at all.
                    if (m_pServer) {
                        m_pServer->setDeviceNumber(deviceNumber);
                    }
                    // The moment we have a number is the first moment a player
                    // will answer us, so ask straight away rather than waiting
                    // for the user to expand something.
                    if (deviceNumber > 0) {
                        queryAllMedia();
                    }
                });
        connect(m_pMediaQuery,
                &ProLinkMediaQuery::mediaInfoReceived,
                this,
                [this](const QHostAddress& player, MediaSlot slot, const MediaInfo& info) {
                    // Map the responder's address back to a MAC: that is what
                    // the library layer keys everything on, and a device number
                    // can be reassigned mid-session.
                    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
                        if (it->address == player) {
                            // Answered: stop asking. Marked with the cap
                            // rather than by removing the entry -- removing it
                            // reset the counter to zero, so the one slot that
                            // *did* answer was the one we re-asked every three
                            // seconds forever, while the silent ones correctly
                            // gave up.
                            m_mediaQueryAttempts.insert(
                                    QStringLiteral("%1|%2")
                                            .arg(QString::fromLatin1(it->mac.toHex()))
                                            .arg(static_cast<int>(slot)),
                                    kMediaQueryAttempts);
                            emit mediaInfoFound(it->mac, slot, info);
                            return;
                        }
                    }
                });

        m_pBeatListener = new ProLinkBeatListener(this);

        m_pMediaRetry = new QTimer(this);
        m_pMediaRetry->setInterval(kMediaRetryIntervalMs);
        connect(m_pMediaRetry, &QTimer::timeout, this, [this] { queryAllMedia(); });
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
    if (m_pServer) {
        // First: it holds two TCP listeners and three UDP sockets of its own,
        // and its status timer writes to the media query's socket.
        m_pServer->stop();
    }
    if (m_pMediaRetry) {
        m_pMediaRetry->stop();
    }
    if (m_pBeatListener) {
        m_pBeatListener->stop();
    }
    if (m_pMediaQuery) {
        m_pMediaQuery->stop();
    }
    if (m_pVirtualCdj) {
        // Before the socket closes: the announcer writes to it on a timer, and
        // a tick landing on a closed socket would log a warning per second for
        // the rest of shutdown.
        m_pVirtualCdj->stop();
    }
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

        // Offered to the announcer first, and unconditionally: conflicts and
        // claims identify nobody, so parseAnnouncement rejects them, but they
        // are exactly the packets a claim in progress turns on.
        if (m_pVirtualCdj) {
            m_pVirtualCdj->handleDatagram(data, datagram.senderAddress());
        }

        ProLinkDevice device;
        if (!packet::parseAnnouncement(data, &device)) {
            continue;
        }

        // Our own broadcasts come back to us on this socket. Without this we
        // appear in our own sidebar as another CDJ -- offering to browse
        // ourselves, over a network round trip -- and we inflate the peer count
        // we then advertise in every keep-alive.
        if (m_pVirtualCdj && device.mac == m_pVirtualCdj->mac() &&
                !device.mac.isEmpty()) {
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

        if (m_pVirtualCdj) {
            // Feeds the pre-scan. A number seen in use is never free, however
            // quiet its holder is afterwards -- XDJ-XZ and Opus Quad do not
            // defend theirs at all, so silence proves nothing.
            m_pVirtualCdj->noteNumberInUse(device.deviceNumber);
            m_pVirtualCdj->setPeerCount(m_devices.size() + 1);
        }

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
    // Once a second, which is as responsive as a peer list needs to be. This
    // has to be on the *reaper*, not on the refresh path: status is unicast per
    // peer, so an empty list means we transmit nothing at all and a deck never
    // learns we hold media — which looks exactly like the serve side being
    // broken rather than the peer list being empty.
    updateServerPeers();
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
    updateServerPeers();
}

void ProLinkDiscovery::updateServerPeers() {
    if (!m_pServer) {
        return;
    }
    QList<QHostAddress> peers;
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        if (it->online && it->address.protocol() == QAbstractSocket::IPv4Protocol) {
            peers.append(it->address);
        }
    }
    m_pServer->setPeers(peers);
}

QString ProLinkDiscovery::serverSummary() const {
    return m_pServer ? m_pServer->summary() : QString();
}


void ProLinkDiscovery::startAnnouncing() {
    if (!m_pVirtualCdj || !isListening()) {
        return;
    }
    // Announce on the interface a player was actually seen on. Guessing would
    // mean broadcasting DJ-Link onto the house LAN on a multi-homed host, which
    // is the deck Pi's normal configuration.
    QString interfaceName;
    int peers = 0;
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        if (!it->online) {
            continue;
        }
        ++peers;
        if (interfaceName.isEmpty() && !it->interfaceName.isEmpty()) {
            interfaceName = it->interfaceName;
        }
    }
    if (interfaceName.isEmpty()) {
        kLogger.info() << "not announcing yet: no player seen, so no interface to"
                       << "announce on";
        return;
    }
    m_pVirtualCdj->start(interfaceName, peers);
    // Bound here rather than in start(), because it is only worth having once
    // we are going to announce: an unannounced host receives nothing on 50002.
    m_pMediaQuery->start();
    m_pMediaRetry->start();

    // The serve side comes up with the 50002 socket, and shares it: a media or
    // settings query arrives *on* 50002 and its answer must leave from 50002,
    // so a second socket would reply from an ephemeral port and the deck would
    // ignore it. Constructed here rather than above because the socket does not
    // exist until ProLinkMediaQuery::start() has made it.
    if (!m_pServer && m_pMediaQuery->socket()) {
        m_pServer = new server::ProLinkServer(m_pMediaQuery->socket(), this);
        m_pMediaQuery->setStatusServer(m_pServer->statusServer());
        m_pServer->setDeviceNumber(m_pVirtualCdj->deviceNumber());
        m_pServer->start();
    }
    // Beat packets are broadcast, so this would work without announcing -- but
    // there is nothing to compare a phase against until we are on the network
    // anyway, so it starts here with the rest.
    m_pBeatListener->start();

    // We may have already admitted ourselves: start() is the first moment our
    // own MAC is known, and on a restart our previous keep-alives can still be
    // in flight.
    const QByteArray ownMac = m_pVirtualCdj->mac();
    if (!ownMac.isEmpty() && m_devices.remove(ownMac) > 0) {
        m_reportedStale.remove(ownMac);
        emit deviceLost(ownMac);
    }
}

int ProLinkDiscovery::announcedNumber() const {
    return m_pVirtualCdj ? m_pVirtualCdj->deviceNumber() : 0;
}

QString ProLinkDiscovery::announceStatus() const {
    return m_pVirtualCdj ? m_pVirtualCdj->stateName() : QStringLiteral("off");
}

int ProLinkDiscovery::masterDevice() const {
    return m_pMediaQuery ? m_pMediaQuery->masterDevice() : 0;
}

double ProLinkDiscovery::masterBpm() const {
    if (!m_pBeatListener) {
        return 0.0;
    }
    return m_pBeatListener->phaseFor(masterDevice()).bpm;
}

double ProLinkDiscovery::masterBarPhase() const {
    if (!m_pBeatListener) {
        return -1.0;
    }
    const BeatPhase phase = m_pBeatListener->phaseFor(masterDevice());
    // Invalid means the master is paused, or there is no master: beat packets
    // stop the moment the platter does. -1 rather than 0 so the meter can show
    // "nothing to sync to" instead of a marker parked on the downbeat.
    return phase.isValid() ? phase.barPhase() : -1.0;
}

void ProLinkDiscovery::queryAllMedia(bool force) {
    if (!m_pMediaQuery || !m_pVirtualCdj || m_pVirtualCdj->deviceNumber() == 0) {
        return;
    }
    if (force) {
        m_mediaQueryAttempts.clear();
    }
    const QHostAddress ours = m_pVirtualCdj->address();
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        if (!it->online || !it->isPlayer()) {
            continue;
        }
        // Both slots, unconditionally. There is no reply that means "empty" --
        // a deck simply says nothing about a slot with nothing in it -- so the
        // only way to find out is to ask and see, which is also why this is
        // capped rather than open-ended.
        for (const MediaSlot slot : {MediaSlot::Usb, MediaSlot::Sd}) {
            const QString key = QStringLiteral("%1|%2")
                                        .arg(QString::fromLatin1(it->mac.toHex()))
                                        .arg(static_cast<int>(slot));
            const int attempts = m_mediaQueryAttempts.value(key);
            if (attempts >= kMediaQueryAttempts) {
                continue;
            }
            m_mediaQueryAttempts.insert(key, attempts + 1);
            m_pMediaQuery->query(it->address, ours, it->deviceNumber, slot);
        }
    }
}

} // namespace prolink
} // namespace mixxx
