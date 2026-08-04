#include "library/deck/previewwaveformcache.h"

#include <QtConcurrentRun>

#include "library/deck/mediaregistry.h"
#include "network/prolink/prolinkanlz.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("PreviewWaveformCache");

/// Previews to keep. 400 bytes each, so this is 1.6 MB — a backstop against a
/// runaway rather than a budget anyone has to think about. A 700-track stick
/// swept end to end is 280 kB.
constexpr int kMaxEntries = 4096;
} // namespace

namespace mixxx {
namespace deck {

PreviewWaveformCache::PreviewWaveformCache(QObject* pParent)
        : QObject(pParent) {
    // One thread, and ours. See the header: the stick is the contended
    // resource, and four readers of one stick is slower than one.
    m_pool.setMaxThreadCount(1);

    // Remote previews come back through the registry. whenReady() rather than
    // instance(), because construction order in a file skin authors edit is not
    // something to rely on -- relying on it cost the toasts entirely once.
    MediaRegistry::whenReady(this, [this](MediaRegistry* pRegistry) {
        connect(pRegistry,
                &MediaRegistry::previewArrived,
                this,
                &PreviewWaveformCache::onRemoteArrived);
    });
}

PreviewWaveformCache::~PreviewWaveformCache() {
    // Tell anything mid-read to stop calling back, then wait for it. Without
    // the wait a worker can outlive this object and post into a dangling
    // pointer; without the flag it would finish a whole file first.
    m_shuttingDown.storeRelaxed(1);
    m_pool.clear();
    m_pool.waitForDone();
}

QString PreviewWaveformCache::keyFor(const MediumId& medium, quint32 rekordboxId) {
    return medium.key() + QChar('|') + QString::number(rekordboxId);
}

PreviewWaveform PreviewWaveformCache::lookup(
        const MediumId& medium, quint32 rekordboxId) const {
    return m_previews.value(keyFor(medium, rekordboxId));
}

void PreviewWaveformCache::request(
        const MediumId& medium, quint32 rekordboxId, const QString& analyzePath) {
    if (!medium.isValid() || rekordboxId == 0) {
        return;
    }
    const QString key = keyFor(medium, rekordboxId);
    if (m_previews.contains(key) || m_inFlight.contains(key)) {
        return;
    }
    if (!medium.isLocal()) {
        // **A remote track has no file to read.** Its analysis files are on the
        // player until the track is loaded, and `analyze_path` names where they
        // *will* go rather than where they are. So it is asked for over
        // dbserver instead: 900 bytes on the connection cover art already uses,
        // nothing written to a filesystem at either end.
        //
        // Monochrome, and unavoidably so -- a player answers
        // GET_WAVEFORM_PREVIEW with PWAV and nothing else, and the colour
        // preview is behind a request nothing in lib/prolink implements yet.
        m_inFlight.insert(key);
        if (MediaRegistry* pRegistry = MediaRegistry::instance()) {
            pRegistry->requestPreview(medium, rekordboxId);
        } else {
            store(key, medium.key(), rekordboxId, PreviewWaveform());
        }
        return;
    }
    if (analyzePath.isEmpty()) {
        // Nothing to read, ever. Remembered as having no preview so a repaint
        // does not ask again on every frame.
        store(key, medium.key(), rekordboxId, PreviewWaveform());
        return;
    }

    m_inFlight.insert(key);
    const int generation = m_generation.loadRelaxed();
    const QString mediumKey = medium.key();

    QtConcurrent::run(&m_pool,
            [this, key, mediumKey, rekordboxId, analyzePath, generation]() {
                // Checked before the read rather than after: the point is to
                // not touch the stick for a row nobody is looking at any more.
                if (m_shuttingDown.loadRelaxed() ||
                        m_generation.loadRelaxed() != generation) {
                    QMetaObject::invokeMethod(
                            this,
                            [this, key]() { m_inFlight.remove(key); },
                            Qt::QueuedConnection);
                    return;
                }

                // **Colour first, monochrome as the fallback.** The good
                // preview is `PWV4` in the `.EXT`; the `.DAT` beside it carries
                // only `PWAV`, which is what an older rekordbox wrote and what
                // a CDJ will hand over.
                //
                // Both reads go through the seeking reader rather than a full
                // parse. A `.EXT` is around 157 kB and almost all of it is the
                // two full-resolution detail waveforms -- fifty thousand
                // entries each -- so parsing it to reach 1200 colour columns
                // would decode both and throw them away, once per row a DJ
                // pauses on, off a stick that may be busy copying a track.
                // **Both files, because each carries half the answer.** The
                // `.DAT`'s `PWAV` is the envelope -- one column per column
                // drawn, and the least compressed of the two encodings -- and
                // the `.EXT`'s `PWV4` is the three frequency bands. See
                // PreviewWaveform::fromAnlz for the measurements behind that
                // split.
                const prolink::AnlzPreview mono = prolink::readAnlzPreview(analyzePath);
                QVector<prolink::AnlzPreviewColumn> colour;
                if (analyzePath.endsWith(QStringLiteral(".DAT"))) {
                    const QString ext = analyzePath.left(analyzePath.size() - 3) +
                            QStringLiteral("EXT");
                    colour = prolink::readAnlzPreview(ext).colour;
                }
                const PreviewWaveform preview =
                        PreviewWaveform::fromAnlz(mono.mono, colour);

                if (m_shuttingDown.loadRelaxed()) {
                    return;
                }
                QMetaObject::invokeMethod(
                        this,
                        [this, key, mediumKey, rekordboxId, preview]() {
                            store(key, mediumKey, rekordboxId, preview);
                        },
                        Qt::QueuedConnection);
            });
}

void PreviewWaveformCache::onRemoteArrived(
        const MediumId& medium, quint32 rekordboxId, const QByteArray& blob) {
    const QString key = keyFor(medium, rekordboxId);
    if (!m_inFlight.contains(key)) {
        return; // Not ours, or already answered.
    }
    // An empty blob is a player saying it has no preview for that track, which
    // is stored as such so it is never asked for twice.
    store(key, medium.key(), rekordboxId, PreviewWaveform::fromWire(blob));
}

void PreviewWaveformCache::store(const QString& key,
        const QString& mediumKey,
        quint32 rekordboxId,
        const PreviewWaveform& preview) {
    m_inFlight.remove(key);
    if (!m_previews.contains(key)) {
        m_order.enqueue(key);
    }
    // Stored even when null. A track with no preview is a fact worth
    // remembering: without it every repaint of that row starts another read.
    m_previews.insert(key, preview);
    evictIfNeeded();
    emit arrived(mediumKey, rekordboxId);
}

void PreviewWaveformCache::evictIfNeeded() {
    while (m_previews.size() > kMaxEntries && !m_order.isEmpty()) {
        m_previews.remove(m_order.dequeue());
    }
}

void PreviewWaveformCache::cancelQueued() {
    // Bumping the generation is what a queued worker checks; clear() drops the
    // ones that have not started at all. Neither can stop a read already under
    // way, and neither needs to: it is one file.
    m_generation.fetchAndAddRelaxed(1);
    m_pool.clear();
}

void PreviewWaveformCache::forget(const MediumId& medium) {
    const QString prefix = medium.key() + QChar('|');
    int dropped = 0;
    for (auto it = m_previews.begin(); it != m_previews.end();) {
        if (it.key().startsWith(prefix)) {
            it = m_previews.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    if (dropped > 0) {
        kLogger.debug() << "dropped" << dropped << "previews from" << medium.key();
    }
}

} // namespace deck
} // namespace mixxx

#include "moc_previewwaveformcache.cpp"
