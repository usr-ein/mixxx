#include "library/deck/bluetoothreceiver.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSysInfo>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/deck/bluetoothagent.h"
#include "library/deck/screenpower.h"
#include "moc_bluetoothreceiver.cpp"
#include "soundio/soundmanager.h"
#include "util/logger.h"

using BluezInterfaces = QMap<QString, QVariantMap>;
using BluezObjects = QMap<QDBusObjectPath, BluezInterfaces>;
Q_DECLARE_METATYPE(BluezInterfaces)
Q_DECLARE_METATYPE(BluezObjects)

namespace mixxx {
namespace deck {

namespace {
const Logger kLogger("BluetoothReceiver");

const QString kHelper = QStringLiteral("/usr/local/sbin/trimixxx-bluetooth");
const QString kGroup = QStringLiteral("[Bluetooth]");

const QString kBluez = QStringLiteral("org.bluez");
const QString kObjectManager = QStringLiteral("org.freedesktop.DBus.ObjectManager");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kAdapter = QStringLiteral("org.bluez.Adapter1");
const QString kDevice = QStringLiteral("org.bluez.Device1");
const QString kPlayer = QStringLiteral("org.bluez.MediaPlayer1");
const QString kTransport = QStringLiteral("org.bluez.MediaTransport1");
const QString kAgentManager = QStringLiteral("org.bluez.AgentManager1");

const QString kObex = QStringLiteral("org.bluez.obex");
const QString kObexClient = QStringLiteral("org.bluez.obex.Client1");
const QString kObexSession = QStringLiteral("org.bluez.obex.Session1");
const QString kObexImage = QStringLiteral("org.bluez.obex.Image1");
const QString kObexTransfer = QStringLiteral("org.bluez.obex.Transfer1");

/// How long a reacquire waits between tries while the card is busy: the
/// player that held it can take a moment to close it.
constexpr int kReacquireRetryMs = 1000;
/// Tries before the toast says it is not getting better.
constexpr int kReacquireQuietTries = 3;
/// How long after a refused cover session to look for another client's: one
/// still connecting when we looked is up by then.
constexpr int kCoverSessionLookAgainMs = 2000;

/// A unit's cgroup is there exactly while it runs: a look at whether a crash
/// left the radio up, without a process or a bus.
bool leftUp() {
    return QFileInfo::exists(
                   QStringLiteral("/sys/fs/cgroup/system.slice/trimixxx-bluetoothd.service")) ||
            QFileInfo::exists(QStringLiteral(
                    "/sys/fs/cgroup/system.slice/trimixxx-bluetooth-guard.service"));
}

/// A D-Bus value as a plain map: a dictionary inside a variant arrives as a
/// QDBusArgument still to be read.
QVariantMap mapOf(const QVariant& value) {
    if (value.canConvert<QDBusArgument>()) {
        return qdbus_cast<QVariantMap>(value.value<QDBusArgument>());
    }
    return value.toMap();
}

/// A property as BlueZ sent it, unwrapped where it came wrapped.
QVariant plain(const QVariant& value) {
    if (value.canConvert<QDBusVariant>()) {
        return value.value<QDBusVariant>().variant();
    }
    return value;
}

QVariantMap plainMap(const QVariantMap& properties) {
    QVariantMap out;
    for (auto it = properties.cbegin(); it != properties.cend(); ++it) {
        out.insert(it.key(), plain(it.value()));
    }
    return out;
}

QString objectPathOf(const QVariant& value) {
    return value.value<QDBusObjectPath>().path();
}

/// What the phone calls itself: its alias, else its name, else its address.
QString deviceName(const QVariantMap& device) {
    for (const char* key : {"Alias", "Name", "Address"}) {
        const QString name = device.value(QString::fromLatin1(key)).toString();
        if (!name.isEmpty()) {
            return name;
        }
    }
    return QString();
}
} // namespace

BluetoothReceiver::BluetoothReceiver(const QString& deckGroup, QObject* pParent)
        : QObject(pParent),
          m_group(deckGroup),
          m_pActive(std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("active")))),
          m_pScreenOff(std::make_unique<ControlObject>(
                  ConfigKey(kGroup, QStringLiteral("screen_off")))),
          // Every set is news, the same value again included: the mapping
          // sets it to 1 at each control it hears.
          m_pWake(std::make_unique<ControlObject>(
                  ConfigKey(kGroup, QStringLiteral("wake")), false)) {
    qDBusRegisterMetaType<BluezInterfaces>();
    qDBusRegisterMetaType<BluezObjects>();
    m_clock.start();
    m_reacquireRetry.setSingleShot(true);
    connect(&m_reacquireRetry, &QTimer::timeout, this, &BluetoothReceiver::reacquireSound);

    // A screen a crash left off is lit again, and what a crash left up goes
    // down, before Mixxx opens the sound card
    // (MixxxMainWindow::initialize(), well after this): the player would hold
    // it. The guard normally gets there first; this is for whatever it did
    // not. A look at two folders when nothing is up, which is every start.
    screenpower::recover();
    if (leftUp()) {
        kLogger.warning() << "Bluetooth was left up (a crash?): taking it down";
        QProcess down;
        down.start(QStringLiteral("sudo"),
                QStringList{
                        QStringLiteral("-n"), kHelper, QStringLiteral("down")});
        if (!down.waitForFinished(15000) || down.exitCode() != 0) {
            kLogger.warning() << "could not take Bluetooth down:"
                              << QString::fromUtf8(down.readAllStandardError()).trimmed();
        }
    }

    m_pAgent = new BluetoothAgent(
            [this](const QString& device) { return allowed(device); }, this);
    connect(m_pAgent,
            &BluetoothAgent::confirmationRequested,
            this,
            [this](const QString& device, quint32 passkey) {
                m_pairingPath = device;
                m_facts.hasPasskey = true;
                m_facts.passkey = passkey;
                m_facts.pairingName = deviceName(m_objects.value(device).value(kDevice));
                emit changed();
            });
    connect(m_pAgent,
            &BluetoothAgent::passkeyShown,
            this,
            [this](const QString& device, quint32 passkey) {
                // The phone types it: shown as the same question, answered
                // there.
                m_pairingPath = device;
                m_facts.hasPasskey = true;
                m_facts.passkey = passkey;
                m_facts.pairingName =
                        deviceName(m_objects.value(device).value(kDevice));
                emit changed();
            });
    connect(m_pAgent, &BluetoothAgent::confirmationEnded, this, [this]() {
        m_facts.hasPasskey = false;
        m_facts.passkey = 0;
        emit changed();
    });

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        kLogger.warning() << "no system bus: no Bluetooth receiver";
        return;
    }
    bus.registerObject(QString::fromLatin1(BluetoothAgent::kPath),
            m_pAgent,
            QDBusConnection::ExportScriptableSlots);
    m_pWatcher = new QDBusServiceWatcher(kBluez,
            bus,
            QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration,
            this);
    connect(m_pWatcher,
            &QDBusServiceWatcher::serviceRegistered,
            this,
            &BluetoothReceiver::onBluezRegistered);
    connect(m_pWatcher,
            &QDBusServiceWatcher::serviceUnregistered,
            this,
            &BluetoothReceiver::onBluezUnregistered);
}

