#pragma once

#include <QAtomicInt>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <memory>

#include "library/deck/mediumid.h"

namespace mixxx {
namespace deck {

class StreamingFile;

/// Where the deck's audio actually comes from.
///
/// **The deck never plays off removable media.** Every track it plays is
/// copied here and played from the copy, which is the whole reason a stick can
/// be pulled mid-set without the music stopping. Remote media already worked
/// this way because they had no choice; this extends the same rule to sticks.
///
/// **And it plays the copy while it arrives,** as a remote track does: the
/// deck's decoder reads it through a StreamingFile, which hands over bytes as
/// they land and makes a read of the rest wait. A load used to copy the whole
/// file first, on the GUI thread -- eighteen seconds of a frozen deck for a
/// 52 MB AIFF off a USB 2 stick at 3.7 MB/s, and nothing on the screen to say
/// the push had been heard.
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
    /// well under way. One at a time, because the constraint is the USB bus,
    /// not the CPU -- so only the latest row is copied: a prefetch for a row
    /// scrolled past is dropped, and none starts while the track on the deck is
    /// still arriving, which would take the stick away from it. That one runs
    /// once the deck's copy is done.
    void prefetch(const MediumId& medium, const QString& sourcePath);

    /// Start copying a stick's track for the deck, and return where it goes.
    ///
    /// **Never waits for the copy.** What comes back is the finished copy when
    /// it is already here, or a file of the track's full size, registered as a
    /// StreamingFile, that a decoder reads as the bytes land: in order from the
    /// start, except that a read waiting further on -- an M4A's index at the
    /// end, a seek, a hot cue -- has the copy carry on from there. A copy already running for the track (a prefetch,
    /// usually) is handed back as it is, never started a second time: two
    /// copies of one file raced, and the loser fell back to playing straight
    /// off the stick. Other prefetches give way. Empty when the stick cannot be
    /// read at all.
    QString startLocal(const MediumId& medium, const QString& sourcePath);

    /// The deck let go of the track at *localPath*: if its copy is still
    /// running, nobody is waiting for it any more, so it stops and makes way
    /// for the next one.
    void release(const QString& localPath);

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
    /// medium and is here in full. What lets the eject notice say the track
    /// stays playable only when that is actually true: a copy still arriving
    /// does not count, because it stops where the stick did.
    bool hasPinnedFrom(const MediumId& medium) const;

    /// Whether a copy off a stick is running right now.
    ///
    /// What background reads of a stick ask before starting -- a folder
    /// medium's tags (MediaRegistry) -- because they share the USB bus with the
    /// copy, and the copy is the thing a DJ is waiting on.
    bool isCopying() const {
        return m_copiesInFlight.loadRelaxed() > 0;
    }

    /// Where the disk tier lives. Shared with the boot-time purge, which has to
    /// forget the library rows of tracks that were played from here.
    static QString diskTierRoot();

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

    /// For tests: wait this long between chunks, so a copy lasts long enough
    /// to be caught running.
    static void setChunkDelayForTest(int milliseconds);

  signals:
    /// A background copy finished, or was stopped (ok false). Carries the local
    /// path, so a caller waiting on one track can tell it from another.
    void cached(const QString& localPath, bool ok);

  private:
    /// A copy running or queued on the copy pool. GUI thread only.
    struct Copy {
        MediumId medium;
        QString sourcePath;
        QString localPath;
        qint64 size = 0;
        std::shared_ptr<StreamingFile> stream;
        /// Set to stop it at its next chunk. Shared with the copy itself.
        std::shared_ptr<QAtomicInt> stop;
        /// The deck is reading it, so nothing but its own release stops it.
        bool forDeck = false;
    };

    struct Entry {
        MediumId medium;
        QString localPath;
        qint64 size = 0;
        /// False once the source is gone: dropping it would lose the only copy.
        bool reReadable = true;
        bool onDisk = false;
        qint64 lastUsed = 0;
    };

    /// The file name a copy of *sourcePath* on *medium* goes by.
    static QString cacheName(const MediumId& medium, const QString& sourcePath);
    /// Copy one file, making the parent directories. Returns bytes written.
    static qint64 copyFile(const QString& from, const QString& to);
    /// The body of a copy, on the pool's thread: *from* into *to*, already at
    /// its full size, telling *pStream* about every chunk that lands.
    static bool copyInto(const QString& from,
            const QString& to,
            qint64 size,
            StreamingFile* pStream,
            const QAtomicInt& stop,
            QString* pError);
    /// Whether the whole of *localPath* is here, with no copy still writing it.
    bool isComplete(const QString& localPath) const;
    QString beginCopy(const MediumId& medium, const QString& sourcePath, bool forDeck);
    void onCopyDone(const QString& localPath,
            const std::shared_ptr<StreamingFile>& pStream,
            bool ok,
            const QString& error);
    /// Stop every prefetch but the one for *keep*.
    void stopPrefetches(const QString& keep = QString());
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

    /// Whole copies, by file name.
    QHash<QString, Entry> m_entries;
    /// Copies under way, by local path.
    QHash<QString, Copy> m_copies;
    /// The prefetch waiting for the deck's copy to finish: only the latest.
    MediumId m_pendingPrefetchMedium;
    QString m_pendingPrefetchSource;
    /// Media pulled out while a copy of theirs was running: what those copies
    /// leave behind is the only copy there is.
    QSet<QString> m_unreachable;
    QSet<QString> m_pinned;
    QString m_tier1Root;
    QString m_tier2Root;
    /// Measured from the filesystem tier 1 actually landed on, not assumed.
    qint64 m_tier1Cap = 0;
    qint64 m_ramBytes = 0;
    qint64 m_diskBytes = 0;
    qint64 m_diskBytesWritten = 0;
    qint64 m_clock = 0;
    /// Copies running on the pool's thread.
    QAtomicInt m_copiesInFlight;
};

} // namespace deck
} // namespace mixxx
