#pragma once

#include <QByteArray>
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
    QString name;
    int deviceNumber = 0;

    /// Whether the discovery table considered this device present when the
    /// signal carrying it was emitted.
    ///
    /// Because deviceChanged is deliberately not emitted for plain keep-alives
    /// (four a second per deck would be noise), a mirror held by the GUI thread
    /// can be minutes old while the device is perfectly alive: this is the
    /// field to ask, not the age of the copy.
    bool online = true;
};

} // namespace prolink
} // namespace mixxx

// Crosses the network/GUI thread boundary in queued signals, which requires the
// type to be a registered metatype.
Q_DECLARE_METATYPE(mixxx::prolink::ProLinkDevice)
