#pragma once

#include <QtGlobal>

/// What the rest of Mixxx shares with Pro DJ Link's wire format.
///
/// The protocol itself is `lib/prolink`'s, in Rust. The prolinks-compat repo's
/// `docs/PROTOCOL.md` is its specification and `docs/FINDINGS.md` records how
/// each fact was established; finding numbers (F*n*) refer to that second
/// document.
///
/// This header deliberately contains no Qt types beyond `qint`/`quint` and no
/// includes from `src/library/`: everything under `src/network/prolink/` must
/// stay usable, and unit-testable, without a Library.
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

} // namespace prolink
} // namespace mixxx