BluetoothReceiver::~BluetoothReceiver() {
    shutdown();
}

void BluetoothReceiver::setSoundManager(SoundManager* pSoundManager) {
    m_pSoundManager = pSoundManager;
}

void BluetoothReceiver::shutdown() {
    m_pSoundManager = nullptr;
    m_reacquireRetry.stop();
    // Mixxx quitting with the menu open: the radio goes now, and the card is
    // the sound manager's to close.
    const bool up = m_wanted || m_pHelper || m_facts.radio != bluetooth::Facts::Radio::Off;
    m_wanted = false;
    if (!up) {
        return;
    }
    m_pAgent->refusePending();
    if (m_pHelper) {
        m_pHelper->disconnect(this);
        m_pHelper->waitForFinished(15000);
        m_pHelper->deleteLater();
        m_pHelper = nullptr;
    }
    QProcess down;
    down.start(QStringLiteral("sudo"),
            QStringList{QStringLiteral("-n"), kHelper, QStringLiteral("down")});
    down.waitForFinished(15000);
    m_facts = bluetooth::Facts();
    kLogger.info() << "Bluetooth down: Mixxx is quitting";
}

void BluetoothReceiver::setActive(bool active) {
    if (active == m_wanted) {
        return;
    }
    m_wanted = active;
    m_pActive->set(active ? 1.0 : 0.0);
    kLogger.info() << (active ? "the menu opened" : "the menu closed");
    if (!active) {
        m_pAgent->refusePending();
        m_pScreenOff->set(0.0);
    }
    // Nothing connects until the DJ picks it, on every opening (Sam).
    m_pickedPath.clear();
    m_connectingPath.clear();
    m_pairingPath.clear();
    m_refused.clear();
    advance();
}

void BluetoothReceiver::advance() {
    if (m_pHelper) {
        return; // advance() again when it ends
    }
    if (m_wanted) {
        if (!m_soundReleased) {
            pauseDeck();
            releaseSound();
        }
        if (m_facts.radio == bluetooth::Facts::Radio::Off) {
            runHelper(Helper::Up);
        }
        return;
    }
    if (m_facts.radio != bluetooth::Facts::Radio::Off) {
        disconnectBluez();
        runHelper(Helper::Down);
        return;
    }
    if (m_soundReleased) {
        reacquireSound();
    }
}

