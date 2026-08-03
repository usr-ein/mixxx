#pragma once

#include <QString>

namespace mixxx {
namespace deck {

/// Where the deck keeps bytes that must not touch the SD card, and how many of
/// them it may keep.
///
/// Three things want RAM-backed scratch — the track cache's first tier, the
/// mirror of a remote medium's analysis and artwork, and the copies that keep a
/// consuming player alive across an eject — and all three were reaching for a
/// path and a constant of their own.
///
/// **The constant was wrong.** `/run` on this deck is 760 MB, not the gigabyte
/// the track cache was sized for, so the cap could never be reached: the
/// filesystem would fill first, and `/run` filling takes systemd with it. A
/// budget has to be measured, not assumed, because the number depends on how
/// the kernel sized a tmpfs on a machine nobody has in front of them.
class RamStore {
  public:
    /// A directory under the largest RAM-backed filesystem available.
    ///
    /// `name` is a single component: "cache", "remote", "serve". Created on
    /// first use, and stable for the life of the process.
    ///
    /// Everything under it evaporates at reboot, which is exactly right for a
    /// copy of somebody else's stick.
    static QString path(const QString& name);

    /// How many bytes the caller may hold there.
    ///
    /// A share of what was actually free when the deck started, so the three
    /// users can each take one without the total exceeding what exists. Sharing
    /// rather than each taking a fixed slice of a number nobody checked is the
    /// whole point.
    ///
    /// *share* is a fraction of the store's budget, 0..1.
    static qint64 budget(double share);

    /// Bytes free on the store right now. For the diagnostics page: a store
    /// running out is what precedes every other symptom.
    static qint64 available();

    /// Where the store ended up, for the diagnostics page and the log.
    static QString root();
};

} // namespace deck
} // namespace mixxx
