#include "library/prolink/prolinkdbwriter.h"

#include <QSqlError>
#include <QSqlQuery>

#include "library/dao/trackschema.h"
#include "library/queryutil.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkDb");
} // namespace

namespace mixxx {
namespace prolink {

const QString kProLinkLibraryTable = QStringLiteral("prolink_library");
const QString kProLinkPlaylistsTable = QStringLiteral("prolink_playlists");
const QString kProLinkPlaylistTracksTable = QStringLiteral("prolink_playlist_tracks");

QString deviceKeyFor(const QByteArray& mac, MediaSlot slot) {
    return QStringLiteral("%1|%2")
            .arg(QString::fromLatin1(mac.toHex()))
            .arg(static_cast<int>(slot));
}

bool createProLinkTables(QSqlDatabase& database) {
    QSqlQuery query(database);

    // Neither `location` nor `analyze_path` is UNIQUE on its own, which is the
    // fix the Rekordbox feature also needed: two devices holding clones of the
    // same media -- routine, and how a two-player setup is usually prepared --
    // produce identical values for both, and the second INSERT would fail
    // silently, leaving every one of its tracks with a track_id of -1.
    //
    // What is actually unique is (device, rb_id), which is also what the lookups
    // key on.
    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS %1 ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    rb_id INTEGER,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    year TEXT,"
            "    genre TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    rating INTEGER,"
            "    analyze_path TEXT,"
            "    device TEXT,"
            "    color INTEGER,"
            "    UNIQUE(device, rb_id)"
            ")").arg(kProLinkLibraryTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS %1 ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    name TEXT UNIQUE"
            ")").arg(kProLinkPlaylistsTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS %1 ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES %2(id),"
            "    track_id INTEGER REFERENCES %3(id),"
            "    position INTEGER"
            ")")
                          .arg(kProLinkPlaylistTracksTable,
                                  kProLinkPlaylistsTable,
                                  kProLinkLibraryTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

void dropProLinkTables(QSqlDatabase& database) {
    QSqlQuery query(database);
    for (const QString& table : {kProLinkPlaylistTracksTable,
                 kProLinkPlaylistsTable,
                 kProLinkLibraryTable}) {
        query.prepare(QStringLiteral("DROP TABLE IF EXISTS %1").arg(table));
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
        }
    }
}

void clearMedium(QSqlDatabase& database, const QString& deviceKey) {
    QSqlQuery query(database);
    // Playlist-track rows first: they reference library rows, and leaving them
    // behind would point at ids that are about to be reused by another medium.
    query.prepare(QStringLiteral(
            "DELETE FROM %1 WHERE track_id IN "
            "(SELECT id FROM %2 WHERE device = :device)")
                          .arg(kProLinkPlaylistTracksTable, kProLinkLibraryTable));
    query.bindValue(QStringLiteral(":device"), deviceKey);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare(QStringLiteral("DELETE FROM %1 WHERE device = :device")
                          .arg(kProLinkLibraryTable));
    query.bindValue(QStringLiteral(":device"), deviceKey);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare(QStringLiteral("DELETE FROM %1 WHERE name LIKE :prefix")
                          .arg(kProLinkPlaylistsTable));
    query.bindValue(QStringLiteral(":prefix"),
            QString(deviceKey + QStringLiteral("|%")));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

int writeMedium(QSqlDatabase& database,
        const PdbContents& contents,
        const QString& deviceKey,
        const QString& localRoot) {
    clearMedium(database, deviceKey);

    QSqlQuery insertTrack(database);
    // OR REPLACE so a re-fetch of the same medium updates rather than failing on
    // UNIQUE(device, rb_id). Within one medium the pair cannot repeat.
    insertTrack.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO %1 "
            "(rb_id, artist, title, album, year, genre, tracknumber, location, "
            " comment, duration, bitrate, bpm, key, rating, analyze_path, device, color) "
            "VALUES (:rb_id, :artist, :title, :album, :year, :genre, :tracknumber, "
            " :location, :comment, :duration, :bitrate, :bpm, :key, :rating, "
            " :analyze_path, :device, :color)")
                                .arg(kProLinkLibraryTable));

    // rb_id -> our row id, so playlist entries can be resolved without a query
    // per entry. A 651-track medium with a 40-entry playlist is small, but the
    // same code will meet a 10,000-track library.
    QHash<quint32, int> rowIds;

