#pragma once

#include <QString>

namespace mixxx {
namespace deck {

/// What a stick calls itself.
///
/// The deck mounts DJ sticks at `/media/DJ_USB_1` and `/media/DJ_USB_2`, which
/// is a *slot*, not a name — and the slots are handed out in plug order, so
/// there is no relation between the two at all. On this deck right now,
/// `DJ_USB_1` is a stick labelled `SAM2` and `DJ_USB_2` is one labelled `SAM1`.
/// Showing the mount point, as the old library view did, therefore shows the
/// wrong name about half the time.
///
/// Two sources, in order:
///
///   1. **A sidecar written at mount time** by `pi_config/dj-usb`, which
///      already runs `blkid` for the filesystem type and needs one more field.
///      Preferred because it is authoritative and needs no guessing: it is the
///      label of the device that was actually mounted there.
///
///   2. **The `/dev/disk/by-label` symlinks**, resolved against the device
///      behind the mount point via `/proc/self/mountinfo`. Needed because the
///      sidecar does not exist on a development box, or when something other
///      than `dj-usb` did the mounting.
///
/// An unlabelled stick is not an error — plenty of them have no label at all —
/// and falls back to the slot: `USB 1`, `USB 2`.
QString volumeLabelFor(const QString& mountPoint);

/// The block device behind a mount point, e.g. `/dev/sda1`, or empty.
/// Exposed for the tests, and for the diagnostics page.
QString deviceForMountPoint(const QString& mountPoint);

} // namespace deck
} // namespace mixxx
