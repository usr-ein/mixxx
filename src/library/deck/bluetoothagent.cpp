#include "library/deck/bluetoothagent.h"

#include <QDBusConnection>

#include "moc_bluetoothagent.cpp"
#include "util/logger.h"

namespace mixxx {
namespace deck {

namespace {
const Logger kLogger("BluetoothAgent");
const QString kRejected = QStringLiteral("org.bluez.Error.Rejected");
const QString kCanceled = QStringLiteral("org.bluez.Error.Canceled");
} // namespace

BluetoothAgent::BluetoothAgent(std::function<bool(const QString&)> allowed, QObject* pParent)
        : QObject(pParent),
          m_allowed(std::move(allowed)) {
}

void BluetoothAgent::answer(bool accept) {
    if (!m_hasPending) {
        return;
    }
    QDBusConnection::systemBus().send(accept
                    ? m_pending.createReply()
                    : m_pending.createErrorReply(kRejected, QStringLiteral("refused on the deck")));
    kLogger.info() << "pairing" << (accept ? "confirmed" : "refused") << "on the deck";
    endPending();
}

void BluetoothAgent::refusePending() {
    if (!m_hasPending) {
        return;
    }
    QDBusConnection::systemBus().send(
            m_pending.createErrorReply(kCanceled, QStringLiteral("the menu closed")));
    endPending();
}

void BluetoothAgent::endPending() {
    m_pending = QDBusMessage();
    m_hasPending = false;
    emit confirmationEnded();
}

void BluetoothAgent::Release() {
    // bluetoothd going, or the agent unregistered: nothing can be answered.
    if (m_hasPending) {
        endPending();
    }
}

QString BluetoothAgent::RequestPinCode(const QDBusObjectPath& device) {
    // A legacy PIN (pre-2.1 devices) needs typing on the deck, which it has no
    // keyboard for: refused, as an old phone would be by any speaker.
    kLogger.info() << "refused a PIN request from" << device.path();
    sendErrorReply(kRejected, QStringLiteral("no PIN entry on this deck"));
    return QString();
}

void BluetoothAgent::DisplayPinCode(const QDBusObjectPath& device, const QString& pincode) {
    Q_UNUSED(device);
    Q_UNUSED(pincode);
    sendErrorReply(kRejected, QStringLiteral("no PIN pairing on this deck"));
}

uint BluetoothAgent::RequestPasskey(const QDBusObjectPath& device) {
    kLogger.info() << "refused a passkey request from" << device.path();
    sendErrorReply(kRejected, QStringLiteral("no passkey entry on this deck"));
    return 0;
}

void BluetoothAgent::DisplayPasskey(
        const QDBusObjectPath& device, uint passkey, ushort entered) {
    Q_UNUSED(entered);
    emit passkeyShown(device.path(), passkey);
}

void BluetoothAgent::RequestConfirmation(const QDBusObjectPath& device, uint passkey) {
    // One at a time: a second phone asking while a code is on screen is
    // refused, rather than swapping the code under the DJ's eyes.
    if (m_hasPending) {
        sendErrorReply(kRejected, QStringLiteral("another pairing is in progress"));
        return;
    }
    setDelayedReply(true);
    m_pending = message();
    m_hasPending = true;
    kLogger.info() << "code to confirm for" << device.path();
    emit confirmationRequested(device.path(), passkey);
}

void BluetoothAgent::RequestAuthorization(const QDBusObjectPath& device) {
    // Just-works pairing: no code at all. Only in the Pair window.
    if (!m_allowed(device.path())) {
        sendErrorReply(kRejected, QStringLiteral("pairing is not open"));
    }
}

void BluetoothAgent::AuthorizeService(const QDBusObjectPath& device, const QString& uuid) {
    if (!m_allowed(device.path())) {
        kLogger.info() << "refused service" << uuid << "to" << device.path();
        sendErrorReply(kRejected, QStringLiteral("not the device picked on the deck"));
    }
}

void BluetoothAgent::Cancel() {
    // The phone gave up, or BlueZ timed it out.
    if (m_hasPending) {
        kLogger.info() << "pairing cancelled by the phone";
        endPending();
    }
}

} // namespace deck
} // namespace mixxx