void BluetoothReceiver::runHelper(Helper action) {
    QStringList args{QStringLiteral("-n"), kHelper};
    if (action == Helper::Up) {
        args << QStringLiteral("up") << QString::number(QCoreApplication::applicationPid());
        m_facts = bluetooth::Facts();
        m_facts.radio = bluetooth::Facts::Radio::Starting;
        emit changed();
    } else {
        args << QStringLiteral("down");
    }
    m_pHelper = new QProcess(this);
    connect(m_pHelper,
            &QProcess::finished,
            this,
            [this, action](int exitCode, QProcess::ExitStatus status) {
                const QString said = QString::fromUtf8(m_pHelper->readAllStandardError()).trimmed();
                onHelperFinished(action, status == QProcess::CrashExit ? -1 : exitCode, said);
            });
    connect(m_pHelper,
            &QProcess::errorOccurred,
            this,
            [this, action](QProcess::ProcessError error) {
                // The only error that is not followed by finished().
                if (error == QProcess::FailedToStart) {
                    onHelperFinished(action, -1, m_pHelper->errorString());
                }
            });
    // sudo -n: a password asked for would wait for ever.
    m_pHelper->start(QStringLiteral("sudo"), args);
}

void BluetoothReceiver::onHelperFinished(Helper action, int exitCode, const QString& said) {
    m_pHelper->deleteLater();
    m_pHelper = nullptr;
    kLogger.info() << "trimixxx-bluetooth" << (action == Helper::Up ? "up" : "down")
                   << "ended:" << exitCode << said;
    if (action == Helper::Up) {
        if (exitCode == 0) {
            m_facts.radio = bluetooth::Facts::Radio::Up;
            // BlueZ may already be on the bus: the watcher only tells of a
            // change.
            if (m_pWatcher &&
                    QDBusConnection::systemBus().interface()->isServiceRegistered(kBluez).value()) {
                connectBluez();
            }
        } else {
            m_facts.radio = bluetooth::Facts::Radio::Failed;
            m_facts.failure = said.section(QChar('\n'), -1);
            emit notice(tr("Bluetooth would not start"));
        }
        emit changed();
    } else {
        m_facts = bluetooth::Facts();
        clearCover();
        emit changed();
    }
    advance();
}

void BluetoothReceiver::pauseDeck() {
    // Made here rather than with the controls: the deck's own controls do not
    // exist yet when DeckServices is made.
    if (!m_pPlay) {
        m_pPlay = std::make_unique<ControlProxy>(m_group, QStringLiteral("play"), this);
        // While the menu is open nothing plays the deck: Mixxx's engine has
        // no card to run on, and a deck set playing would start the moment
        // the menu closed.
        m_pPlay->connectValueChanged(this, [this](double value) {
            if (m_wanted && value > 0.0) {
                m_pPlay->set(0.0);
            }
        });
    }
    m_pPlay->set(0.0);
}

void BluetoothReceiver::releaseSound() {
    if (!m_pSoundManager) {
        return;
    }
    m_reacquireRetry.stop();
    m_pSoundManager->releaseDevices();
    m_soundReleased = true;
    kLogger.info() << "the sound card let go of";
}

void BluetoothReceiver::reacquireSound() {
    if (m_wanted || !m_soundReleased || !m_pSoundManager || m_pHelper) {
        return;
    }
    const SoundDeviceStatus status = m_pSoundManager->setupDevices();
    if (status == SoundDeviceStatus::Ok) {
        if (m_reacquireTries >= kReacquireQuietTries) {
            emit notice(tr("Sound back on the deck"));
        }
        m_soundReleased = false;
        m_reacquireTries = 0;
        kLogger.info() << "the sound card taken back";
        return;
    }
    // Busy, most likely: the player closing it. Tried again until it works,
    // and said once it is clearly not a moment's wait.
    ++m_reacquireTries;
    kLogger.warning() << "could not take the sound card back (try" << m_reacquireTries
                      << "):" << m_pSoundManager->getLastErrorMessage(status);
    if (m_reacquireTries == kReacquireQuietTries) {
        emit notice(tr("No sound: the sound card is busy. Trying again…"));
    }
    m_reacquireRetry.start(kReacquireRetryMs);
}

void BluetoothReceiver::pair() {
    if (m_adapterPath.isEmpty()) {
        return;
    }
    setAdapterProperty(QStringLiteral("Pairable"), true);
    setAdapterProperty(QStringLiteral("Discoverable"), true);
}

