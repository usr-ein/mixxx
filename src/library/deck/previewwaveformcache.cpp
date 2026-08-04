#include "library/deck/previewwaveformcache.h"

#include <QtConcurrentRun>

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
                PreviewWaveform preview;
                if (analyzePath.endsWith(QStringLiteral(".DAT"))) {
                    const QString ext =
                            analyzePath.left(analyzePath.size() - 3) +
                            QStringLiteral("EXT");
                    const prolink::AnlzPreview colour = prolink::readAnlzPreview(ext);
                    preview = PreviewWaveform::fromColourPreview(colour.colour);
                }
                if (preview.isNull()) {
                    const prolink::AnlzPreview mono = prolink::readAnlzPreview(analyzePath);
                    preview = PreviewWaveform::fromPwav(mono.mono);
                }

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