    for (const PdbTrack& track : contents.tracks) {
        insertTrack.bindValue(QStringLiteral(":rb_id"), track.id);
        insertTrack.bindValue(QStringLiteral(":artist"), track.artist);
        insertTrack.bindValue(QStringLiteral(":title"), track.title);
        insertTrack.bindValue(QStringLiteral(":album"), track.album);
        insertTrack.bindValue(QStringLiteral(":year"),
                track.year > 0 ? QString::number(track.year) : QString());
        insertTrack.bindValue(QStringLiteral(":genre"), track.genre);
        insertTrack.bindValue(QStringLiteral(":tracknumber"), QString());
        insertTrack.bindValue(QStringLiteral(":location"),
                QString(localRoot + track.filePath));
        insertTrack.bindValue(QStringLiteral(":comment"), track.comment);
        insertTrack.bindValue(QStringLiteral(":duration"), track.durationSeconds);
        insertTrack.bindValue(QStringLiteral(":bitrate"), track.bitrate);
        // Centi-BPM only becomes a double here, at the last moment, so 12800
        // cannot pick up rounding error on the way through.
        insertTrack.bindValue(QStringLiteral(":bpm"), track.tempoCentiBpm / 100.0);
        insertTrack.bindValue(QStringLiteral(":key"), track.key);
        insertTrack.bindValue(QStringLiteral(":rating"), track.rating);
        insertTrack.bindValue(QStringLiteral(":analyze_path"),
                QString(localRoot + track.analyzePath));
        insertTrack.bindValue(QStringLiteral(":device"), deviceKey);
        insertTrack.bindValue(QStringLiteral(":color"), QVariant());
        if (!insertTrack.exec()) {
            LOG_FAILED_QUERY(insertTrack);
            continue;
        }
        rowIds.insert(track.id, insertTrack.lastInsertId().toInt());
    }

    QSqlQuery insertPlaylist(database);
    insertPlaylist.prepare(
            QStringLiteral("INSERT INTO %1 (name) VALUES (:name)")
                    .arg(kProLinkPlaylistsTable));
    QSqlQuery insertEntry(database);
    insertEntry.prepare(QStringLiteral(
            "INSERT INTO %1 (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)")
                                .arg(kProLinkPlaylistTracksTable));

    // Playlist names are namespaced by medium, because the playlists table has
    // a UNIQUE name and two sticks can both hold a "Warmup".
    auto addPlaylist = [&](const QString& name, const QList<quint32>& trackIds) {
        insertPlaylist.bindValue(QStringLiteral(":name"),
                QStringLiteral("%1|%2").arg(deviceKey, name));
        if (!insertPlaylist.exec()) {
            LOG_FAILED_QUERY(insertPlaylist);
            return;
        }
        const int playlistId = insertPlaylist.lastInsertId().toInt();
        int position = 1;
        for (const quint32 rbId : trackIds) {
            const auto rowId = rowIds.constFind(rbId);
            if (rowId == rowIds.constEnd()) {
                // An entry naming a track that is not in the tracks table. Real
                // in a pdb, and skipping it is right: inserting -1 would put a
                // phantom row in the playlist, which is what the Rekordbox
                // feature's UNIQUE collision used to do.
                continue;
            }
            insertEntry.bindValue(QStringLiteral(":playlist_id"), playlistId);
            insertEntry.bindValue(QStringLiteral(":track_id"), *rowId);
            insertEntry.bindValue(QStringLiteral(":position"), position++);
            if (!insertEntry.exec()) {
                LOG_FAILED_QUERY(insertEntry);
            }
        }
    };

    // "All tracks" in pdb order, then the curated playlists.
    QList<quint32> everything;
    everything.reserve(contents.tracks.size());
    for (const PdbTrack& track : contents.tracks) {
        everything.append(track.id);
    }
    addPlaylist(QStringLiteral("All tracks"), everything);

    for (const PdbPlaylist& playlist : contents.playlists) {
        if (playlist.isFolder) {
            continue;
        }
        addPlaylist(playlist.name, playlist.trackIds);
    }

    kLogger.info() << "wrote" << rowIds.size() << "tracks for" << deviceKey;
    return rowIds.size();
}

} // namespace prolink
} // namespace mixxx
