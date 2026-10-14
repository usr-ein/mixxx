#include "library/deck/historystore.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <algorithm>

#include "moc_historystore.cpp"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("HistoryStore");

const QString kDatabaseName = QStringLiteral("history.sqlite");
const QString kArtworkDirectory = QStringLiteral("art");
/// Its schema, for whoever changes it next.
constexpr int kSchemaVersion = 1;
/// The write-ahead log's size before it is checkpointed and begun again, in
/// pages: a heartbeat is one page, so five minutes of play.
constexpr int kLogPages = 64;

bool exec(QSqlQuery& query) {
    if (query.exec()) {
        return true;
    }
    kLogger.warning() << "failed:" << query.lastQuery() << "--" << query.lastError().text();
    return false;
}

bool exec(QSqlDatabase& database, const QString& statement) {
    QSqlQuery query(database);
    query.prepare(statement);
    return exec(query);
}
} // namespace

namespace mixxx {
namespace deck {

const HistoryTrack* HistorySession::track(int id) const {
    for (const HistoryTrack& track : tracks) {
        if (track.id == id) {
            return &track;
        }
    }
    return nullptr;
}

QList<history::Play> HistorySession::playsUntil(
        const QList<int>& openPlayIds, qint64 nowMs) const {
    QList<history::Play> list;
    list.reserve(plays.size());
    for (const HistoryPlay& stored : plays) {
        history::Play play = stored.play;
        if (nowMs >= 0 && openPlayIds.contains(stored.id)) {
            play.endMs = std::max(play.endMs, nowMs);
        }
        list.append(play);
    }
    return list;
}

HistoryStore::HistoryStore(QObject* pParent)
        : QObject(pParent),
          m_connectionName(QStringLiteral("trimixxx-history-%1")
                                   .arg(reinterpret_cast<quintptr>(this))) {
}

HistoryStore::~HistoryStore() {
    if (m_database.isOpen()) {
        // DeckHistory closes it on its own thread, where it was opened; this
        // is the backstop, and Qt warns from it.
        kLogger.warning() << "closed from its destructor";
        close();
    }
}

QString HistoryStore::artworkPath(const QString& name) const {
    if (name.isEmpty()) {
        return QString();
    }
    // An absolute name is a cover where it lies, for a History with no store.
    return QDir(QDir(m_directory).filePath(kArtworkDirectory)).filePath(name);
}

HistoryStore::Opened HistoryStore::open(
        const QString& directory, const QByteArray& bootId, qint64 nowWallMs) {
    Opened opened;
    m_directory = directory;
    if (directory.isEmpty() || !QDir(directory).exists()) {
        opened.error = QStringLiteral("no directory %1").arg(directory);
        kLogger.warning() << "no store:" << opened.error;
        return opened;
    }
    QDir(directory).mkpath(kArtworkDirectory);
    const QString path = QDir(directory).filePath(kDatabaseName);

    QString error;
    bool ok = openDatabase(path, &error) && createSchema() && rotate(bootId, nowWallMs);
    if (!ok) {
        // A database that will not open, or will not take a write, is put
        // aside rather than mended: it is three nights of track names, and a
        // deck that refuses to start over them is a deck that does not play
        // tonight. Kept, not deleted, so a person can look at it.
        kLogger.warning() << "cannot use" << path << "--" << error << "; putting it aside";
        close();
        for (const QString& suffix :
                {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            QFile::remove(path + suffix + QStringLiteral(".broken"));
            QFile::rename(path + suffix, path + suffix + QStringLiteral(".broken"));
        }
        ok = openDatabase(path, &error) && createSchema() && rotate(bootId, nowWallMs);
    }
    if (!ok) {
        opened.error = error.isEmpty() ? QStringLiteral("cannot write %1").arg(path) : error;
        kLogger.warning() << "no store:" << opened.error;
        close();
        return opened;
    }
    opened.ok = true;
    opened.sessions = readSessions();
    dropUnusedArtwork();
    kLogger.info() << "open at" << path << "with" << opened.sessions.size() << "session(s)";
    return opened;
}

bool HistoryStore::openDatabase(const QString& path, QString* pError) {
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(path);
    if (!m_database.open()) {
        *pError = m_database.lastError().text();
        return false;
    }
    // A file that is not a database opens without complaint: SQLite reads
    // nothing until it is asked something.
    {
        QSqlQuery check(m_database);
        if (!check.exec(QStringLiteral("PRAGMA quick_check")) || !check.next() ||
                check.value(0).toString() != QStringLiteral("ok")) {
            *pError = QStringLiteral("quick_check: %1").arg(check.lastError().isValid()
                            ? check.lastError().text()
                            : check.value(0).toString());
            return false;
        }
        // Finished, or the statement holds a read open, and SQLite will not
        // change the journal under it: journal_mode answers with the old one.
        check.finish();
    }
    // WAL: a commit appends to the log and syncs it, and the database proper
    // is written only at a checkpoint. FULL: every commit is synced before it
    // returns, so what write() wrote is on the card when it says so -- NORMAL
    // would let a cut take the last commits back.
    {
        QSqlQuery mode(m_database);
        if (!mode.exec(QStringLiteral("PRAGMA journal_mode=WAL")) || !mode.next() ||
                mode.value(0).toString().toLower() != QStringLiteral("wal")) {
            *pError = QStringLiteral("journal_mode=WAL refused (%1)")
                              .arg(mode.isValid() ? mode.value(0).toString()
                                                  : mode.lastError().text());
            return false;
        }
        mode.finish();
    }
    // A small log, checkpointed often, so that it is written over rather
    // than grown: a commit that grows the file commits its new size and its
    // blocks to the filesystem's journal as well. At SQLite's default of a
    // thousand pages, the first 80 minutes of every power-up's heartbeats
    // grow it. Measured on an emulated deck, one track playing: 29 MB an hour
    // to the card while it grows, 13 once it is written over.
    return exec(m_database, QStringLiteral("PRAGMA synchronous=FULL")) &&
            exec(m_database, QStringLiteral("PRAGMA wal_autocheckpoint=%1").arg(kLogPages)) &&
            exec(m_database, QStringLiteral("PRAGMA foreign_keys=ON"));
}

bool HistoryStore::createSchema() {
    // Rows name their session, and go with it.
    const QStringList statements = {
            QStringLiteral("CREATE TABLE IF NOT EXISTS sessions ("
                           "  id INTEGER PRIMARY KEY,"
                           "  boot_id TEXT NOT NULL UNIQUE,"
                           "  opened_wall_ms INTEGER NOT NULL"
                           ")"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS tracks ("
                           "  session_id INTEGER NOT NULL"
                           "    REFERENCES sessions(id) ON DELETE CASCADE,"
                           "  id INTEGER NOT NULL,"
                           "  key TEXT NOT NULL,"
                           "  title TEXT NOT NULL DEFAULT '',"
                           "  artist TEXT NOT NULL DEFAULT '',"
                           "  source_name TEXT NOT NULL DEFAULT '',"
                           "  source_player INTEGER NOT NULL DEFAULT 0,"
                           "  source_slot TEXT NOT NULL DEFAULT '',"
                           "  artwork TEXT NOT NULL DEFAULT '',"
                           "  PRIMARY KEY (session_id, id)"
                           ")"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS plays ("
                           "  session_id INTEGER NOT NULL"
                           "    REFERENCES sessions(id) ON DELETE CASCADE,"
                           "  id INTEGER NOT NULL,"
                           "  track_id INTEGER NOT NULL,"
                           "  player INTEGER NOT NULL,"
                           "  start_ms INTEGER NOT NULL,"
                           "  end_ms INTEGER NOT NULL,"
                           "  PRIMARY KEY (session_id, id)"
                           ")"),
            QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion),
    };
    for (const QString& statement : statements) {
        if (!exec(m_database, statement)) {
            return false;
        }
    }
    return true;
}

bool HistoryStore::rotate(const QByteArray& bootId, qint64 nowWallMs) {
    if (!m_database.transaction()) {
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id FROM sessions WHERE boot_id = :boot"));
    query.bindValue(QStringLiteral(":boot"), QString::fromLatin1(bootId));
    if (!exec(query)) {
        m_database.rollback();
        return false;
    }
    if (query.next()) {
        // Mixxx started again within one power-up: the same session, on.
        return m_database.commit();
    }

    // The power-up before, if the History lists nothing for it, makes way for
    // this one rather than for the oldest: decided by the list's own rule, so
    // that a power-up shown as "nothing played" is one that pushes nothing
    // out.
    bool ok = true;
    query.prepare(QStringLiteral("SELECT MAX(id) FROM sessions"));
    if (!exec(query)) {
        m_database.rollback();
        return false;
    }
    if (query.next() && !query.value(0).isNull()) {
        const int last = query.value(0).toInt();
        QList<history::Play> plays;
        QSqlQuery read(m_database);
        read.prepare(QStringLiteral(
                "SELECT track_id, player, start_ms, end_ms FROM plays WHERE session_id = :session"));
        read.bindValue(QStringLiteral(":session"), last);
        ok = exec(read);
        while (ok && read.next()) {
            history::Play play;
            play.track = read.value(0).toInt();
            play.player = read.value(1).toInt();
            play.startMs = read.value(2).toLongLong();
            play.endMs = read.value(3).toLongLong();
            plays.append(play);
        }
        if (ok && history::rowsOf(history::segmentsOf(plays)).isEmpty()) {
            query.prepare(QStringLiteral("DELETE FROM sessions WHERE id = :session"));
            query.bindValue(QStringLiteral(":session"), last);
            ok = exec(query);
        }
    }

    query.prepare(QStringLiteral(
            "INSERT INTO sessions (boot_id, opened_wall_ms) VALUES (:boot, :now)"));
    query.bindValue(QStringLiteral(":boot"), QString::fromLatin1(bootId));
    query.bindValue(QStringLiteral(":now"), nowWallMs);
    ok = ok && exec(query);

    query.prepare(QStringLiteral(
            "DELETE FROM sessions WHERE id NOT IN "
            "(SELECT id FROM sessions ORDER BY id DESC LIMIT :kept)"));
    query.bindValue(QStringLiteral(":kept"), kSessionsKept);
    ok = ok && exec(query);

    if (!ok) {
        m_database.rollback();
        return false;
    }
    return m_database.commit();
}

QList<HistorySession> HistoryStore::readSessions() {
    QList<HistorySession> sessions;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
            "SELECT id, boot_id, opened_wall_ms FROM sessions ORDER BY id DESC"));
    if (!exec(query)) {
        return sessions;
    }
    while (query.next()) {
        HistorySession session;
        session.id = query.value(0).toInt();
        session.bootId = query.value(1).toString().toLatin1();
        session.openedWallMs = query.value(2).toLongLong();
        sessions.append(session);
    }
    for (HistorySession& session : sessions) {
        QSqlQuery tracks(m_database);
        tracks.prepare(QStringLiteral(
                "SELECT id, key, title, artist, source_name, source_player, "
                "source_slot, artwork FROM tracks WHERE session_id = :session "
                "ORDER BY id"));
        tracks.bindValue(QStringLiteral(":session"), session.id);
        if (exec(tracks)) {
            while (tracks.next()) {
                HistoryTrack track;
                track.id = tracks.value(0).toInt();
                track.key = tracks.value(1).toString();
                track.title = tracks.value(2).toString();
                track.artist = tracks.value(3).toString();
                track.sourceName = tracks.value(4).toString();
                track.sourcePlayer = tracks.value(5).toInt();
                track.sourceSlot = tracks.value(6).toString();
                track.artwork = tracks.value(7).toString();
                session.tracks.append(track);
            }
        }
        QSqlQuery plays(m_database);
        plays.prepare(QStringLiteral(
                "SELECT id, track_id, player, start_ms, end_ms FROM plays "
                "WHERE session_id = :session ORDER BY id"));
        plays.bindValue(QStringLiteral(":session"), session.id);
        if (exec(plays)) {
            while (plays.next()) {
                HistoryPlay play;
                play.id = plays.value(0).toInt();
                play.play.track = plays.value(1).toInt();
                play.play.player = plays.value(2).toInt();
                play.play.startMs = plays.value(3).toLongLong();
                play.play.endMs = plays.value(4).toLongLong();
                session.plays.append(play);
            }
        }
    }
    return sessions;
}

void HistoryStore::write(int sessionId, const HistoryWrite& write) {
    if (!m_database.isOpen() || write.isEmpty()) {
        return;
    }
    if (!m_database.transaction()) {
        kLogger.warning() << "cannot write:" << m_database.lastError().text();
        return;
    }
    bool ok = true;
    QSqlQuery query(m_database);
    for (const HistoryTrack& track : write.newTracks) {
        query.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO tracks (session_id, id, key, title, artist, "
                "source_name, source_player, source_slot, artwork) VALUES "
                "(:session, :id, :key, :title, :artist, :name, :player, :slot, :artwork)"));
        query.bindValue(QStringLiteral(":session"), sessionId);
        query.bindValue(QStringLiteral(":id"), track.id);
        query.bindValue(QStringLiteral(":key"), track.key);
        query.bindValue(QStringLiteral(":title"), track.title);
        query.bindValue(QStringLiteral(":artist"), track.artist);
        query.bindValue(QStringLiteral(":name"), track.sourceName);
        query.bindValue(QStringLiteral(":player"), track.sourcePlayer);
        query.bindValue(QStringLiteral(":slot"), track.sourceSlot);
        query.bindValue(QStringLiteral(":artwork"), track.artwork);
        ok = ok && exec(query);
    }
    for (const HistoryTrack& track : write.changedTracks) {
        query.prepare(QStringLiteral(
                "UPDATE tracks SET title = :title, artist = :artist, "
                "source_name = :name, source_player = :player, source_slot = :slot, "
                "artwork = :artwork WHERE session_id = :session AND id = :id"));
        query.bindValue(QStringLiteral(":title"), track.title);
        query.bindValue(QStringLiteral(":artist"), track.artist);
        query.bindValue(QStringLiteral(":name"), track.sourceName);
        query.bindValue(QStringLiteral(":player"), track.sourcePlayer);
        query.bindValue(QStringLiteral(":slot"), track.sourceSlot);
        query.bindValue(QStringLiteral(":artwork"), track.artwork);
        query.bindValue(QStringLiteral(":session"), sessionId);
        query.bindValue(QStringLiteral(":id"), track.id);
        ok = ok && exec(query);
    }
    for (const HistoryPlay& play : write.newPlays) {
        query.prepare(QStringLiteral(
                "INSERT OR REPLACE INTO plays (session_id, id, track_id, player, "
                "start_ms, end_ms) VALUES (:session, :id, :track, :player, :start, :end)"));
        query.bindValue(QStringLiteral(":session"), sessionId);
        query.bindValue(QStringLiteral(":id"), play.id);
        query.bindValue(QStringLiteral(":track"), play.play.track);
        query.bindValue(QStringLiteral(":player"), play.play.player);
        query.bindValue(QStringLiteral(":start"), play.play.startMs);
        query.bindValue(QStringLiteral(":end"), play.play.endMs);
        ok = ok && exec(query);
    }
    for (const auto& [playId, endMs] : write.ends) {
        query.prepare(QStringLiteral(
                "UPDATE plays SET end_ms = :end WHERE session_id = :session AND id = :id"));
        query.bindValue(QStringLiteral(":end"), endMs);
        query.bindValue(QStringLiteral(":session"), sessionId);
        query.bindValue(QStringLiteral(":id"), playId);
        ok = ok && exec(query);
    }
    if (!ok || !m_database.commit()) {
        kLogger.warning() << "write rolled back:" << m_database.lastError().text();
        m_database.rollback();
    }
}

void HistoryStore::saveArtwork(const QString& imagePath, const QString& name) {
    if (!m_database.isOpen() || name.isEmpty()) {
        return;
    }
    const QString path = artworkPath(name);
    if (QFile::exists(path)) {
        emit artworkSaved(name);
        return;
    }
    QImage image(imagePath);
    if (image.isNull()) {
        kLogger.debug() << "no image at" << imagePath;
        return;
    }
    // Square, as every cover is drawn: scaled until the shorter side fits,
    // then the middle of the longer one. Never scaled up: a 80 px rekordbox
    // thumbnail stays the 80 px it is.
    if (image.width() > kArtworkSize || image.height() > kArtworkSize) {
        image = image.scaled(kArtworkSize,
                kArtworkSize,
                Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation);
    }
    const int side = std::min(image.width(), image.height());
    image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    // Whole or not at all: QSaveFile writes beside it, syncs, then renames.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "JPG", 85) || !file.commit()) {
        kLogger.warning() << "could not write" << path;
        return;
    }
    emit artworkSaved(name);
}

void HistoryStore::dropUnusedArtwork() {
    QSet<QString> used;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT DISTINCT artwork FROM tracks WHERE artwork != ''"));
    if (!exec(query)) {
        // Better a few kB too many than a cover pulled from under its row.
        return;
    }
    while (query.next()) {
        used.insert(query.value(0).toString());
    }
    QDir art(QDir(m_directory).filePath(kArtworkDirectory));
    for (const QString& name : art.entryList(QDir::Files)) {
        // QSaveFile's leftovers from a cut go too: never renamed, never named.
        if (!used.contains(name)) {
            art.remove(name);
        }
    }
}

void HistoryStore::close() {
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

} // namespace deck
} // namespace mixxx
