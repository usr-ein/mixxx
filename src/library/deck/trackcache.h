#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include "library/deck/mediumid.h"

namespace mixxx {
namespace deck {

/// Where the deck's audio actually comes from.
///
/// **The deck never plays off removable media.** Every track it plays has been
/// copied here first and is played from the copy, which is the whole reason a
/// stick can be pulled mid-set without the music stopping. Remote media already
/// worked this way because they had no choice; this extends the same rule to
/// sticks.
///
/// Without it the deck survives about **fifteen seconds** after a stick is
/// pulled — Mixxx holds 5 MB of decoded audio per deck (80 chunks × 8192 frames
/// × 2 ch × 4 B in `cachingreader.cpp`), which at 44.1 kHz is 14.9 seconds of
/// played and unplayed audio together.
///
/// ## Two tiers, and the rule that picks between them
///
/// Tier 1 is RAM. Tier 2 is the SD card, and it is written **only** for bytes
/// that can no longer be got again — a track whose stick has been unplugged, or
/// whose player has left. Everything still re-readable is simply dropped when
/// RAM gets tight, because re-copying from a stick that is still in the slot
/// costs a second and costs the card nothing.
///
/// So in normal operation the card is never written at all.
class TrackCache : public QObject {
    Q_OBJECT

  public:
    explicit TrackCache(QObject* pParent = nullptr);
    ~TrackCache() override;

    static TrackCache* instance();

    /// Where a copy of *sourcePath* lives, whether or not it is there yet.
    QString localPathFor(const MediumId& medium, const QString& sourcePath) const;
    bool isCached(const MediumId& medium, const QString& sourcePath) const;

    /// Start copying in the background, if it is not already here.
    ///
    /// Called as the selection dwells on a row: a DJ looks at a track before
    /// loading it, so by the time the encoder is pushed the copy is usually
    /// already done and the load is instant. One at a time — the constraint is
    /// the USB bus, not the CPU.
    void prefetch(const MediumId& medium, const QString& sourcePath);

    /// Copy now and return the local path, or empty on failure.
    ///
    /// Blocks. This is the slow path — the one taken when a DJ pushes load on a
    /// track the prefetch has not reached yet — and it is why prefetch exists.
    QString ensureLocal(const MediumId& medium, const QString& sourcePath);

    /// Take responsibility for a file that got here some other way.
    ///
    /// A streamed track is written straight into tier 1 by the network layer,
    /// so the cache never sees it copied and would otherwise not know it is
    /// there at all — which means it would neither count against the RAM cap
    /// nor be reclaimable, and a night of remote loads would quietly fill the
    /// tmpfs.
    ///
    /// Deliberately does **not** evict: the caller pins the file immediately
    /// afterwards, and running the sweep in between could drop the very file
    /// being adopted.
    void adopt(const MediumId& medium, const QString& localPath, qint64 size);

    /// Never evict this one. The track on the deck, and anything a Pro DJ Link
    /// peer is reading from us.
    void pin(const QString& localPath);
    void unpin(const QString& localPath);

    /// A medium is gone: everything cached from it can no longer be re-read, so
    /// nothing belonging to it may be dropped to reclaim space.
    void markUnreachable(const MediumId& medium);

    /// Whether something pinned -- i.e. on the deck right now -- came from this
    /// medium. What lets the eject notice say the track stays playable only
    /// when that is actually true.
    bool hasPinnedFrom(const MediumId& medium) const;

    /// For the diagnostics page.
    qint64 bytesInRam() const {
        return m_ramBytes;
    }
    qint64 bytesOnDisk() const {
        return m_diskBytes;
    }
    qint64 bytesWrittenToDisk() const {
        return m_diskBytesWritten;
    }

  signals:
    /// A background copy finished. Carries the local path, so a caller waiting
    /// on one track can tell it from another.
    void cached(const QString& localPath, bool ok);

  private:
    struct Entry {
        MediumId medium;
        QString localPath;
        qint64 size = 0;
        /// False once the source is gone: dropping it would lose the only copy.
        bool reReadable = true;
        bool onDisk = false;
        qint64 lastUsed = 0;
    };

    /// Copy one file, making the parent directories. Returns bytes written.
    static qint64 copyFile(const QString& from, const QString& to);
    void touch(const QString& localPath);
    /// Bring tier 1 back under its cap, dropping what can be re-read and
    /// spilling to tier 2 only what cannot.
    void evictIfNeeded();

    /// Our own pool rather than the global one, for two reasons.
    ///
    /// Lifetime: a background copy captures `this` and dereferences it when it
    /// finishes, so one still running when the cache is destroyed writes into
    /// freed memory. A pool we own can be drained in the destructor; the global
    /// one cannot, because other things are using it.
    ///
    /// And concurrency: the constraint here is the USB bus, not the CPU, so
    /// this pool runs exactly one copy at a time. The global pool runs as many
    /// as there are cores, which is slower for the same work and makes the
    /// stick seek between files.
    QThreadPool m_copyPool;

    QHash<QString, Entry> m_entries;
    QSet<QString> m_pinned;
    QString m_tier1Root;
    QString m_tier2Root;
    /// Measured from the filesystem tier 1 actually landed on, not assumed.
    qint64 m_tier1Cap = 0;
    qint64 m_ramBytes = 0;
    qint64 m_diskBytes = 0;
    qint64 m_diskBytesWritten = 0;
    qint64 m_clock = 0;
};

} // namespace deck
} // namespace mixxx