void BluetoothReceiver::answerPairing(bool accept) {
    m_pAgent->answer(accept);
}

QString BluetoothReceiver::deckName() const {
    const QString alias = m_objects.value(m_adapterPath)
                                  .value(kAdapter)
                                  .value(QStringLiteral("Alias"))
                                  .toString();
    return alias.isEmpty() ? QSysInfo::machineHostName() : alias;
}

bool BluetoothReceiver::allowed(const QString& device) const {
    const QVariantMap properties = m_objects.value(device).value(kDevice);
    return bluetooth::mayConnect(device,
            m_pickedPath,
            properties.value(QStringLiteral("Paired")).toBool(),
            m_facts.discoverable);
}

void BluetoothReceiver::pick(const QString& path) {
    const QVariantMap device = m_objects.value(path).value(kDevice);
    if (device.isEmpty() || !device.value(QStringLiteral("Paired")).toBool()) {
        return;
    }
    kLogger.info() << "picked" << deviceName(device);
    m_pickedPath = path;
    m_refused.remove(path);
    // Trusted is what lets a device in without the agent being asked: the
    // picked one only, so another paired device trying to connect or take
    // over is asked about, and refused (allowed()).
    trustOnly(path);
    // One source at a time: whatever else is connected goes.
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (it.key() != path &&
                it.value()
                        .value(kDevice)
                        .value(QStringLiteral("Connected"))
                        .toBool()) {
            callDevice(it.key(), QStringLiteral("Disconnect"));
        }
    }
    if (!device.value(QStringLiteral("Connected")).toBool()) {
        m_connectingPath = path;
        QDBusMessage call = QDBusMessage::createMethodCall(
                kBluez, path, kDevice, QStringLiteral("Connect"));
        // A phone out of range or with its radio off fails after a while:
        // said, and it stays picked, so it may still come in by itself.
        auto* pWatcher = new QDBusPendingCallWatcher(
                QDBusConnection::systemBus().asyncCall(call, 30000), this);
        connect(pWatcher,
                &QDBusPendingCallWatcher::finished,
                this,
                [this, path](QDBusPendingCallWatcher* pCall) {
                    pCall->deleteLater();
                    QDBusPendingReply<> reply = *pCall;
                    if (path != m_connectingPath) {
                        return;
                    }
                    m_connectingPath.clear();
                    if (reply.isError()) {
                        const QString name = deviceName(
                                m_objects.value(path).value(kDevice));
                        kLogger.info() << name << "did not connect:"
                                       << reply.error().message();
                        emit notice(tr("%1 did not answer").arg(name));
                    }
                    recompute();
                });
    }
    recompute();
}

void BluetoothReceiver::forget(const QString& path) {
    const QVariantMap device = m_objects.value(path).value(kDevice);
    if (device.isEmpty() || m_adapterPath.isEmpty()) {
        return;
    }
    const QString name = deviceName(device);
    kLogger.info() << "forgetting" << name;
    if (path == m_pickedPath) {
        m_pickedPath.clear();
        m_connectingPath.clear();
    }
    // BlueZ disconnects it and deletes what it kept of it, its keys
    // included (on p8, on a release card): it pairs again to come back.
    QDBusMessage call = QDBusMessage::createMethodCall(
            kBluez, m_adapterPath, kAdapter, QStringLiteral("RemoveDevice"));
    call << QVariant::fromValue(QDBusObjectPath(path));
    auto* pWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(call), this);
    connect(pWatcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, name](QDBusPendingCallWatcher* pCall) {
                pCall->deleteLater();
                QDBusPendingReply<> reply = *pCall;
                if (reply.isError()) {
                    kLogger.warning() << "could not forget" << name << ":"
                                      << reply.error().message();
                    emit notice(tr("Could not forget %1").arg(name));
                    return;
                }
                emit notice(tr("%1 forgotten").arg(name));
            });
    recompute();
}

void BluetoothReceiver::trustOnly(const QString& path) {
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        const QVariantMap device = it.value().value(kDevice);
        if (device.isEmpty() || !device.value(QStringLiteral("Paired")).toBool()) {
            continue;
        }
        const bool trusted = it.key() == path;
        if (device.value(QStringLiteral("Trusted")).toBool() == trusted) {
            continue; // BlueZ writes the device's file on a change only
        }
        QDBusMessage call = QDBusMessage::createMethodCall(
                kBluez, it.key(), kProperties, QStringLiteral("Set"));
        call << kDevice << QStringLiteral("Trusted") << QVariant::fromValue(QDBusVariant(trusted));
        QDBusConnection::systemBus().asyncCall(call);
    }
}

