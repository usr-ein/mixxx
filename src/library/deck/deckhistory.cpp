#include "library/deck/deckhistory.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <algorithm>
#include <ctime>

#include "control/controlproxy.h"
#include "library/dao/trackschema.h"
#include "library/deck/deckloader.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/pdbingest.h"
#include "library/queryutil.h"
#include "moc_deckhistory.cpp"
#include "network/prolink/prolinknetworkservice.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("DeckHistory");

const QString kBootIdPath = QStringLiteral("/proc/sys/kernel/random/boot_id");

/// The boot's clock: milliseconds since the Pi started, which no NTP moves.
qint64 bootClockMs() {
#ifdef Q_OS_LINUX
    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    return static_cast<qint64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#else
    // Elsewhere, Mixxx's own run, which is also all a session is there.
    static const qint64 start = QDateTime::currentMSecsSinceEpoch();
    return QDateTime::currentMSecsSinceEpoch() - start;
#endif
}

/// Which power-up this is. Off Linux, this run of Mixxx: there is no boot to
/// key on, and a development machine gets a session per start.
QByteArray currentBootId() {
    QFile file(kBootIdPath);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray id = file.readAll().trimmed();
        if (!id.isEmpty()) {
            return id;
        }
    }
    return QByteArrayLiteral("run-") +
            QByteArray::number(QDateTime::currentMSecsSinceEpoch());
}

/// Whether a player's status says it plays: playing or looping, CUE held, or
/// the buffer after its stick went (Sam). The flag as well as the state,
/// because each is missing where the other is not -- an emulated NXS playing a
/// plain file leaves the flag clear (E12).
bool statusSaysPlaying(const mixxx::prolink::ProLinkPlayer& player) {
    using State = mixxx::prolink::ProLinkPlayer::PlayState;
    switch (player.playState) {
    case State::Playing:
    case State::Looping:
    case State::CuePlay:
    case State::Emergency:
        return true;
    case State::NoTrack:
    case State::Loading:
        return false;
    default:
        return player.isPlaying;
    }
}

QString slotName(mixxx::prolink::MediaSlot slot) {
    switch (slot) {
    case mixxx::prolink::MediaSlot::Sd:
        return QStringLiteral("sd");
    case mixxx::prolink::MediaSlot::Usb:
        return QStringLiteral("usb");
    case mixxx::prolink::MediaSlot::Cd:
        return QStringLiteral("cd");
    default:
        return QString();
    }
}
} // namespace

