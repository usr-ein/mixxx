#include "widget/deck/deckautoplay.h"

#include <QList>
#include <QPair>
#include <QPointer>
#include <QRandomGenerator>
#include <utility>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "moc_deckautoplay.cpp"
#include "util/logger.h"
#include "widget/deck/wdeckbrowser.h"

namespace {
const mixxx::Logger kLogger("Autoplay");

/// Tracks in a row that may fail to load before autoplay gives up. One bad file
/// in a genre is no reason to stop the music; a drive none of whose tracks
/// will load is no reason to keep trying.
constexpr int kMaxFailures = 3;
constexpr int kRetryMs = 1000;

mixxx::deck::DeckAutoplay* s_pInstance = nullptr;
/// Subscribers that asked before there was anything to subscribe to.
QList<QPair<QPointer<QObject>, std::function<void(mixxx::deck::DeckAutoplay*)>>> s_pending;
} // namespace

namespace mixxx {
namespace deck {

DeckAutoplay* DeckAutoplay::instance() {
    return s_pInstance;
}

void DeckAutoplay::whenReady(QObject* pContext, std::function<void(DeckAutoplay*)> callback) {
    if (s_pInstance != nullptr) {
        callback(s_pInstance);
        return;
    }
    s_pending.append({QPointer<QObject>(pContext), std::move(callback)});
}

DeckAutoplay::DeckAutoplay(MediaRegistry* pRegistry,
        std::function<QSqlDatabase()> database,
        LoadRow loadRow,
        QObject* pParent)
        : QObject(pParent),
          m_pRegistry(pRegistry),
          m_database(std::move(database)),
          m_loadRow(std::move(loadRow)) {
    const QString group = WDeckBrowser::deckGroup();
    // The end of a track is the play position reaching 1. Not `play` going to
    // 0, which a pause does too; and not `end_of_track`, which is a warning
    // that the last thirty seconds have begun and never fires at all on a
    // track shorter than that.
    m_pPlayPosition = std::make_unique<ControlProxy>(group, QStringLiteral("playposition"), this);
    m_pPlayPosition->connectValueChanged(this, &DeckAutoplay::onPlayPosition);
    m_pTrackLoaded = std::make_unique<ControlProxy>(group, QStringLiteral("track_loaded"), this);
    m_pTrackLoaded->connectValueChanged(this, &DeckAutoplay::onTrackLoaded);
    m_pPicked = std::make_unique<ControlObject>(
            ConfigKey(QStringLiteral("[Browser]"), QStringLiteral("autoplay_picked")));
    m_pPicked->setReadOnly();

    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &DeckAutoplay::advance);

    if (m_pRegistry) {
        connect(m_pRegistry,
                &MediaRegistry::mediumVanished,
                this,
                &DeckAutoplay::onMediumVanished);
    }

