#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QMetaType>
#include <QString>

/// The values that cross from ProLinkNetworkService to the rest of Mixxx: a
/// slot, a device, what a device has in a slot.
///
/// The protocol itself is `lib/prolink`'s, in Rust. The prolinks-compat repo's
/// `docs/PROTOCOL.md` is its specification and `docs/FINDINGS.md` records how
/// each fact was established; finding numbers (F*n*) refer to that second
/// document.
///
/// No includes from `src/library/`: everything under `src/network/prolink/`
/// must stay usable, and unit-testable, without a Library.
namespace mixxx {
namespace prolink {

/// The slot byte of a dbserver request descriptor, and the discriminator when
/// one connection carries two media (F37).
enum class MediaSlot : quint8 {
    Empty = 0,
    Cd = 1,
    Sd = 2,
    Usb = 3,
    Rekordbox = 4,
};

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

/// What a player says is in one of its slots.
struct MediaInfo {
    /// The volume label the DJ formatted the medium with — `Sam CDJ1000mk3`,
    /// and what the deck itself shows. UTF-16 **big**-endian on the wire, like
    /// the dbserver strings and unlike the NFS layer's UTF-16LE.
    ///
    /// **Often empty, and legitimately so**: an unlabelled stick reports no
    /// name at all while still carrying a full library. Absence of a name is
    /// not absence of media — that is what `isOccupied()` is for.
    QString name;
    quint32 trackCount = 0;
    quint32 playlistCount = 0;

    /// A slot with something in it. A deck answers for an empty slot too, with
    /// everything zeroed, so this is the distinction that matters.
    ///
    /// Occupancy is published in status packets and nowhere else, and those are
    /// unicast only to peers that have announced themselves (F20/F21).
    bool isOccupied() const {
        return trackCount > 0 || !name.isEmpty();
    }
};

} // namespace prolink
} // namespace mixxx

// Both cross the network/GUI thread boundary in queued signals, which requires
// a registered metatype.
Q_DECLARE_METATYPE(mixxx::prolink::ProLinkDevice)
Q_DECLARE_METATYPE(mixxx::prolink::MediaInfo)
