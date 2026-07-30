#include "network/prolink/prolinkvirtualcdj.h"

#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>

#include "moc_prolinkvirtualcdj.cpp"
#include "network/prolink/prolinkpacket.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkVirtualCdj");

/// Candidate numbers, highest first: take the top free slot and leave 1 and 2 —
/// the decks a DJ reaches for — alone as long as possible.
const QList<int> kCandidateOrder = {4, 3, 2, 1};
} // namespace

namespace mixxx {
namespace prolink {

namespace {
/// How many of each packet the handshake sends, in order.
///
/// The stage-4 count depends on whether we were first on the network, not on
/// the assignment mode as research/02 has it: three when first, one when
/// joining peers (C13).
QList<QPair<ProLinkVirtualCdj::State, int>> handshakeStages(bool firstOnNetwork) {
    return {{ProLinkVirtualCdj::State::Hello, 3},
            {ProLinkVirtualCdj::State::ClaimMac, 3},
            {ProLinkVirtualCdj::State::ClaimIp, 3},
            {ProLinkVirtualCdj::State::ClaimNumber, firstOnNetwork ? 3 : 1}};
}
} // namespace

ProLinkVirtualCdj::ProLinkVirtualCdj(QUdpSocket* pSocket, QObject* parent)
        : QObject(parent), m_pSocket(pSocket) {
    m_pStageTimer = new QTimer(this);
    m_pStageTimer->setSingleShot(true);
    connect(m_pStageTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Prescan) {
            finishPrescan();
        } else {
            advance();
        }
    });

    m_pKeepAlive = new QTimer(this);
    m_pKeepAlive->setInterval(kKeepAliveIntervalMs);
    connect(m_pKeepAlive, &QTimer::timeout, this, &ProLinkVirtualCdj::sendKeepAlive);
}

ProLinkVirtualCdj::~ProLinkVirtualCdj() {
    m_pStageTimer->stop();
    m_pKeepAlive->stop();
}

QString ProLinkVirtualCdj::stateName() const {
    switch (m_state) {
    case State::Idle:
        return QStringLiteral("idle");
    case State::Prescan:
        return QStringLiteral("watching");
    case State::Hello:
        return QStringLiteral("hello");
    case State::ClaimMac:
        return QStringLiteral("claiming (mac)");
    case State::ClaimIp:
        return QStringLiteral("claiming (ip)");
    case State::ClaimNumber:
        return QStringLiteral("claiming (number)");
    case State::Active:
        return QStringLiteral("announcing");
    case State::Failed:
        return QStringLiteral("failed");
    }
    return QString();
}

bool ProLinkVirtualCdj::resolveInterface(const QString& interfaceName) {
    const QNetworkInterface iface = QNetworkInterface::interfaceFromName(interfaceName);
    if (!iface.isValid()) {
        return false;
    }
    m_mac = QByteArray::fromHex(
            iface.hardwareAddress().remove(QLatin1Char(':')).toLatin1());
    for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
        if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
            continue;
        }
        m_address = entry.ip();
        // The subnet broadcast, not 255.255.255.255: a limited broadcast on a
        // multi-homed host goes out whichever interface the routing table
        // prefers, which on the deck Pi is wlan0 — the house LAN. Announcing
        // ourselves as a CDJ there would be both useless and rude.
        m_broadcast = entry.broadcast().isNull()
                ? QHostAddress(QHostAddress::Broadcast)
                : entry.broadcast();
        break;
    }
    return m_mac.size() == 6 && !m_address.isNull();
}

void ProLinkVirtualCdj::start(const QString& interfaceName, int peersAlreadyPresent) {
    if (m_state != State::Idle && m_state != State::Failed) {
        return;
    }
    if (!resolveInterface(interfaceName)) {
        enter(State::Failed,
                tr("no usable address on %1").arg(interfaceName));
        return;
    }
    m_interfaceName = interfaceName;

    // Latched once, as a real deck does at boot. Recomputing it as peers come
    // and go would make byte 0x25 flicker, which no real device does.
    m_firstOnNetwork = peersAlreadyPresent == 0;
    m_peerCount = peersAlreadyPresent + 1;

    m_candidates = kCandidateOrder;
    m_stage = 0;
    m_iteration = 0;
    enter(State::Prescan,
            tr("watching for %1 ms before claiming a number").arg(kPrescanMs));
    m_pStageTimer->start(kPrescanMs);
}

void ProLinkVirtualCdj::stop() {
    m_pStageTimer->stop();
    m_pKeepAlive->stop();
    // Deliberately no farewell packet: the protocol has none, and peers time us
    // out after five missed keep-alives, which is the same thing a real deck
    // being switched off looks like.
    m_deviceNumber = 0;
    enter(State::Idle, QString());
}

void ProLinkVirtualCdj::noteNumberInUse(int number) {
    if (number >= kMinPlayerNumber && number <= kMaxPlayerNumber) {
        m_numbersInUse.insert(number);
    }
}

void ProLinkVirtualCdj::enter(State state, const QString& detail) {
    m_state = state;
    if (!detail.isEmpty()) {
        kLogger.info() << stateName() << "-" << detail;
    }
    emit stateChanged(static_cast<int>(state), deviceNumber(), detail);
}