namespace mixxx {
namespace deck {

QString DeckHistory::defaultDirectory() {
    return QStringLiteral("/var/lib/rauc/trimixxx-history");
}

DeckHistory::DeckHistory(const QString& group,
        MediaRegistry* pRegistry,
        mixxx::prolink::ProLinkNetworkService* pNetwork,
        DeckLoader* pLoader,
        mixxx::DbConnectionPoolPtr dbConnectionPool,
        const QString& directory,
        QObject* pParent)
        : QObject(pParent),
          m_pRegistry(pRegistry),
          m_pNetwork(pNetwork),
          m_pLoader(pLoader),
          m_dbConnectionPool(std::move(dbConnectionPool)),
          m_pPlay(std::make_unique<ControlProxy>(group, QStringLiteral("play"), this)),
          m_pTrackLoaded(std::make_unique<ControlProxy>(
                  group, QStringLiteral("track_loaded"), this)),
          m_pStore(std::make_unique<HistoryStore>()),
          m_epoch(QString::number(bootClockMs(), 36)) {
    m_thread.setObjectName(QStringLiteral("DeckHistory"));
    m_pStore->moveToThread(&m_thread);
    m_thread.start(QThread::LowPriority);
    // Waited for, once: the sessions are what the History shows, and opening
    // is a few small reads on a card that is not yet busy with anything else.
    HistoryStore::Opened opened;
    const QByteArray bootId = currentBootId();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(
            m_pStore.get(),
            [this, &opened, &directory, &bootId, now]() {
                opened = m_pStore->open(directory, bootId, now);
            },
            Qt::BlockingQueuedConnection);
    m_stored = opened.ok;
    m_sessions = opened.sessions;
    m_artworkDirectory = QDir(directory).filePath(QStringLiteral("art"));
    if (m_sessions.isEmpty() || m_sessions.first().bootId != bootId) {
        // No store: this power-up's History, in RAM.
        HistorySession session;
        session.bootId = bootId;
        session.openedWallMs = now;
        m_sessions.prepend(session);
        kLogger.warning() << "the History is not on the card:" << opened.error;
    }
    // Mixxx started again within the power-up: carry on its numbering.
    for (const HistoryTrack& track : std::as_const(current().tracks)) {
        m_nextTrackId = std::max(m_nextTrackId, track.id + 1);
        m_trackIds.insert(track.key, track.id);
    }
    for (const HistoryPlay& play : std::as_const(current().plays)) {
        m_nextPlayId = std::max(m_nextPlayId, play.id + 1);
    }

    connect(m_pStore.get(),
            &HistoryStore::artworkSaved,
            this,
            &DeckHistory::artworkArrived,
            Qt::QueuedConnection);
    // A medium read is the moment its tracks can be named; one gone, the
    // moment its slot may next hold another stick.
    connect(m_pRegistry, &MediaRegistry::mediaChanged, this, &DeckHistory::resolvePending);
    connect(m_pRegistry,
            &MediaRegistry::artworkArrived,
            this,
            [this](const QString& coverPath) {
                const QStringList names = m_coversWaited.take(coverPath);
                for (const QString& name : names) {
                    requestArtwork(coverPath, name);
                }
            });

    m_sampleTimer.setInterval(kSampleMs);
    connect(&m_sampleTimer, &QTimer::timeout, this, &DeckHistory::sample);
    m_sampleTimer.start();
    m_heartbeatTimer.setInterval(kHeartbeatMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &DeckHistory::heartbeat);
    m_heartbeatTimer.start();
}

DeckHistory::~DeckHistory() {
    // Nobody is listening by now (the skin went first), and nobody should be
    // told: what plays stops because Mixxx is going, not because it stopped.
    blockSignals(true);
    m_sampleTimer.stop();
    m_heartbeatTimer.stop();
    apply(m_recorder.stopAll(bootClockMs()));
    // Blocking, and queued behind every write before it: when it returns,
    // everything is on the card and the database closed, on its own thread.
    QMetaObject::invokeMethod(
            m_pStore.get(),
            [pStore = m_pStore.get()]() {
                pStore->close();
            },
            Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

QList<history::Row> DeckHistory::rowsOf(int index) const {
    if (index < 0 || index >= m_sessions.size()) {
        return {};
    }
    const QList<int> open = index == 0 ? m_openPlays.values() : QList<int>();
    QList<history::Row> rows = history::rowsOf(history::segmentsOf(
            m_sessions.at(index).playsUntil(open, index == 0 ? bootClockMs() : -1)));
    // The latest at the top, the oldest at the bottom (Sam).
    std::reverse(rows.begin(), rows.end());
    return rows;
}

int DeckHistory::trackCountOf(int index) const {
    QSet<int> tracks;
    for (const history::Row& row : rowsOf(index)) {
        for (const int track : row.tracks) {
            tracks.insert(track);
        }
    }
    return tracks.size();
}

QString DeckHistory::artworkPath(const HistoryTrack& track) const {
    if (track.artwork.isEmpty()) {
        return QString();
    }
    // With no store, the cover where it lies, for as long as it does.
    return QDir(m_artworkDirectory).filePath(track.artwork);
}

void DeckHistory::sample() {
    const qint64 now = bootClockMs();
    QList<history::Sample> playing;

    // This deck. Its own track is the one DeckLoader put on it, named by the
    // medium it came from, whichever player's that is.
    if (m_pPlay->get() > 0.0 && m_pTrackLoaded->get() > 0.0 &&
            m_pLoader->loadedMedium().isValid()) {
        const MediumId& medium = m_pLoader->loadedMedium();
        const quint32 rekordboxId = m_pLoader->loadedRekordboxId();
        const QString key = mediumTrackKey(medium, rekordboxId);
        if (!m_origins.contains(key)) {
            Origin origin;
            origin.medium = medium;
            origin.rekordboxId = rekordboxId;
            origin.rekordbox = rekordboxId != 0;
            m_origins.insert(key, origin);
        }
        playing.append({history::kThisDeck, key});
    }

    // The link: every other player, as its status has it.
    if (m_pNetwork) {
        for (const mixxx::prolink::ProLinkPlayer& player : m_pNetwork->players()) {
            if (!player.hasStatus || player.statusAgeMs < 0.0) {
                continue;
            }
            if (player.statusAgeMs > kStatusGoneMs) {
                // Gone mid-play, as far as anyone can tell. Its track stops
                // where it was last heard, not two seconds of nothing later.
                m_lastHeard.insert(player.number,
                        now - static_cast<qint64>(player.statusAgeMs));
                continue;
            }
            m_lastHeard.remove(player.number);
            if (!statusSaysPlaying(player)) {
                continue;
            }
            playing.append({player.number, keyFor(player)});
        }
    }

    const QList<history::Recorder::Change> changes = m_recorder.update(now, playing);
    if (!changes.isEmpty()) {
        apply(changes);
    }
}

void DeckHistory::heartbeat() {
    if (m_openPlays.isEmpty()) {
        // Nothing plays: nothing is written.
        return;
    }
    const qint64 now = bootClockMs();
    const QList<int> open = m_openPlays.values();
    HistoryWrite write;
    for (HistoryPlay& play : current().plays) {
        if (open.contains(play.id)) {
            play.play.endMs = now;
            write.ends.append({play.id, now});
        }
    }
    post(write);
}

void DeckHistory::apply(const QList<history::Recorder::Change>& changes) {
    HistoryWrite write;
    for (const history::Recorder::Change& change : changes) {
        if (change.kind == history::Recorder::Change::Kind::Stopped) {
            const int playId = m_openPlays.take(change.player);
            for (HistoryPlay& play : current().plays) {
                if (play.id != playId) {
                    continue;
                }
                qint64 end = change.atMs;
                const auto heard = m_lastHeard.constFind(change.player);
                if (heard != m_lastHeard.constEnd()) {
                    end = std::min(end, *heard);
                }
                play.play.endMs = std::max(play.play.startMs, end);
                write.ends.append({play.id, play.play.endMs});
                kLogger.debug() << "player" << change.player << "stopped" << change.trackKey;
                break;
            }
            continue;
        }
        HistoryPlay play;
        play.id = m_nextPlayId++;
        play.play.player = change.player;
        play.play.track = trackIdFor(change.trackKey, &write);
        play.play.startMs = change.atMs;
        play.play.endMs = change.atMs;
        current().plays.append(play);
        m_openPlays.insert(change.player, play.id);
        write.newPlays.append(play);
        kLogger.debug() << "player" << change.player << "started" << change.trackKey;
    }
    post(write);
    emit changed();
}

QString DeckHistory::keyFor(const mixxx::prolink::ProLinkPlayer& player) {
    const MediumId medium = m_pRegistry->mediumOf(player.trackSourcePlayer, player.trackSourceSlot);
    QString key;
    if (player.trackId != 0 && player.trackIsRekordbox && medium.isValid()) {
        key = mediumTrackKey(medium, player.trackId);
    } else if (player.trackId != 0) {
        // The player's own numbering, on its own medium: a plain file, which
        // no database the deck has can name. Told apart all the same, so one
        // played twice is one track.
        key = QStringLiteral("unanalysed:%1:%2:%3~%4")
                      .arg(medium.isValid() ? medium.key()
                                            : QString::number(player.trackSourcePlayer))
                      .arg(static_cast<int>(player.trackSourceSlot))
                      .arg(player.trackId)
                      .arg(m_epoch);
    } else {
        // Playing, and naming nothing: a TriMixxx deck playing a stick it
        // does not serve, which announces no track.
        key = QStringLiteral("player:%1~%2").arg(player.number).arg(m_epoch);
    }
    if (!m_origins.contains(key)) {
        Origin origin;
        origin.medium = medium;
        origin.rekordboxId = player.trackId;
        origin.rekordbox = player.trackId != 0 && player.trackIsRekordbox && medium.isValid();
        origin.sourcePlayer = player.trackId != 0 ? player.trackSourcePlayer : player.number;
        origin.slot = player.trackSourceSlot;
        m_origins.insert(key, origin);
    }
    return key;
}

QString DeckHistory::mediumTrackKey(const MediumId& medium, quint32 rekordboxId) {
    return QStringLiteral("%1~%2.%3#%4")
            .arg(medium.key(), m_epoch)
            .arg(generationOf(medium))
            .arg(rekordboxId);
}

int DeckHistory::generationOf(const MediumId& medium) {
    // A remote medium is a player's slot, and a slot holds one stick after
    // another: two DJs share a CDJ in one night, and both sticks number their
    // tracks from 1. So which stick it is goes in the key, as what it says of
    // itself -- its name and its counts -- and a different one moves the
    // number on. The same stick seen again, after a player dropped off the
    // link for a moment, is the same stick.
    //
    // Counts of 0 are a slot the player has not described yet, which says
    // nothing either way.
    QString fingerprint;
    const int index = m_pRegistry->indexOf(medium);
    if (index >= 0) {
        const MediumInfo& info = m_pRegistry->media().at(index);
        if (info.trackCount > 0) {
            fingerprint = QStringLiteral("%1|%2|%3")
                                  .arg(info.name)
                                  .arg(info.trackCount)
                                  .arg(info.playlistCount);
        }
    }
    Generation& generation = m_generations[medium.key()];
    if (!fingerprint.isEmpty() && fingerprint != generation.fingerprint) {
        if (!generation.fingerprint.isEmpty()) {
            ++generation.number;
        }
        generation.fingerprint = fingerprint;
    }
    return generation.number;
}

int DeckHistory::trackIdFor(const QString& key, HistoryWrite* pWrite) {
    const auto found = m_trackIds.constFind(key);
    if (found != m_trackIds.constEnd()) {
        return *found;
    }
    HistoryTrack track;
    track.id = m_nextTrackId++;
    track.key = key;
    if (!resolve(&track, m_origins.value(key))) {
        m_unnamed.insert(track.id);
    }
    m_trackIds.insert(key, track.id);
    current().tracks.append(track);
    pWrite->newTracks.append(track);
    return track.id;
}

bool DeckHistory::resolve(HistoryTrack* pTrack, const Origin& origin) {
    // Where it came from: the medium's own name and its player, as the
    // source list shows them.
    const int index = origin.medium.isValid() ? m_pRegistry->indexOf(origin.medium) : -1;
    if (index >= 0) {
        const MediumInfo& info = m_pRegistry->media().at(index);
        pTrack->sourceName = info.name;
        pTrack->sourcePlayer = origin.medium.isLocal()
                ? (m_pNetwork ? m_pNetwork->announcedNumber() : 0)
                : info.playerNumber;
        pTrack->sourceSlot = info.kind == MediumInfo::Kind::Sd ? QStringLiteral("sd")
                                                               : QStringLiteral("usb");
    } else {
        pTrack->sourcePlayer = origin.sourcePlayer;
        pTrack->sourceSlot = slotName(origin.slot);
    }
    if (!origin.rekordbox && !origin.medium.isLocal()) {
        // Nothing will name it: an unknown track, from there.
        return true;
    }
    if (!origin.medium.isValid()) {
        return true;
    }

    // Its row in what the deck read of that medium, if it has been read.
    QString coverPath;
    {
        const mixxx::DbConnectionPooler pooler(m_dbConnectionPool);
        QSqlDatabase database = mixxx::DbConnectionPooled(m_dbConnectionPool);
        if (!database.isOpen()) {
            return false;
        }
        QSqlQuery query(database);
        query.prepare(QStringLiteral("SELECT %1, %2, %3 FROM %4 "
                                     "WHERE medium = :medium AND rb_id = :rb LIMIT 1")
                              .arg(LIBRARYTABLE_TITLE,
                                      LIBRARYTABLE_ARTIST,
                                      LIBRARYTABLE_COVERART_LOCATION,
                                      kLibraryTable));
        query.bindValue(QStringLiteral(":medium"), origin.medium.key());
        query.bindValue(QStringLiteral(":rb"), origin.rekordboxId);
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            return false;
        }
        if (!query.next()) {
            // Not read yet: named when it is (resolvePending()).
            return false;
        }
        pTrack->title = query.value(0).toString();
        pTrack->artist = query.value(1).toString();
        coverPath = query.value(2).toString();
    }
    if (!coverPath.isEmpty()) {
        // Named after the track and the cover, never the cover alone: every
        // rekordbox stick keeps its art at the same few paths.
        const QString name = QString::fromLatin1(
                                     QCryptographicHash::hash(
                                             (pTrack->key + QChar('|') + coverPath).toUtf8(),
                                             QCryptographicHash::Sha1)
                                             .toHex()
                                             .left(20)) +
                QStringLiteral(".jpg");
        if (m_stored) {
            pTrack->artwork = name;
            requestArtwork(coverPath, name);
        } else {
            // No store to keep a copy in: the cover where it is, for as long
            // as it is there.
            pTrack->artwork = coverPath;
        }
    }
    return true;
}

void DeckHistory::requestArtwork(const QString& coverPath, const QString& name) {
    if (QFileInfo::exists(coverPath)) {
        QMetaObject::invokeMethod(
                m_pStore.get(),
                [pStore = m_pStore.get(), coverPath, name]() {
                    pStore->saveArtwork(coverPath, name);
                },
                Qt::QueuedConnection);
        return;
    }
    // A player's cover, not fetched yet: asked for, and made a thumbnail of
    // when it lands (the constructor's artworkArrived).
    m_coversWaited[coverPath].append(name);
    m_pRegistry->requestArtwork(coverPath);
}

void DeckHistory::resolvePending() {
    if (m_unnamed.isEmpty()) {
        return;
    }
    HistoryWrite write;
    for (HistoryTrack& track : current().tracks) {
        if (!m_unnamed.contains(track.id)) {
            continue;
        }
        const auto origin = m_origins.constFind(track.key);
        if (origin == m_origins.constEnd()) {
            // From before Mixxx started again: nothing left to name it from.
            m_unnamed.remove(track.id);
            continue;
        }
        if (resolve(&track, *origin)) {
            m_unnamed.remove(track.id);
            write.changedTracks.append(track);
        }
    }
    if (!write.isEmpty()) {
        post(write);
        emit changed();
    }
}

void DeckHistory::post(const HistoryWrite& write) {
    if (write.isEmpty() || !m_stored) {
        return;
    }
    QMetaObject::invokeMethod(
            m_pStore.get(),
            [pStore = m_pStore.get(), session = current().id, write]() {
                pStore->write(session, write);
            },
            Qt::QueuedConnection);
}

} // namespace deck
} // namespace mixxx