void BluetoothReceiver::callDevice(const QString& path, const QString& method) {
    QDBusMessage call = QDBusMessage::createMethodCall(kBluez, path, kDevice, method);
    QDBusConnection::systemBus().asyncCall(call);
}

void BluetoothReceiver::onBluezRegistered() {
    if (m_facts.radio == bluetooth::Facts::Radio::Up) {
        connectBluez();
    }
}

void BluetoothReceiver::onBluezUnregistered() {
    // bluetoothd went (a crash, or the menu closing): whatever it said is
    // gone with it, and it is listened to again if it comes back.
    if (m_bluezConnected) {
        stopListening();
    }
    m_pAgent->refusePending();
    recompute();
}

void BluetoothReceiver::connectBluez() {
    if (m_bluezConnected) {
        return;
    }
    m_bluezConnected = true;
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(kBluez,
            QStringLiteral("/"),
            kObjectManager,
            QStringLiteral("InterfacesAdded"),
            this,
            SLOT(onInterfacesAdded(QDBusMessage)));
    bus.connect(kBluez,
            QStringLiteral("/"),
            kObjectManager,
            QStringLiteral("InterfacesRemoved"),
            this,
            SLOT(onInterfacesRemoved(QDBusMessage)));
    // Every object's: an empty path matches them all.
    bus.connect(kBluez,
            QString(),
            kProperties,
            QStringLiteral("PropertiesChanged"),
            this,
            SLOT(onPropertiesChanged(QDBusMessage)));
    readManagedObjects();
    registerAgent();
}

void BluetoothReceiver::disconnectBluez() {
    if (!m_bluezConnected) {
        return;
    }
    // Left untrusted, so that the next opening starts with nothing let in
    // even in the moment before this Mixxx has read BlueZ's objects.
    trustOnly(QString());
    m_pAgent->refusePending();
    QDBusMessage unregister = QDBusMessage::createMethodCall(kBluez,
            QStringLiteral("/org/bluez"),
            kAgentManager,
            QStringLiteral("UnregisterAgent"));
    unregister << QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(BluetoothAgent::kPath)));
    QDBusConnection::systemBus().asyncCall(unregister);
    stopListening();
}

void BluetoothReceiver::stopListening() {
    m_bluezConnected = false;
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.disconnect(kBluez,
            QStringLiteral("/"),
            kObjectManager,
            QStringLiteral("InterfacesAdded"),
            this,
            SLOT(onInterfacesAdded(QDBusMessage)));
    bus.disconnect(kBluez,
            QStringLiteral("/"),
            kObjectManager,
            QStringLiteral("InterfacesRemoved"),
            this,
            SLOT(onInterfacesRemoved(QDBusMessage)));
    bus.disconnect(kBluez,
            QString(),
            kProperties,
            QStringLiteral("PropertiesChanged"),
            this,
            SLOT(onPropertiesChanged(QDBusMessage)));
    m_objects.clear();
}

void BluetoothReceiver::readManagedObjects() {
    QDBusMessage call = QDBusMessage::createMethodCall(
            kBluez, QStringLiteral("/"), kObjectManager, QStringLiteral("GetManagedObjects"));
    auto* pWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(call), this);
    connect(pWatcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this](QDBusPendingCallWatcher* pCall) {
                pCall->deleteLater();
                QDBusPendingReply<BluezObjects> reply = *pCall;
                if (reply.isError()) {
                    kLogger.warning() << "BlueZ's objects could not be read:"
                                      << reply.error().message();
                    return;
                }
                const BluezObjects objects = reply.value();
                for (auto object = objects.cbegin(); object != objects.cend();
                        ++object) {
                    for (auto interface = object.value().cbegin();
                            interface != object.value().cend();
                            ++interface) {
                        store(object.key().path(),
                                interface.key(),
                                interface.value());
                    }
                }
                // Nothing trusted, nothing picked: no paired device comes in by
                // itself, a crash's leftover pick included.
                trustOnly(m_pickedPath);
                recompute();
            });
}

void BluetoothReceiver::registerAgent() {
    const QDBusObjectPath path(QString::fromLatin1(BluetoothAgent::kPath));
    QDBusMessage call = QDBusMessage::createMethodCall(
            kBluez, QStringLiteral("/org/bluez"), kAgentManager, QStringLiteral("RegisterAgent"));
    // A screen, and a yes/no: numeric comparison with every phone that has a
    // screen of its own.
    call << QVariant::fromValue(path) << QStringLiteral("DisplayYesNo");
    auto* pWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(call), this);
    connect(pWatcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, path](QDBusPendingCallWatcher* pCall) {
                pCall->deleteLater();
                QDBusPendingReply<> reply = *pCall;
                if (reply.isError()) {
                    kLogger.warning() << "the pairing agent was refused:"
                                      << reply.error().message();
                    return;
                }
                QDBusMessage call = QDBusMessage::createMethodCall(kBluez,
                        QStringLiteral("/org/bluez"),
                        kAgentManager,
                        QStringLiteral("RequestDefaultAgent"));
                call << QVariant::fromValue(path);
                QDBusConnection::systemBus().asyncCall(call);
            });
}

