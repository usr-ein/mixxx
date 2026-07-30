#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include "network/prolink/prolinkdevice.h"

class QUdpSocket;
class QTimer;

namespace mixxx {
namespace prolink {

/// Listens on UDP 50000 and maintains the table of devices on the network.
///
/// **Entirely passive: this class never transmits.** Discovery works by
/// listening to broadcasts every player already sends, so Mixxx can be plugged
/// into a live rig with no risk whatsoever of contending for a device number or
/// confusing a peer. Announcing is a separate, later, opt-in component.
///
/// The cost of passivity is that slot occupancy is invisible: a player unicasts
/// status only to peers that have announced themselves (F21), and media presence
/// is published there and nowhere else (F20). So this reports *which devices
/// exist*, not what is in them.
///
/// Lives on the network thread. Everything it emits is a value type, so the
/// signals cross to the GUI thread as plain copies.
class ProLinkDiscovery : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkDiscovery(QObject* parent = nullptr);
    ~ProLinkDiscovery() override = default;

    /// Bind the socket and start listening. Must be called on the thread this
    /// object lives on -- the socket is created here rather than in the
    /// constructor precisely so that it belongs to the right thread.
    ///
    /// Returns false if the port could not be bound, which is a normal thing to
    /// happen: rekordbox, prolink-tools or a second Mixxx may already hold it.
    /// The caller reports that to the user rather than treating it as fatal.
    bool start();
    void stop();

    bool isListening() const;
    /// Why the bind failed, for the preferences status area. Empty on success.
    QString lastError() const {
        return m_lastError;
    }

    QList<ProLinkDevice> devices() const;
    int deviceCount() const {
        return m_devices.size();
    }

    /// Drop every device that is currently stale, without waiting out the
    /// removal grace period. Bound to the `[ProLink],refresh` control.
    void forgetStaleDevices();

  signals:
    /// A device we had not seen before, or one that had been removed and is back.
    void deviceFound(const mixxx::prolink::ProLinkDevice& device);
    /// An existing device whose number, address or online state changed. Not
    /// emitted for a plain keep-alive, which would be four signals a second per
    /// deck for no new information.
    void deviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void deviceLost(const QByteArray& mac);

  private slots:
    void readPendingDatagrams();
    void reapDevices();

  private:
    /// Which local interface an address belongs to, for the multi-homed case.
    static QString interfaceForAddress(const QHostAddress& address);

    /// Raw pointers parented to `this`, not parented_ptr: both are created
    /// lazily in start(), and parented_ptr is deliberately non-assignable.
    /// Ownership is Qt's -- they die with this object, on its own thread.
    QUdpSocket* m_pSocket = nullptr;
    QTimer* m_pReaper = nullptr;
    /// Keyed on MAC. See the note in ProLinkDevice about why not on number.
    QHash<QByteArray, ProLinkDevice> m_devices;
    /// Which devices we have already told the world are stale, so the "went
    /// offline" signal fires once rather than once per reaper tick.
    QSet<QByteArray> m_reportedStale;
    QString m_lastError;
};

} // namespace prolink
} // namespace mixxx
