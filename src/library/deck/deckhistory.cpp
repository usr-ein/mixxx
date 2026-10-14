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
          m_directory(directory),
          m_artworkDirectory(QDir(directory).filePath(QStringLiteral("art"))),
          m_bootId(currentBootId()),
          m_epoch(QString::number(bootClockMs(), 36)) {
    m_thread.setObjectName(QStringLiteral("DeckHistory"));
    m_pStore->moveToThread(&m_thread);
    m_thread.start(QThread::LowPriority);

    // This power-up's History starts in RAM, and goes onto the card as soon
    // as the store opens: now, or once its directory is there.
    HistorySession session;
    session.bootId = m_bootId;
    session.openedWallMs = QDateTime::currentMSecsSinceEpoch();
    m_sessions.append(session);
    if (!openStore() && !QDir(directory).exists()) {
        // Not there yet: on a release card the state partition can be mounted
        // after Mixxx has started, and the directory is made on it once it is
        // (trimixxx-history.service); a dev deck gets it from the system
        // step. Looked for again until it is there: a stat every few seconds.
        m_reopenTimer.setInterval(kReopenMs);
        connect(&m_reopenTimer, &QTimer::timeout, this, [this]() {
            if (QDir(m_directory).exists()) {
                m_reopenTimer.stop();
                openStore();
            }
        });
        m_reopenTimer.start();
    }

    connect(m_pStore.get(),
            &HistoryStore::artworkSaved,
            this,
            &DeckHistory::artworkArrived,
            Qt::QueuedConnection);
    // A medium read is the moment its tracks can be named.
    connect(m_pRegistry, &MediaRegistry::mediaChanged, this, &DeckHistory::resolvePending);
    connect(m_pRegistry,
            &MediaRegistry::mediumAppeared,
            this,
            &DeckHistory::onMediumAppeared);
    connect(m_pRegistry,
            &MediaRegistry::artworkArrived,
            this,
            [this](const QString& coverPath) {
                const QStringList names = m_coversWaited.take(coverPath);
                for (const QString& name : names) {
                    requestArtwork(coverPath, name);
                }
            });
    if (m_pNetwork) {
        connect(m_pNetwork,
                &mixxx::prolink::ProLinkNetworkService::plainFileTagsFetched,
                this,
                &DeckHistory::onPlainFileTags);
    }

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
    m_reopenTimer.stop();
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

bool DeckHistory::openStore() {
    // Waited for: the sessions are what the History shows, and opening is a
    // few small reads.
    HistoryStore::Opened opened;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(
            m_pStore.get(),
            [this, &opened, now]() {
                opened = m_pStore->open(m_directory, m_bootId, now);
            },
            Qt::BlockingQueuedConnection);
    if (!opened.ok || opened.sessions.isEmpty() ||
            opened.sessions.first().bootId != m_bootId) {
        kLogger.warning() << "the History is not on the card:" << opened.error;
        return false;
    }
    m_stored = true;

    // What this power-up recorded in RAM before the store opened goes into
    // its session on the card, after what that already holds: plays of a
    // Mixxx started earlier within the power-up, or nothing at all.
    const HistorySession recorded = m_sessions.first();
    m_sessions = opened.sessions;
    HistorySession& here = current();
    int trackBase = 0;
    int playBase = 0;
    for (const HistoryTrack& track : std::as_const(here.tracks)) {
        trackBase = std::max(trackBase, track.id);
        m_trackIds.insert(track.key, track.id);
    }
    for (const HistoryPlay& play : std::as_const(here.plays)) {
        playBase = std::max(playBase, play.id);
    }
    HistoryWrite write;
    for (HistoryTrack track : recorded.tracks) {
        track.id += trackBase;
        if (QDir::isAbsolutePath(track.artwork)) {
            // The cover where it lay, while there was no store to keep it.
            const QString coverPath = track.artwork;
            track.artwork = thumbnailName(track.key, coverPath);
            requestArtwork(coverPath, track.artwork);
        }
        m_trackIds.insert(track.key, track.id);
        here.tracks.append(track);
        write.newTracks.append(track);
    }
    for (HistoryPlay play : recorded.plays) {
        play.id += playBase;
        play.play.track += trackBase;
        here.plays.append(play);
        write.newPlays.append(play);
    }
    for (auto it = m_openPlays.begin(); it != m_openPlays.end(); ++it) {
        it.value() += playBase;
    }
    QSet<int> unnamed;
    for (const int id : std::as_const(m_unnamed)) {
        unnamed.insert(id + trackBase);
    }
    m_unnamed = unnamed;
    m_nextTrackId += trackBase;
    m_nextPlayId += playBase;
    post(write);

    // The power-ups before this one never change again: their rows are
    // worked out once, here, rather than on every look.
    m_pastRows.clear();
    for (int i = 1; i < m_sessions.size(); ++i) {
        QList<history::Row> rows = history::rowsOf(
                history::segmentsOf(m_sessions.at(i).playsUntil({}, -1)));
        std::reverse(rows.begin(), rows.end());
        m_pastRows.append(rows);
    }
    if (!recorded.plays.isEmpty()) {
        kLogger.info() << "the History is on the card now, with" << recorded.plays.size()
                       << "play(s) from before it was";
    }
    emit changed();
    return true;
}

qint64 DeckHistory::clockMs() {
    return bootClockMs();
}

QByteArray DeckHistory::bootId() {
    return currentBootId();
}

QList<history::Play> DeckHistory::playsNow() const {
    if (m_sessions.isEmpty()) {
        return {};
    }
    return m_sessions.first().playsUntil(m_openPlays.values(), bootClockMs());
}

QList<history::Row> DeckHistory::rowsOf(int index) const {
    if (index < 0 || index >= m_sessions.size()) {
        return {};
    }
    if (index > 0) {
        return m_pastRows.value(index - 1);
    }
    QList<history::Row> rows = history::rowsOf(history::segmentsOf(
            m_sessions.first().playsUntil(m_openPlays.values(), bootClockMs())));
    // The latest at the top, the oldest at the bottom (Sam).
    std::reverse(rows.begin(), rows.end());
    return rows;
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
    // medium it came from, whichever player's that is: a player's plain file
    // by what that player says of it, as when the player plays it.
    if (m_pPlay->get() > 0.0 && m_pTrackLoaded->get() > 0.0 &&
            m_pLoader->loadedMedium().isValid()) {
        const MediumId& medium = m_pLoader->loadedMedium();
        const quint32 rekordboxId = m_pLoader->loadedRekordboxId();
        const QString key = mediumTrackKey(medium, rekordboxId);
        if (!m_origins.contains(key)) {
            Origin origin;
            origin.medium = medium;
            origin.rekordboxId = rekordboxId;
            origin.plain = rekordboxId != 0 && !medium.isLocal() &&
                    m_pRegistry->formatOf(medium) == MediumInfo::Format::Folder;
            origin.rekordbox = rekordboxId != 0 && !origin.plain;
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
    // The deck's own row for the track, when it has one: a rekordbox track's
    // id is its row's. A plain file off a stick of ours is named by the id we
    // serve it under, which is no row's, and is looked up by its path; then
    // one played here and on a player is one track.
    quint32 row = player.trackIsRekordbox ? player.trackId : 0;
    if (player.trackId != 0 && !player.trackIsRekordbox && medium.isLocal()) {
        row = m_pRegistry->rowOfServedFile(medium, player.trackId);
    }
    // A plain file off a player's stick the deck has read by its folders: its
    // row is keyed on the player's own id, so one played here and there is
    // one track. It is still named by what its player says of it, which is
    // the file's own tags whether or not the deck has got to them yet.
    const bool plainRemote = player.trackId != 0 && !player.trackIsRekordbox &&
            !medium.isLocal() && medium.isValid() &&
            m_pRegistry->formatOf(medium) == MediumInfo::Format::Folder;
    QString key;
    if (row != 0 && medium.isValid()) {
        key = mediumTrackKey(medium, row);
    } else if (plainRemote) {
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
        origin.rekordboxId = row != 0 ? row : player.trackId;
        origin.rekordbox = row != 0 && medium.isValid();
        origin.plain = player.trackId != 0 && !player.trackIsRekordbox && row == 0;
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

int DeckHistory::generationOf(const MediumId& medium) const {
    return m_generations.value(medium.key()).number;
}

void DeckHistory::onMediumAppeared(const MediumInfo& medium) {
    // A remote medium is a player's slot, and a slot holds one stick after
    // another: two DJs share a CDJ in one night, and both sticks number their
    // tracks from 1. So which stick it is goes in a track's key, as what the
    // player says of it when it appears -- its name and counts, from the one
    // description a medium is born with -- and a different stick moves the
    // number on. The same stick seen again, after its player dropped off the
    // link for a moment, is the same stick. A stick of our own needs none:
    // its id carries its filesystem's UUID.
    if (medium.id.isLocal()) {
        return;
    }
    const QString fingerprint = QStringLiteral("%1|%2|%3")
                                        .arg(medium.name)
                                        .arg(medium.trackCount)
                                        .arg(medium.playlistCount);
    Generation& generation = m_generations[medium.id.key()];
    if (!generation.fingerprint.isEmpty() && generation.fingerprint != fingerprint) {
        ++generation.number;
    }
    generation.fingerprint = fingerprint;
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
    if (origin.plain) {
        // A plain file, by its player's own id: the player that holds it is
        // the one thing that can name it. Off a stick of ours it is a file the
        // deck has no row for, and nothing names it.
        return origin.medium.isLocal() || !askPlainTags(*pTrack, origin);
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
        if (m_stored) {
            pTrack->artwork = thumbnailName(pTrack->key, coverPath);
            requestArtwork(coverPath, pTrack->artwork);
        } else {
            // No store to keep a copy in: the cover where it is, for as long
            // as it is there.
            pTrack->artwork = coverPath;
        }
    }
    return true;
}

QString DeckHistory::thumbnailName(const QString& trackKey, const QString& coverPath) {
    // After the track and the cover, never the cover alone: every rekordbox
    // stick keeps its art at the same few paths.
    return QString::fromLatin1(QCryptographicHash::hash(
                                       (trackKey + QChar('|') + coverPath).toUtf8(),
                                       QCryptographicHash::Sha1)
                                       .toHex()
                                       .left(20)) +
            QStringLiteral(".jpg");
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

bool DeckHistory::askPlainTags(const HistoryTrack& track, const Origin& origin) {
    QByteArray mac;
    mixxx::prolink::MediaSlot slot = origin.slot;
    if (!m_pNetwork || !MediaRegistry::addressOf(origin.medium, &mac, &slot)) {
        return false;
    }
    const QString asked = QStringLiteral("%1|%2|%3")
                                  .arg(QString::fromLatin1(mac.toHex()))
                                  .arg(static_cast<int>(slot))
                                  .arg(origin.rekordboxId);
    QList<int>& waiting = m_plainAsked[asked];
    if (!waiting.contains(track.id)) {
        waiting.append(track.id);
    }
    if (waiting.size() == 1) {
        m_pNetwork->fetchPlainFileTags(mac, slot, origin.rekordboxId);
    }
    return true;
}

void DeckHistory::onPlainFileTags(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const mixxx::prolink::PlainFileTags& tags,
        const QString& error) {
    const QList<int> waiting = m_plainAsked.take(QStringLiteral("%1|%2|%3")
                    .arg(QString::fromLatin1(mac.toHex()))
                    .arg(static_cast<int>(slot))
                    .arg(tags.id));
    if (waiting.isEmpty()) {
        return;
    }
    if (!error.isEmpty()) {
        // Still unnamed, and asked again when the media next change.
        kLogger.info() << "a player did not say what its plain file" << tags.id << "is:"
                       << error;
        return;
    }
    HistoryWrite write;
    for (HistoryTrack& track : current().tracks) {
        if (!waiting.contains(track.id) || !m_unnamed.contains(track.id)) {
            continue;
        }
        track.title = tags.title;
        track.artist = tags.artist;
        m_unnamed.remove(track.id);
        write.changedTracks.append(track);
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