void BluetoothReceiver::onInterfacesAdded(const QDBusMessage& message) {
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2) {
        return;
    }
    const QString path = objectPathOf(args.at(0));
    const BluezInterfaces interfaces =
            qdbus_cast<BluezInterfaces>(args.at(1).value<QDBusArgument>());
    for (auto it = interfaces.cbegin(); it != interfaces.cend(); ++it) {
        store(path, it.key(), it.value());
    }
    recompute();
}

void BluetoothReceiver::onInterfacesRemoved(const QDBusMessage& message) {
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2) {
        return;
    }
    const QString path = objectPathOf(args.at(0));
    for (const QString& interface : args.at(1).toStringList()) {
        m_objects[path].remove(interface);
    }
    if (m_objects.value(path).isEmpty()) {
        m_objects.remove(path);
    }
    recompute();
}

void BluetoothReceiver::onPropertiesChanged(const QDBusMessage& message) {
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2) {
        return;
    }
    const QString interface = args.at(0).toString();
    const QVariantMap changes = mapOf(args.at(1));
    store(message.path(), interface, changes);
    recompute();
}

void BluetoothReceiver::store(
        const QString& path, const QString& interface, const QVariantMap& properties) {
    QVariantMap& stored = m_objects[path][interface];
    const QVariantMap changes = plainMap(properties);
    if (interface == kPlayer) {
        // The track moves on by itself between the positions BlueZ reports:
        // each one restarts the clock, and so does a change of status, at
        // the position it had reached.
        const qint64 now = m_clock.elapsed();
        if (changes.contains(QStringLiteral("Position"))) {
            m_facts.positionMs = changes.value(QStringLiteral("Position")).toLongLong();
            m_facts.positionStampMs = now;
        } else if (changes.contains(QStringLiteral("Status")) && path == m_playerPath) {
            m_facts.positionMs = bluetooth::positionNow(m_facts, now);
            m_facts.positionStampMs = now;
        }
    }
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        stored.insert(it.key(), it.value());
    }
}

