#pragma once

#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QObject>
#include <functional>

namespace mixxx {
namespace deck {

/// The deck's half of pairing, as BlueZ asks it: org.bluez.Agent1, on the
/// system bus, registered by BluetoothReceiver while the radio is up.
///
/// The deck has a screen and a button to say yes with (DisplayYesNo), so a
/// phone pairing with it is numeric comparison: both show the same six digits,
/// and the DJ confirms on both (Sam: the Pair button and a code). The
/// confirmation waits here, as a reply BlueZ is not given until the DJ answers
/// on the deck (answer()), BlueZ cancels it (Cancel(), the phone's ~30 s), or
/// the menu closes (refusePending()).
///
/// Everything a phone may do without a code -- just-works pairing, a profile
/// asked for by a phone not yet trusted -- is allowed only while the Pair
/// window is open or for a phone already paired: *allowed* says which.
class BluetoothAgent : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Agent1")

  public:
    static constexpr const char* kPath = "/org/mixxx/deck/bluetooth/agent";

    /// *allowed*: whether a device (BlueZ's object path) may pair or connect
    /// without a code.
    explicit BluetoothAgent(std::function<bool(const QString& device)> allowed,
            QObject* pParent = nullptr);

    /// The DJ's answer to the code on screen.
    void answer(bool accept);
    /// Whatever waits is refused: the menu is closing.
    void refusePending();
    bool hasPending() const {
        return m_hasPending;
    }

  signals:
    /// A code to show and confirm: *passkey*, from the phone at *device*.
    void confirmationRequested(const QString& device, quint32 passkey);
    /// The phone types the code (it has a keyboard, no screen): only shown.
    void passkeyShown(const QString& device, quint32 passkey);
    /// Nothing waits any more: answered, cancelled, or the agent released.
    void confirmationEnded();

  public slots:
    // org.bluez.Agent1, called by bluetoothd.
    Q_SCRIPTABLE void Release();
    Q_SCRIPTABLE QString RequestPinCode(const QDBusObjectPath& device);
    Q_SCRIPTABLE void DisplayPinCode(const QDBusObjectPath& device, const QString& pincode);
    Q_SCRIPTABLE uint RequestPasskey(const QDBusObjectPath& device);
    Q_SCRIPTABLE void DisplayPasskey(const QDBusObjectPath& device, uint passkey, ushort entered);
    Q_SCRIPTABLE void RequestConfirmation(const QDBusObjectPath& device, uint passkey);
    Q_SCRIPTABLE void RequestAuthorization(const QDBusObjectPath& device);
    Q_SCRIPTABLE void AuthorizeService(const QDBusObjectPath& device, const QString& uuid);
    Q_SCRIPTABLE void Cancel();

  private:
    void endPending();

    std::function<bool(const QString&)> m_allowed;
    /// RequestConfirmation's call, answered later.
    QDBusMessage m_pending;
    bool m_hasPending = false;
};

} // namespace deck
} // namespace mixxx
