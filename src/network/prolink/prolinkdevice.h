#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QString>

#include "network/prolink/prolinkdefs.h"

namespace mixxx {
namespace prolink {

/// One player, mixer or other device seen on the Pro DJ Link network.
///
/// A value type, so it can cross the thread boundary between the network thread
/// and the GUI as a plain copy in a queued signal.
///
/// **Identity is the MAC, not the device number and not the IP.** All three
/// change: a link-local address is self-assigned and can differ across boots, a
/// player in automatic mode picks a number that need not be the one it had
/// yesterday, and a DJ can renumber a deck mid-session from its UTILITY screen.
/// The MAC is the only field that stays put, so it is what the peer table keys
/// on — otherwise a renumbered deck appears twice and the original never times
/// out.
class ProLinkDevice {
  public:
    ProLinkDevice() = default;

    QByteArray mac;
    QHostAddress address;
    /// The literal 20-byte name field, NUL padding included, alongside the
    /// trimmed string. Kept because the padding is part of what makes an
    /// announcement byte-identical to a real one, and we will need to reproduce
    /// it exactly when we start announcing (F1, F23).
    QByteArray nameRaw;
    QString name;
    int deviceNumber = 0;
    DeviceKind kind = DeviceKind::Cdj;

    /// Name of the local interface the announcement arrived on.
    ///
    /// Recorded per device because the deck Pi is multi-homed — `eth0` on the
    /// CDJ network and `wlan0` — and a broadcast keep-alive can arrive on
    /// either. Every socket we later open towards this device must bind a source
    /// address on the same subnet, or link-local routing silently picks the
    /// wrong NIC and every RPC times out with no error to point at.
    QString interfaceName;

    /// Whether the discovery table considered this device present when the
    /// signal carrying it was emitted.
    ///
    /// **Consumers must use this, not isStale().** The timer below is
    /// owner-side state: it keeps running inside a copy, so asking a copy how
    /// long ago the device was seen actually answers "how long since this copy
    /// was made" -- a different and useless question. Because deviceChanged is
    /// deliberately not emitted for plain keep-alives (four a second per deck
    /// would be noise), a mirror held by the GUI thread can be minutes old while
    /// the device is perfectly alive.
    bool online = true;

    /// Milliseconds since the last keep-alive. **Only meaningful on the
    /// instance owned by ProLinkDiscovery**, which is the one being touched.
    /// Driven by a monotonic clock, so it is immune to the system clock stepping
    /// (NTP settling on a Pi with no battery-backed RTC, which is exactly our
    /// case).
    qint64 msSinceLastSeen() const {
        return m_lastSeen.isValid() ? m_lastSeen.elapsed() : -1;
    }
    void touch() {
        m_lastSeen.start();
    }

    /// Missed at least five keep-alives. Owner-side only -- see `online`.
    /// Shown greyed out, but its subtree, database rows and cache are all kept.
    /// See kDeviceTimeoutMs.
    bool isStale() const {
        const qint64 elapsed = msSinceLastSeen();
        return elapsed < 0 || elapsed > kDeviceTimeoutMs;
    }
    /// Gone long enough to actually remove.
    bool isExpired() const {
        const qint64 elapsed = msSinceLastSeen();
        return elapsed < 0 || elapsed > kDeviceRemovalGraceMs;
    }

    bool isValid() const {
        return !mac.isEmpty() && !address.isNull();
    }

    /// Could this device be browsed over dbserver, if we asked?
    ///
    /// Only players 1–4 participate in linked playback; the same range that a
    /// *server* must sit in to be offered at all (F45).
    bool isPlayer() const {
        return deviceNumber >= kMinPlayerNumber && deviceNumber <= kMaxPlayerNumber;
    }

    /// "1 · CDJ-2000nexus", the sidebar label. Number first because that is how
    /// a DJ refers to a deck and how the deck's own display labels itself.
    QString label() const {
        return QStringLiteral("%1 · %2").arg(deviceNumber).arg(name);
    }

    QString macString() const;

    /// Same physical device? Compares the MAC only, per the note above.
    bool sameDeviceAs(const ProLinkDevice& other) const {
        return !mac.isEmpty() && mac == other.mac;
    }

  private:
    QElapsedTimer m_lastSeen;
};

} // namespace prolink
} // namespace mixxx

// Crosses the network/GUI thread boundary in queued signals, which requires the
// type to be a registered metatype.
Q_DECLARE_METATYPE(mixxx::prolink::ProLinkDevice)