    s_pInstance = this;
    const auto pending = std::exchange(s_pending, {});
    for (const auto& [pContext, callback] : pending) {
        if (pContext) {
            callback(this);
        }
    }
}

DeckAutoplay::~DeckAutoplay() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

QString DeckAutoplay::genreTitle() const {
    return m_genre.isEmpty() ? QStringLiteral("—") : m_genre;
}

QString DeckAutoplay::rootOf(const MediumId& medium) const {
    return medium.isLocal() ? medium.mountPoint() : MediaRegistry::remoteCacheRoot(medium);
}

void DeckAutoplay::start(const MediumId& medium,
        const QString& mediumName,
        const QString& genre,
        int rowId) {
    QSqlDatabase db = m_database();
    const QList<autoplay::Candidate> candidates =
            autoplay::candidates(db, medium, genre, rootOf(medium));
    const autoplay::Candidate* pStart = nullptr;
    for (const autoplay::Candidate& candidate : candidates) {
        if (candidate.rowId == rowId) {
            pStart = &candidate;
            break;
        }
    }
    if (!pStart) {
        // Not one of the genre's tracks after all: the list was stale.
        kLogger.warning() << "row" << rowId << "is not in" << genre << "on" << mediumName;
        return;
    }

    m_retry.stop();
    m_medium = medium;
    m_mediumName = mediumName;
    m_genre = genre;
    m_scope = autoplay::scopeKey(medium, genre);
    m_round = 1;
    m_failures = 0;
    m_currentKey.clear();
    m_currentBpm = 0.0;
    m_sinceEnd.invalidate();

    // The starting track counts as played: the round is the genre, and the DJ
    // has just played one of it.
    m_memory.insert(m_scope, pStart->key);
    m_pendingKey = pStart->key;
    m_pendingTitle = pStart->title;
    m_pendingBpm = pStart->bpm;
    m_pendingIsStart = true;
    m_state = State::Loading;
    // The DJ's pick, so the mapping returns to the deck when it loads, as for
    // any track they load.
    m_pPicked->forceSet(0.0);
    kLogger.info() << "on:" << genreTitle() << "on" << mediumName << "from" << pStart->title
                   << pStart->bpm << "BPM," << candidates.size() << "tracks";
    emit stateChanged();

    if (!m_loadRow(rowId)) {
        stop();
    }
}

void DeckAutoplay::stop(const QString& notice) {
    if (!isOn()) {
        return;
    }
    m_retry.stop();
    m_state = State::Off;
    m_pPicked->forceSet(0.0);
    kLogger.info() << "off:" << genreTitle() << "on" << m_mediumName
                   << (notice.isEmpty() ? QStringLiteral("(stopped)") : notice);
    emit stateChanged();
    if (!notice.isEmpty()) {
        emit this->notice(notice);
    }
}

void DeckAutoplay::onManualLoad() {
    stop(tr("Autoplay off — a track was loaded by hand"));
}

void DeckAutoplay::onDeckEmpty() {
    if (m_state == State::Loading) {
        // The load came back empty: the track would not load.
        onLoadFailed();
        return;
    }
    stop(tr("Autoplay off — the track was ejected"));
}

void DeckAutoplay::onPlayPosition(double position) {
    // Only the track autoplay put on the deck, and only once the deck has it.
    // While a load is on its way the position still reads the end of the last
    // track, and a value that has not changed is not announced again -- so the
    // next one seen is the new track's.
    if (m_state != State::Playing || position < 1.0) {
        return;
    }
    m_sinceEnd.start();
    advance();
}

void DeckAutoplay::onTrackLoaded(double loaded) {
    if (m_state != State::Loading || loaded <= 0.0) {
        return;
    }
    m_currentKey = m_pendingKey;
    m_currentBpm = m_pendingBpm;
    m_pendingIsStart = false;
    m_failures = 0;
    m_state = State::Playing;
    if (m_sinceEnd.isValid()) {
        kLogger.info() << "loaded" << m_pendingTitle << m_sinceEnd.elapsed()
                       << "ms after the end of the last";
        m_sinceEnd.invalidate();
    }
}

void DeckAutoplay::onMediumVanished(const MediumInfo& medium) {
    if (!isOn() || medium.id != m_medium) {
        return;
    }
    // Stopped now rather than at the end of the track: what is playing goes on
    // playing from its copy (or not, which is the cache's business and its
    // toast's), and the DJ learns now, not when the music stops, that nothing
    // will follow it.
    stop(tr("Autoplay off — %1 was removed").arg(m_mediumName));
}

void DeckAutoplay::advance() {
    if (!isOn()) {
        return;
    }
    const int index = m_pRegistry ? m_pRegistry->indexOf(m_medium) : -1;
    if (index < 0 || m_pRegistry->media().at(index).state != MediumInfo::State::Ready) {
        stop(tr("Autoplay off — %1 is gone").arg(m_mediumName));
        return;
    }

    QSqlDatabase db = m_database();
    const QList<autoplay::Candidate> candidates =
            autoplay::candidates(db, m_medium, m_genre, rootOf(m_medium));
    // The tempo of the track that has just played, as the library has it now:
    // a stick's track with no BPM tag gets one written back once Mixxx has
    // analysed it, which is usually while it plays.
    double reference = m_currentBpm;
    for (const autoplay::Candidate& candidate : candidates) {
        if (candidate.key == m_currentKey) {
            reference = candidate.bpm;
            break;
        }
    }
    const autoplay::Pick pick = autoplay::next(candidates,
            &m_memory,
            m_scope,
            m_currentKey,
            reference,
            QRandomGenerator::global());
    if (pick.index < 0) {
        stop(tr("Autoplay off — no %1 left on %2").arg(genreTitle(), m_mediumName));
        return;
    }
    if (pick.newRound) {
        ++m_round;
        kLogger.info() << "every" << genreTitle() << "track has played: round" << m_round;
    }
    const autoplay::Candidate& next = candidates.at(pick.index);
    // The line a set can be read back from: what, at what tempo, after what,
    // and how much of the round is left after it.
    kLogger.info() << "next:" << next.title << "(" << next.key << ")" << next.bpm << "BPM after"
                   << reference << "BPM;" << pick.unplayed - 1 << "of" << candidates.size()
                   << "left in round" << m_round;

    m_pendingKey = next.key;
    m_pendingTitle = next.title;
    m_pendingBpm = next.bpm;
    m_pendingIsStart = false;
    m_state = State::Loading;
    m_pPicked->forceSet(1.0);
    if (!m_loadRow(next.rowId)) {
        onLoadFailed();
    }
}

void DeckAutoplay::onLoadFailed() {
    if (m_pendingIsStart) {
        // The DJ's own pick would not load, and the deck's toast says so.
        // There is nothing playing to follow.
        stop();
        return;
    }
    ++m_failures;
    if (m_failures >= kMaxFailures) {
        stop(tr("Autoplay off — %1 tracks in a row would not load").arg(m_failures));
        return;
    }
    // It counts as played, so the next pick is another track, still near the
    // one that played last.
    kLogger.warning() << "could not load" << m_pendingTitle << "(" << m_pendingKey
                      << ") -- trying the next";
    m_retry.start();
}

} // namespace deck
} // namespace mixxx