void BluetoothReceiver::recompute() {
    m_adapterPath.clear();
    QList<bluetooth::Device> devices;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (m_adapterPath.isEmpty() && it.value().contains(kAdapter)) {
            m_adapterPath = it.key();
        }
        const QVariantMap device = it.value().value(kDevice);
        if (device.value(QStringLiteral("Paired")).toBool()) {
            bluetooth::Device listed;
            listed.path = it.key();
            listed.name = deviceName(device);
            listed.connected = device.value(QStringLiteral("Connected")).toBool();
            listed.picked = it.key() == m_pickedPath;
            devices.append(listed);
        }
    }
    m_facts.devices = bluetooth::sortedDevices(devices);
    const QVariantMap adapter = m_objects.value(m_adapterPath).value(kAdapter);
    m_facts.adapter = !m_adapterPath.isEmpty();
    m_facts.discoverable = adapter.value(QStringLiteral("Discoverable")).toBool();

    // A phone just paired becomes the one picked, and plays (Sam).
    QString justPaired;
    if (!m_pairingPath.isEmpty()) {
        const QVariantMap paired = m_objects.value(m_pairingPath).value(kDevice);
        if (paired.value(QStringLiteral("Paired")).toBool()) {
            kLogger.info() << "paired with" << deviceName(paired);
            emit notice(tr("Paired with %1").arg(deviceName(paired)));
            justPaired = m_pairingPath;
            m_pickedPath = justPaired;
            m_pairingPath.clear();
            // Visible no longer than it takes: the phone is in.
            setAdapterProperty(QStringLiteral("Discoverable"), false);
        }
    }

    // A paired device in without being picked, in the moment before this
    // Mixxx untrusted it say, goes again: one source, the one picked.
    for (const bluetooth::Device& listed : std::as_const(devices)) {
        if (!listed.connected) {
            m_refused.remove(listed.path);
        } else if (listed.path != m_pickedPath && listed.path != m_pairingPath &&
                !m_refused.contains(listed.path)) {
            m_refused.insert(listed.path);
            kLogger.info() << "refused" << listed.name << ": not the device picked";
            callDevice(listed.path, QStringLiteral("Disconnect"));
        }
    }

    // The phone: the device picked, once it is in; never another.
    const QVariantMap picked = m_objects.value(m_pickedPath).value(kDevice);
    const QString phonePath =
            picked.value(QStringLiteral("Connected")).toBool() ? m_pickedPath : QString();
    const QString phoneName = phonePath.isEmpty() ? QString() : deviceName(picked);
    if (!phonePath.isEmpty() && phonePath == m_connectingPath) {
        m_connectingPath.clear();
    }
    m_facts.connectingName = m_connectingPath.isEmpty()
            ? QString()
            : deviceName(m_objects.value(m_connectingPath).value(kDevice));
    if (phonePath != m_phonePath) {
        if (!phonePath.isEmpty()) {
            emit notice(tr("%1 connected").arg(phoneName));
        }
        m_phonePath = phonePath;
        // A cover session is the phone's: another phone needs its own.
        m_obexSession.clear();
    }
    m_facts.phoneName = phoneName;

    // Its player and its audio: the ones whose Device is the phone.
    m_playerPath.clear();
    m_facts.streaming = false;
    for (auto it = m_objects.cbegin(); it != m_objects.cend(); ++it) {
        if (m_phonePath.isEmpty()) {
            break;
        }
        const QVariantMap player = it.value().value(kPlayer);
        if (m_playerPath.isEmpty() && !player.isEmpty() &&
                objectPathOf(player.value(QStringLiteral("Device"))) == m_phonePath) {
            m_playerPath = it.key();
        }
        const QVariantMap transport = it.value().value(kTransport);
        if (!transport.isEmpty() &&
                objectPathOf(transport.value(QStringLiteral("Device"))) == m_phonePath &&
                transport.value(QStringLiteral("State")).toString() == QStringLiteral("active")) {
            m_facts.streaming = true;
        }
    }
    const QVariantMap player = m_objects.value(m_playerPath).value(kPlayer);
    m_facts.playerStatus = player.value(QStringLiteral("Status")).toString();
    const QVariantMap track = mapOf(player.value(QStringLiteral("Track")));
    m_facts.track.title = track.value(QStringLiteral("Title")).toString();
    m_facts.track.artist = track.value(QStringLiteral("Artist")).toString();
    m_facts.track.album = track.value(QStringLiteral("Album")).toString();
    m_facts.track.durationMs = track.value(QStringLiteral("Duration")).toLongLong();
    m_facts.track.imageHandle = track.value(QStringLiteral("ImgHandle")).toString();
    if (m_playerPath.isEmpty()) {
        m_facts.positionMs = 0;
    }

    if (m_facts.track.imageHandle != m_coverHandle) {
        fetchCover();
    }
    emit changed();
    if (!justPaired.isEmpty()) {
        pick(justPaired);
    }
}

void BluetoothReceiver::setAdapterProperty(const QString& name, const QVariant& value) {
    if (m_adapterPath.isEmpty()) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
            kBluez, m_adapterPath, kProperties, QStringLiteral("Set"));
    call << kAdapter << name << QVariant::fromValue(QDBusVariant(value));
    QDBusConnection::systemBus().asyncCall(call);
}

void BluetoothReceiver::clearCover() {
    if (!m_obexFile.isEmpty()) {
        QFile::remove(m_obexFile);
    }
    m_coverHandle.clear();
    m_obexTransfer.clear();
    m_obexFile.clear();
    m_coverPath.clear();
}

void BluetoothReceiver::fetchCover() {
    clearCover();
    m_coverHandle = m_facts.track.imageHandle;
    // A cover only when the phone offers one: a handle for this track, and
    // the port its imaging server listens on. Without either, the view draws
    // the rune.
    if (m_coverHandle.isEmpty() || coverPort() == 0 || phoneAddress().isEmpty() ||
            !QDBusConnection::sessionBus().isConnected()) {
        return;
    }
    if (!m_obexSession.isEmpty()) {
        requestThumbnail();
        return;
    }
    findCoverSession(true);
}

quint16 BluetoothReceiver::coverPort() const {
    const QVariantMap player = m_objects.value(m_playerPath).value(kPlayer);
    return static_cast<quint16>(player.value(QStringLiteral("ObexPort")).toUInt());
}

QString BluetoothReceiver::phoneAddress() const {
    return m_objects.value(m_phonePath).value(kDevice).value(QStringLiteral("Address")).toString();
}

