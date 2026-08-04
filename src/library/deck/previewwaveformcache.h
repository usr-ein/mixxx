#pragma once

#include <QAtomicInt>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QThreadPool>

#include "library/deck/mediumid.h"
#include "library/deck/previewwaveform.h"

namespace mixxx {
namespace deck {

/// Preview waveforms for the info panel, fetched off the GUI thread and kept in
/// RAM.
///
/// # Nothing here may block the GUI thread
///
/// The panel follows the selection with no click, so this is asked for a
/// preview on every encoder detent — and the answer lives in a file on a USB
/// stick that may be busy copying a track. So:
///
///  * **lookup() never reads anything.** It answers from RAM or says it does
///    not have it, and is safe to call from a paint.
///  * **request() returns immediately.** The read happens on this class's own
///    worker and the result arrives as `arrived`.
///  * **The worker returns a value; the GUI thread mutates the cache.** The
///    worker touches no member and no widget.
///  * **The worker is one thread, and it is ours.** Not the global pool, which
///    carries Mixxx's analysers and the track cache's prefetch: four threads
///    reading one stick is slower than one, and a private pool is also what
///    makes "drop everything queued" mean anything.
///
/// # A late answer is normal
///
/// Selection moves faster than I/O. Every request carries the generation it was
/// made in; the worker checks before it reads and gives up if the selection has
/// moved on since. A result that arrives anyway is cached and simply not drawn.
class PreviewWaveformCache : public QObject {
    Q_OBJECT

  public:
    explicit PreviewWaveformCache(QObject* pParent = nullptr);
    ~PreviewWaveformCache() override;

    /// What is already in RAM. Never reads a file, never asks the network.
    ///
    /// A null preview means either "not asked for yet" or "this track has
    /// none", and the caller cannot tell the two apart — deliberately, because
    /// the panel draws the same thing for both.
    PreviewWaveform lookup(const MediumId& medium, quint32 rekordboxId) const;

    /// Ask for one, if it is not already here or already being read.
    ///
    /// *analyzePath* is the track's `.DAT`; a track without one is remembered
    /// as having no preview rather than asked about again.
    void request(const MediumId& medium, quint32 rekordboxId, const QString& analyzePath);

    /// Abandon everything queued that has not started.
    ///
    /// Called when the selection moves, so spinning the encoder through a
    /// hundred rows does not leave a hundred file reads behind it.
    void cancelQueued();

    /// Forget everything from a medium that has gone.
    ///
    /// The opposite of the track cache's markUnreachable(), and the difference
    /// is the point: a cached *track* is irreplaceable once the stick is out,
    /// while a cached *waveform* is a picture of a track that can no longer be
    /// played, held for rows that are no longer on screen.
    void forget(const MediumId& medium);

    /// Entries held, for the diagnostics page.
    int count() const {
        return m_previews.size();
    }
    /// Bytes held by the previews themselves.
    int bytes() const {
        return m_previews.size() * PreviewWaveform::kColumns;
    }

  signals:
    /// One arrived. The panel re-asks with lookup() rather than being handed
    /// it, because by now the selection may be somewhere else.
    void arrived(const QString& mediumKey, quint32 rekordboxId);

  private:
    /// A remote preview came back off the network, as the 900 bytes a player
    /// sends. Empty when it had none.
    void onRemoteArrived(const MediumId& medium, quint32 rekordboxId, const QByteArray& blob);

    /// `medium|id`, so one hash covers both sources and survives a medium
    /// coming back.
    static QString keyFor(const MediumId& medium, quint32 rekordboxId);

    /// Called on the GUI thread with what the worker read.
    void store(const QString& key, const QString& mediumKey, quint32 rekordboxId,
            const PreviewWaveform& preview);

    /// Held to the cap by dropping the oldest inserted. Not a true LRU: at 400
    /// bytes an entry the cap exists to bound a runaway, not to make a decision
    /// worth tracking access times for.
    void evictIfNeeded();

    QHash<QString, PreviewWaveform> m_previews;
    /// Insertion order, for the eviction above.
    QQueue<QString> m_order;
    /// Asked for and not yet answered, so a repaint does not ask again.
    QSet<QString> m_inFlight;

    /// Bumped whenever the selection moves. Shared with the worker, which
    /// compares against the value its request carried.
    QAtomicInt m_generation;
    /// Set on destruction, so a worker mid-read stops rather than calling back
    /// into a dead object.
    QAtomicInt m_shuttingDown;
    QThreadPool m_pool;
};

} // namespace deck
} // namespace mixxx
