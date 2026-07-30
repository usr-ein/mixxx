#pragma once

#include <QtGlobal>

/// Constants for Pioneer's Pro DJ Link protocol.
///
/// Every value here was observed on two CDJ-2000NXS running firmware 1.44 and
/// is documented, with the capture it came from, in the prolinks-compat repo:
/// `docs/PROTOCOL.md` is the specification and `docs/FINDINGS.md` records how
/// each fact was established. Finding numbers (F*n*, C*n*) below refer to that
/// second document, so a constant that looks arbitrary can be traced to a
/// packet rather than to somebody's guess.
///
/// This header deliberately contains no Qt types beyond `qint`/`quint` and no
/// includes from `src/library/`: everything under `src/network/prolink/` must
/// stay usable, and unit-testable, without a Library.
namespace mixxx {
namespace prolink {

/// UDP, broadcast. Discovery, device-number claiming, keep-alive.
constexpr quint16 kDiscoveryPort = 50000;
/// UDP. Beat and sync. Not implemented.
constexpr quint16 kBeatPort = 50001;
/// UDP, **unicast only**. Status, media query, device settings.
///
/// A player sends status to peers that have announced themselves and to nobody
/// else: 1507 status packets in one session all went deck-to-deck, and not one
/// reached a host that was on the network the whole time without announcing
/// (F21). So a passive listener sees nothing here, and slot occupancy — which
/// is published at offsets 0x6f and 0x73 of the status packet and *nowhere
/// else* (F20) — is invisible until we announce.
constexpr quint16 kStatusPort = 50002;
/// TCP. "Which port is your dbserver on?" — a fixed 19-byte query, 2-byte reply.
constexpr quint16 kDbServerQueryPort = 12523;
/// TCP. The metadata protocol the LINK button drives.
constexpr quint16 kDbServerPort = 1051;
/// UDP. ONC RPC portmapper.
///
/// Privileged, and unavoidably so for the serve side: a deck asks the
/// portmapper for the mountd and nfsd ports *before* it opens dbserver, retries
/// once a second indefinitely if nothing answers, and never falls back to the
/// well-known ports even when those are bound and idle (F46). Nothing in the
/// consume direction needs it.
constexpr quint16 kPortmapPort = 111;

/// Present at offset 0 of every Pro DJ Link datagram, on all three UDP ports.
constexpr char kMagic[] = "Qspt1WmJOL";
constexpr int kMagicLength = 10;

/// Length of the NUL-padded device-name field. `CDJ-2000nexus` is the exact
/// casing, captured literally rather than inferred (F1).
constexpr int kDeviceNameLength = 20;

/// Smallest datagram that can carry a complete header, on either port layout.
constexpr int kMinPacketLength = 0x25;

/// Keep-alive interval on real hardware: **2.0026 s**, a tight timer.
///
/// Not the 1.5 s the pre-hardware research claimed, which traced back to what
/// reference *tools* chose rather than to what players do (C12). It matters
/// because every timeout below is a multiple of it.
constexpr int kKeepAliveIntervalMs = 2000;

/// Mark a device offline after this long without a keep-alive: five missed
/// beats. Two-tier on purpose — a player can blip off for 2 s on a cable jiggle
/// or a switch reconverging, and tearing its subtree down for that would be
/// infuriating mid-set.
constexpr int kDeviceTimeoutMs = 10000;
/// Only after this long is the device actually removed.
constexpr int kDeviceRemovalGraceMs = 60000;
/// How often the reaper re-evaluates the two timeouts above.
constexpr int kReaperIntervalMs = 1000;

/// Device numbers a real player can hold, and the range within which a server
/// must sit to be browsable at all.
///
/// Announcing outside 1–4 is not merely unconventional, it silently does not
/// work: at device 5 a deck accepted us completely — 927 status packets
/// unicast to us over four minutes — and never once asked what media we had, so
/// it never offered us as a source (F45). The check happens before any of the
/// browse machinery is reached.
constexpr int kMinPlayerNumber = 1;
constexpr int kMaxPlayerNumber = 4;
/// A number outside the player range, for announcing without contending. Usable
/// for consuming, useless for serving.
constexpr int kObserverNumber = 7;

/// Byte 0x0a of a discovery packet.
enum class PacketType : quint8 {
    ClaimMac = 0x00,
    MixerAssignIntent = 0x01,
    ClaimIp = 0x02,
    MixerAssign = 0x03,
    ClaimNumber = 0x04,
    /// Filed under mixer channel assignment by the pre-hardware research, but
    /// observed doing something else: a deck holding an auto-assigned number
    /// unicasts one of these, carrying its own number, in reply to somebody
    /// else's claim (F36).
    NumberInUse = 0x05,
    KeepAlive = 0x06,
    NumberConflict = 0x08,
    Hello = 0x0a,
};

/// Byte 0x21. Critical for impersonation.
enum class DeviceKind : quint8 {
    Mixer = 0x01,
    Cdj = 0x02,
    /// rekordbox desktop, and also CDJ-3000.
    RekordboxOrCdj3000 = 0x03,
    Cdj3000Hello = 0x04,
};

/// The slot byte of a dbserver request descriptor, and the discriminator when
/// one connection carries two media (F37).
enum class MediaSlot : quint8 {
    Empty = 0,
    Cd = 1,
    Sd = 2,
    Usb = 3,
    Rekordbox = 4,
};

/// The NFS export a player mounts for a slot.
///
/// `/C/EXPORT` has also been seen for USB on other hardware, so a *client*
/// should match on the prefix rather than the whole string (C6).
constexpr char kExportSd[] = "/B/";
constexpr char kExportUsb[] = "/C/";

/// The export a player serves for *slot*, or empty for a slot that has none.
inline const char* exportPathForSlot(MediaSlot slot) {
    switch (slot) {
    case MediaSlot::Sd:
        return kExportSd;
    case MediaSlot::Usb:
        return kExportUsb;
    default:
        return "";
    }
}

} // namespace prolink
} // namespace mixxx