void BluetoothReceiver::findCoverSession(bool orOpen) {
    // The phone may have a cover session open already: Pi OS's mpris-proxy
    // opens one for every phone that offers covers, and a phone can take a
    // single one (Android's serves one client at all), which would refuse
    // ours. obexd lets any client use a session, so that one is taken, and
    // one is opened only when there is none.
    QDBusMessage call = QDBusMessage::createMethodCall(
            kObex, QStringLiteral("/"), kObjectManager, QStringLiteral("GetManagedObjects"));
    auto* pWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(call), this);
    const QString handle = m_coverHandle;
    const QString phone = m_phonePath;
    connect(pWatcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, handle, phone, orOpen](QDBusPendingCallWatcher* pCall) {
                pCall->deleteLater();
                if (handle != m_coverHandle || phone != m_phonePath) {
                    return; // the track or the phone moved on meanwhile
                }
                QDBusPendingReply<BluezObjects> reply = *pCall;
                const BluezObjects objects = reply.isError() ? BluezObjects() : reply.value();
                const QString address = phoneAddress();
                for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
                    const QString destination = plainMap(it.value().value(kObexSession))
                                                        .value(QStringLiteral("Destination"))
                                                        .toString();
                    if (it.value().contains(kObexImage) &&
                            destination.compare(address, Qt::CaseInsensitive) == 0) {
                        kLogger.info() << "covers through the session open to the phone:"
                                       << it.key().path();
                        m_obexSession = it.key().path();
                        requestThumbnail();
                        return;
                    }
                }
                if (orOpen) {
                    openCoverSession();
                }
            });
}

void BluetoothReceiver::openCoverSession() {
    QDBusMessage call = QDBusMessage::createMethodCall(
            kObex, QStringLiteral("/org/bluez/obex"), kObexClient, QStringLiteral("CreateSession"));
    QVariantMap options;
    options.insert(QStringLiteral("Target"), QStringLiteral("bip-avrcp"));
    options.insert(QStringLiteral("PSM"), QVariant::fromValue(coverPort()));
    call << phoneAddress() << options;
    auto* pWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(call), this);
    const QString handle = m_coverHandle;
    const QString phone = m_phonePath;
    connect(pWatcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, handle, phone](QDBusPendingCallWatcher* pCall) {
                pCall->deleteLater();
                QDBusPendingReply<QDBusObjectPath> reply = *pCall;
                if (reply.isError()) {
                    kLogger.info() << "no cover session:" << reply.error().message();
                    // Refused, most likely for another client's session still
                    // connecting when we looked: look once more when it can
                    // be there.
                    QTimer::singleShot(kCoverSessionLookAgainMs, this, [this, handle, phone]() {
                        if (handle == m_coverHandle && phone == m_phonePath &&
                                m_obexSession.isEmpty()) {
                            findCoverSession(false);
                        }
                    });
                    return;
                }
                m_obexSession = reply.value().path();
                if (handle == m_coverHandle && phone == m_phonePath) {
                    requestThumbnail();
                }
            });
}

void BluetoothReceiver::requestThumbnail() {
    // The thumbnail: 200 px square, JPEG, which is what the view draws. No
    // target file: obexd makes one and says where (Filename).
    QDBusMessage call = QDBusMessage::createMethodCall(
            kObex, m_obexSession, kObexImage, QStringLiteral("GetThumbnail"));
    call << QString() << m_coverHandle;
    auto* pWatcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(call), this);
    const QString handle = m_coverHandle;
    connect(pWatcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, handle](QDBusPendingCallWatcher* pCall) {
                pCall->deleteLater();
                QDBusPendingReply<QDBusObjectPath, QVariantMap> reply = *pCall;
                if (reply.isError()) {
                    kLogger.info() << "no cover:" << reply.error().message();
                    // The session may be gone (another client's ends with it):
                    // the next track looks for one again.
                    m_obexSession.clear();
                    return;
                }
                if (handle != m_coverHandle) {
                    return; // the track moved on meanwhile
                }
                m_obexTransfer = reply.argumentAt<0>().path();
                m_obexFile = reply.argumentAt<1>()
                                     .value(QStringLiteral("Filename"))
                                     .toString();
                QDBusConnection::sessionBus().connect(kObex,
                        m_obexTransfer,
                        kProperties,
                        QStringLiteral("PropertiesChanged"),
                        this,
                        SLOT(onObexPropertiesChanged(QDBusMessage)));
            });
}

void BluetoothReceiver::onObexPropertiesChanged(const QDBusMessage& message) {
    if (message.path() != m_obexTransfer) {
        return;
    }
    const QList<QVariant> args = message.arguments();
    if (args.size() < 2 || args.at(0).toString() != kObexTransfer) {
        return;
    }
    const QString status = plainMap(mapOf(args.at(1))).value(QStringLiteral("Status")).toString();
    if (status == QStringLiteral("complete")) {
        m_coverPath = m_obexFile;
        emit changed();
    } else if (status == QStringLiteral("error")) {
        kLogger.info() << "the cover transfer failed";
    }
}

} // namespace deck
} // namespace mixxx