void ProLinkVirtualCdj::finishPrescan() {
    QList<int> free;
    for (const int number : m_candidates) {
        if (!m_numbersInUse.contains(number)) {
            free.append(number);
        }
    }
    if (free.isEmpty()) {
        // Four real players already. Nothing to claim, and taking one anyway
        // would kick a deck off mid-set.
        enter(State::Failed,
                tr("players %1 are all in use; nothing free to claim")
                        .arg(QStringLiteral("1-4")));
        return;
    }

    m_deviceNumber = free.takeFirst();
    m_candidates = free;
    m_stage = 0;
    m_iteration = 0;
    advance();
}

QByteArray ProLinkVirtualCdj::buildStagePacket() const {
    const QString name = QString::fromLatin1(kVirtualCdjName);
    switch (m_state) {
    case State::Hello:
        return packet::buildHello(name, DeviceKind::Cdj);
    case State::ClaimMac:
        return packet::buildClaimMac(name, DeviceKind::Cdj, m_iteration, m_mac);
    case State::ClaimIp:
        return packet::buildClaimIp(
                name, DeviceKind::Cdj, m_address, m_mac, m_deviceNumber, m_iteration);
    case State::ClaimNumber:
        return packet::buildClaimNumber(
                name, DeviceKind::Cdj, m_deviceNumber, m_iteration);
    default:
        return QByteArray();
    }
}

void ProLinkVirtualCdj::advance() {
    const auto stages = handshakeStages(m_firstOnNetwork);
    if (m_stage >= stages.size()) {
        enter(State::Active, tr("claimed device number %1").arg(m_deviceNumber));
        sendKeepAlive();
        m_pKeepAlive->start();
        return;
    }

    const State stageState = stages.at(m_stage).first;
    const int count = stages.at(m_stage).second;
    if (stageState != m_state) {
        m_state = stageState; // no enter(): a stage change is not news to the UI
    }
    ++m_iteration;
    send(buildStagePacket());

    if (m_iteration >= count) {
        ++m_stage;
        m_iteration = 0;
    }
    m_pStageTimer->start(kHandshakeIntervalMs);
}

void ProLinkVirtualCdj::sendKeepAlive() {
    send(packet::buildKeepAlive(QString::fromLatin1(kVirtualCdjName),
            DeviceKind::Cdj,
            m_deviceNumber,
            m_mac,
            m_address,
            m_peerCount,
            m_firstOnNetwork));
}

void ProLinkVirtualCdj::send(const QByteArray& datagram, const QHostAddress& to) {
    if (datagram.isEmpty() || m_pSocket == nullptr) {
        return;
    }
    const QHostAddress target = to.isNull() ? m_broadcast : to;
    if (m_pSocket->writeDatagram(datagram, target, kDiscoveryPort) < 0) {
        kLogger.warning() << "could not send to" << target << ":"
                          << m_pSocket->errorString();
    }
}

bool ProLinkVirtualCdj::handleDatagram(
        const QByteArray& datagram, const QHostAddress& sender) {
    const int type = packet::packetType(datagram);
    if (type < 0) {
        return false;
    }
    int contested = 0;
    if (!packet::parseContestedNumber(datagram, &contested)) {
        return false;
    }
    // Our own broadcasts come back to us on this socket. Ignoring by number
    // alone would be wrong -- a genuine conflict names our number too -- so
    // discard by source address first.
    if (sender == m_address) {
        return false;
    }

    if (type == static_cast<int>(PacketType::NumberConflict)) {
        if (contested == m_deviceNumber && m_state != State::Active) {
            backOff(contested, sender);
            return true;
        }
        return false;
    }

    // A claim. If it names the number we hold, defend it; if it names the one we
    // are still claiming, somebody got there first and we move on.
    if (contested != m_deviceNumber || m_deviceNumber == 0) {
        return false;
    }
    if (m_state == State::Active) {
        kLogger.info() << "defending device" << m_deviceNumber << "against" << sender;
        send(packet::buildNumberConflict(QString::fromLatin1(kVirtualCdjName),
                     DeviceKind::Cdj,
                     m_deviceNumber,
                     m_address),
                sender);
        return true;
    }
    backOff(contested, sender);
    return true;
}

void ProLinkVirtualCdj::backOff(int contested, const QHostAddress& challenger) {
    kLogger.warning() << "device" << contested << "is claimed by" << challenger
                      << "- backing off";
    m_numbersInUse.insert(contested);
    m_pStageTimer->stop();

    while (!m_candidates.isEmpty() && m_numbersInUse.contains(m_candidates.first())) {
        m_candidates.removeFirst();
    }
    if (m_candidates.isEmpty()) {
        m_deviceNumber = 0;
        enter(State::Failed, tr("every player number is taken"));
        return;
    }
    m_deviceNumber = m_candidates.takeFirst();
    // Back to stage 1, not 0: the hellos said "a device is arriving", which is
    // still true and does not need repeating.
    m_stage = 1;
    m_iteration = 0;
    enter(State::ClaimMac, tr("retrying as device %1").arg(m_deviceNumber));
    advance();
}

} // namespace prolink
} // namespace mixxx
