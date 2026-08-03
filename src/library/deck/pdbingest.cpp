#include "library/deck/pdbingest.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>

#include "library/coverart.h"
#include "library/dao/trackschema.h"
#include "library/deck/camelot.h"
#include "library/queryutil.h"
#include "track/keyutils.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("DeckIngest");

/// Separator for a playlist's full path, matching what the Rekordbox feature
/// used. Transitional: it only exists to keep `deck_playlists.name` unique per
/// medium while the old library view is still reading these tables through
/// BaseExternalPlaylistModel, which looks playlists up by name.
const QString kPathDelimiter = QStringLiteral("-->");
} // namespace

namespace mixxx {
namespace deck {

const QString kLibraryTable = QStringLiteral("deck_library");
const QString kPlaylistsTable = QStringLiteral("deck_playlists");
const QString kPlaylistTracksTable = QStringLiteral("deck_playlist_tracks");

bool createTables(QSqlDatabase& database) {
    QSqlQuery query(database);

    // The column names are Mixxx's own wherever Mixxx has an equivalent
    // (trackschema.h), not names of our choosing. ColumnCache maps the model's
    // columns by name, so `datetime_added` and `timesplayed` get sorting,
    // formatting and the Camelot key machinery for free, where `date_added` and
    // `play_count` would have needed a special case at every call site.
    //
    // `label` has no Mixxx equivalent and is read directly by the browser.
    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS %1 ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    medium TEXT NOT NULL,"
            "    rb_id INTEGER NOT NULL,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    year TEXT,"
            "    genre TEXT,"
            "    label TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            // Camelot notation *and* Camelot sorting both key off this rather
            // than off the key text: ColumnCache builds a CASE over key_id from
            // the notation preference. Without it a medium shows whatever
            // rekordbox wrote and sorts it as a string, so 10A lands between 1A
            // and 11A -- and when the column is missing entirely the ORDER BY
            // names a field the table does not have, the SELECT fails outright,
            // and the whole list comes back empty.
            "    key_id INTEGER,"
            // Camelot wheel position as a SORTABLE integer. key_id above is
            // Mixxx's ChromaticKey enum, i.e. chromatic order, and sorting a
            // track list on it gives 11A, 6A, 1A -- right chromatically and
            // nonsense to a DJ. SQL cannot know about the wheel, so the order
            // is computed once here and stored.
            "    camelot_order INTEGER,"
            "    rating INTEGER,"
            "    datetime_added TEXT,"
            "    timesplayed INTEGER,"
            "    analyze_path TEXT,"
            "    color INTEGER,"
            "    artwork_path TEXT,"
            // The id is what the image is *requested* by over the network --
            // dbserver answers GET_ARTWORK by id and never sees a path -- while
            // the path is where the result is cached. Both are needed, and
            // neither derives from the other.
            "    artwork_id INTEGER,"
            // The cover-art columns BaseSqlTableModel reads directly.
            // Populating them means the table shows covers from the model
            // alone, with no Track object -- which is the whole point: a Track
            // cannot exist until the audio is readable, and covers should be
            // visible while browsing. `coverart` itself is what the Cover column
            // binds to; without it the column is not offered at all, however
            // well populated the others are.
            "    coverart TEXT,"
            "    coverart_source INTEGER,"
            "    coverart_type INTEGER,"
            "    coverart_location TEXT,"
            "    coverart_color INTEGER,"
            "    coverart_digest BLOB,"
            // Neither `location` nor `analyze_path` is unique on its own, and
            // assuming otherwise cost both of the tables this replaces a real
            // bug: two media holding clones of the same rekordbox export --
            // routine, and how a two-player setup is usually prepared --
            // produce identical values for both. The second INSERT then failed
            // silently and every one of its tracks landed in the playlist
            // tables with a track_id of -1.
            "    UNIQUE(medium, rb_id)"
            ")")
                          .arg(kLibraryTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    // Every category the browser offers is a GROUP BY or a filter over one
    // medium, so `medium` leads each of these.
    const QStringList indexes = {
            QStringLiteral("CREATE INDEX IF NOT EXISTS deck_library_medium "
                           "ON %1(medium)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS deck_library_medium_genre "
                           "ON %1(medium, genre)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS deck_library_medium_artist "
                           "ON %1(medium, artist, album)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS deck_library_medium_bpm "
                           "ON %1(medium, bpm)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS deck_library_medium_camelot "
                           "ON %1(medium, camelot_order)"),
    };
    for (const QString& statement : indexes) {
        query.prepare(statement.arg(kLibraryTable));
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            return false;
        }
    }

    // The playlist *tree*, folders included, rather than a flat list of names.
    // rekordbox nests arbitrarily deep and the browser walks it by parent_rb_id.
    //
    // `name` is transitional and will go once the old library view does: it is
    // the namespaced full path that BaseExternalPlaylistModel looks a playlist
    // up by. `display_name` is what a human sees.
    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS %1 ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    medium TEXT NOT NULL,"
            "    rb_id INTEGER NOT NULL,"
            "    parent_rb_id INTEGER NOT NULL DEFAULT 0,"
            "    name TEXT UNIQUE,"
            "    display_name TEXT,"
            "    is_folder INTEGER NOT NULL DEFAULT 0,"
            "    sort_order INTEGER NOT NULL DEFAULT 0,"
            "    UNIQUE(medium, rb_id)"
            ")")
                          .arg(kPlaylistsTable));
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
                          .arg(kPlaylistTracksTable,
                                  kPlaylistsTable,
                                  kLibraryTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    query.prepare(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS deck_playlist_tracks_playlist "
            "ON %1(playlist_id, position)")
                          .arg(kPlaylistTracksTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    // The two halves of "Last played" (browser-prd.md 7.6).
    //
    // deck_history is the medium's OWN history: rekordbox writes one playlist
    // per player mount, so this is what other people's CDJs played off this
    // stick. We mount read-only and never add to it.
    //
    // deck_play_log is ours, and only ever this boot's -- it lives in the same
    // temporary tables as everything else here and goes with them. There is no
    // timestamp in the stick's history to compare against, only a session and a
    // position, which is why the merge ranks ours above it wholesale rather
    // than interleaving the two.
    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS deck_history ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    medium TEXT NOT NULL,"
            "    track_id INTEGER REFERENCES %1(id),"
            "    session INTEGER NOT NULL DEFAULT 0,"
            "    position INTEGER NOT NULL DEFAULT 0"
            ")")
                          .arg(kLibraryTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    query.prepare(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS deck_play_log ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    track_id INTEGER REFERENCES %1(id),"
            "    played_at INTEGER NOT NULL"
            ")")
                          .arg(kLibraryTable));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

void dropTables(QSqlDatabase& database) {
    QSqlQuery query(database);
    // Entries first: they reference both other tables.
    for (const QString& table : {kPlaylistTracksTable, kPlaylistsTable, kLibraryTable}) {
        query.prepare(QStringLiteral("DROP TABLE IF EXISTS %1").arg(table));
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
        }
    }
}

void clearMedium(QSqlDatabase& database, const MediumId& medium) {
    QSqlQuery query(database);

    // Playlist-track rows first: they reference library and playlist rows, and
    // leaving them behind would point at ids about to be reused by another
    // medium.
    query.prepare(QStringLiteral(
            "DELETE FROM %1 WHERE track_id IN "
            "(SELECT id FROM %2 WHERE medium = :medium)")
                          .arg(kPlaylistTracksTable, kLibraryTable));
    query.bindValue(QStringLiteral(":medium"), medium.key());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    query.prepare(QStringLiteral(
            "DELETE FROM %1 WHERE playlist_id IN "
            "(SELECT id FROM %2 WHERE medium = :medium)")
                          .arg(kPlaylistTracksTable, kPlaylistsTable));
    query.bindValue(QStringLiteral(":medium"), medium.key());
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    for (const QString& table : {kPlaylistsTable, kLibraryTable}) {
        query.prepare(QStringLiteral("DELETE FROM %1 WHERE medium = :medium").arg(table));
        query.bindValue(QStringLiteral(":medium"), medium.key());
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
        }
    }
}

IngestResult writeMedium(QSqlDatabase& database,
        const mixxx::prolink::PdbContents& contents,
        const MediumId& medium,
        const QString& localRoot) {
    using mixxx::prolink::PdbPlaylist;
    using mixxx::prolink::PdbTrack;

    IngestResult result;

    // **One transaction for the whole medium.** Without it SQLite autocommits
    // every statement: 651 tracks plus a playlist row each plus one row per
    // playlist membership is well over a thousand transactions, each a journal
    // write and an fsync to the SD card at 10-20 ms. That was fifteen seconds
    // to read a stick.
    //
    // It is also what stopped the GUI. The collection database is in
    // rollback-journal mode, where a writer holds an exclusive lock and readers
    // BLOCK -- so a thousand short write locks on this worker thread locked the
    // GUI thread out of its own queries, over and over, for the whole read. The
    // work was already off the GUI thread; the lock contention was not.
    ScopedTransaction transaction(database);
    clearMedium(database, medium);

    QSqlQuery insertTrack(database);
    // OR REPLACE so a re-read of the same medium updates rather than failing on
    // UNIQUE(medium, rb_id). Within one medium the pair cannot repeat.
    insertTrack.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO %1 "
            "(medium, rb_id, artist, title, album, year, genre, label, tracknumber, "
            " location, comment, duration, bitrate, bpm, key, key_id, camelot_order, rating, "
            " datetime_added, timesplayed, analyze_path, color, artwork_path, "
            " artwork_id, coverart_source, coverart_type, coverart_location, "
            " coverart_digest) "
            "VALUES (:medium, :rb_id, :artist, :title, :album, :year, :genre, :label, "
            " :tracknumber, :location, :comment, :duration, :bitrate, :bpm, :key, "
            " :key_id, :camelot_order, :rating, :datetime_added, :timesplayed, "
            " :analyze_path, :color, "
            " :artwork_path, :artwork_id, :coverart_source, :coverart_type, "
            " :coverart_location, :coverart_digest)")
                                .arg(kLibraryTable));
    // prepare() returning false is how a malformed statement announces itself,
    // and ignoring it is how a column list that had drifted out of step with its
    // placeholders turned into "wrote 0 tracks" and an empty library, once per
    // row, with only a debug-level warning to show for it.
    VERIFY_OR_DEBUG_ASSERT(insertTrack.lastError().type() == QSqlError::NoError) {
        LOG_FAILED_QUERY(insertTrack) << "could not prepare the track insert";
        return result;
    }

    // rb_id -> our row id, so playlist entries resolve without a query each. A
    // 651-track medium with a 40-entry playlist is small; the same code will
    // meet a 10,000-track library.
    QHash<quint32, int> rowIds;
    rowIds.reserve(contents.tracks.size());

    for (const PdbTrack& track : contents.tracks) {
        insertTrack.bindValue(QStringLiteral(":medium"), medium.key());
        insertTrack.bindValue(QStringLiteral(":rb_id"), track.id);
        insertTrack.bindValue(QStringLiteral(":artist"), track.artist);
        insertTrack.bindValue(QStringLiteral(":title"), track.title);
        insertTrack.bindValue(QStringLiteral(":album"), track.album);
        insertTrack.bindValue(QStringLiteral(":year"),
                track.year > 0 ? QString::number(track.year) : QString());
        insertTrack.bindValue(QStringLiteral(":genre"), track.genre);
        insertTrack.bindValue(QStringLiteral(":label"), track.label);
        insertTrack.bindValue(QStringLiteral(":tracknumber"),
                track.trackNumber > 0 ? QString::number(track.trackNumber) : QString());
        insertTrack.bindValue(QStringLiteral(":location"),
                QString(localRoot + track.filePath));
        insertTrack.bindValue(QStringLiteral(":comment"), track.comment);
        insertTrack.bindValue(QStringLiteral(":duration"), track.durationSeconds);
        insertTrack.bindValue(QStringLiteral(":bitrate"), track.bitrate);
        // Centi-BPM only becomes a double here, at the last moment, so 12800
        // cannot pick up rounding error on the way through.
        insertTrack.bindValue(QStringLiteral(":bpm"), track.tempoCentiBpm / 100.0);
        insertTrack.bindValue(QStringLiteral(":key"), track.key);
        // Parsed from the text, which is all a pdb gives us that is portable:
        // the numeric key_id is per-medium and not a canonical index -- `11A` is
        // row 12 on one export and row 2 on another -- so it cannot be stored
        // directly. An unrecognised spelling yields INVALID and sorts with the
        // other unknowns.
        const auto chromaticKey = KeyUtils::guessKeyFromText(track.key);
        insertTrack.bindValue(QStringLiteral(":key_id"), static_cast<int>(chromaticKey));
        insertTrack.bindValue(QStringLiteral(":camelot_order"), camelot::order(chromaticKey));
        insertTrack.bindValue(QStringLiteral(":rating"), track.rating);
        // Stored exactly as rekordbox wrote it (YYYY-MM-DD), which sorts
        // chronologically as text, so the Date added category needs no parsing.
        insertTrack.bindValue(QStringLiteral(":datetime_added"), track.dateAdded);
        insertTrack.bindValue(QStringLiteral(":timesplayed"), track.playCount);
        insertTrack.bindValue(QStringLiteral(":analyze_path"),
                QString(localRoot + track.analyzePath));
        insertTrack.bindValue(QStringLiteral(":color"), QVariant());
        // Kept as the *medium-relative* path: it is what has to be asked for
        // over the network, and the local location is derivable from it.
        // Storing the local one instead would mean reconstructing the remote
        // path by stripping a prefix, which is exactly the reconciliation the
        // concatenation scheme avoids.
        insertTrack.bindValue(QStringLiteral(":artwork_path"), track.artworkPath);
        insertTrack.bindValue(QStringLiteral(":artwork_id"), track.artworkId);
        if (track.artworkPath.isEmpty()) {
            insertTrack.bindValue(QStringLiteral(":coverart_source"),
                    static_cast<int>(CoverInfo::UNKNOWN));
            insertTrack.bindValue(QStringLiteral(":coverart_type"),
                    static_cast<int>(CoverInfo::NONE));
            insertTrack.bindValue(QStringLiteral(":coverart_location"), QString());
            insertTrack.bindValue(QStringLiteral(":coverart_digest"), QByteArray());
        } else {
            insertTrack.bindValue(QStringLiteral(":coverart_source"),
                    static_cast<int>(CoverInfo::GUESSED));
            insertTrack.bindValue(QStringLiteral(":coverart_type"),
                    static_cast<int>(CoverInfo::FILE));
            insertTrack.bindValue(QStringLiteral(":coverart_location"),
                    QString(localRoot + track.artworkPath));
            // Without a cache key BaseSqlTableModel::getCoverInfo() returns an
            // empty CoverInfo -- it reads type, source and location only
            // `if (coverInfo.hasCacheKey())` -- so the column stays blank
            // however correct the location is.
            //
            // The key is opaque: CoverArtCache uses it to index its pixmap cache
            // and loads the image from coverLocation, so it can be derived from
            // the artwork *path* rather than from the image bytes. That matters
            // for a remote medium, whose images have not been downloaded yet.
            // Paths are unique per image on a medium, so two covers cannot
            // collide.
            insertTrack.bindValue(QStringLiteral(":coverart_digest"),
                    QCryptographicHash::hash(
                            track.artworkPath.toUtf8(), QCryptographicHash::Sha1));
        }
        if (!insertTrack.exec()) {
            LOG_FAILED_QUERY(insertTrack);
            continue;
        }
        rowIds.insert(track.id, insertTrack.lastInsertId().toInt());
    }
    result.trackCount = rowIds.size();

    QSqlQuery insertPlaylist(database);
    insertPlaylist.prepare(QStringLiteral(
            "INSERT INTO %1 (medium, rb_id, parent_rb_id, name, display_name, "
            " is_folder, sort_order) "
            "VALUES (:medium, :rb_id, :parent_rb_id, :name, :display_name, "
            " :is_folder, :sort_order)")
                                   .arg(kPlaylistsTable));
    VERIFY_OR_DEBUG_ASSERT(insertPlaylist.lastError().type() == QSqlError::NoError) {
        LOG_FAILED_QUERY(insertPlaylist) << "could not prepare the playlist insert";
        return result;
    }

    QSqlQuery insertEntry(database);
    insertEntry.prepare(QStringLiteral(
            "INSERT INTO %1 (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)")
                                .arg(kPlaylistTracksTable));

    // A playlist's full path, walked up through its folders. Only used to build
    // the transitional unique `name`; the browser navigates by parent_rb_id.
    const auto fullPath = [&contents](const PdbPlaylist& playlist) {
        QStringList parts{playlist.name};
        quint32 parentId = playlist.parentId;
        // Bounded by the number of entries: a malformed export could otherwise
        // describe a cycle, and walking one would hang the read.
        for (int depth = 0; parentId != 0 && depth <= contents.playlists.size(); ++depth) {
            const auto parent = contents.playlists.constFind(parentId);
            if (parent == contents.playlists.constEnd()) {
                break;
            }
            parts.prepend(parent->name);
            parentId = parent->parentId;
        }
        return parts.join(kPathDelimiter);
    };

    const auto addEntries = [&](int playlistId, const QList<quint32>& trackIds) {
        int position = 1;
        for (const quint32 rbId : trackIds) {
            const auto rowId = rowIds.constFind(rbId);
            if (rowId == rowIds.constEnd()) {
                // An entry naming a track that is not in the tracks table. Real
                // in a pdb, and skipping it is right: inserting -1 would put a
                // phantom row in the playlist, which is what the Rekordbox
                // feature's UNIQUE collision used to do to every track on a
                // second, cloned medium.
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

    const auto addPlaylist = [&](quint32 rbId,
                                     quint32 parentRbId,
                                     const QString& displayName,
                                     const QString& uniqueName,
                                     bool isFolder,
                                     quint32 sortOrder) {
        insertPlaylist.bindValue(QStringLiteral(":medium"), medium.key());
        insertPlaylist.bindValue(QStringLiteral(":rb_id"), rbId);
        insertPlaylist.bindValue(QStringLiteral(":parent_rb_id"), parentRbId);
        // Namespaced by medium: two sticks can both hold a "Warmup", and the
        // column the old view looks up by is globally unique.
        insertPlaylist.bindValue(QStringLiteral(":name"),
                QStringLiteral("%1|%2").arg(medium.key(), uniqueName));
        insertPlaylist.bindValue(QStringLiteral(":display_name"), displayName);
        insertPlaylist.bindValue(QStringLiteral(":is_folder"), isFolder ? 1 : 0);
        insertPlaylist.bindValue(QStringLiteral(":sort_order"), sortOrder);
        if (!insertPlaylist.exec()) {
            LOG_FAILED_QUERY(insertPlaylist);
            return -1;
        }
        return insertPlaylist.lastInsertId().toInt();
    };

    for (const PdbPlaylist& playlist : contents.playlists) {
        const int playlistId = addPlaylist(playlist.id,
                playlist.parentId,
                playlist.name,
                fullPath(playlist),
                playlist.isFolder,
                playlist.sortOrder);
        if (playlistId < 0) {
            continue;
        }
        if (playlist.isFolder) {
            ++result.folderCount;
            continue;
        }
        ++result.playlistCount;
        addEntries(playlistId, playlist.trackIds);
    }

    // "All tracks", in pdb order, as a real playlist row. Transitional in the
    // same way `name` is: the browser builds its own All tracks straight off
    // deck_library, but the old view can only show a playlist.
    //
    // rb_id 0 cannot collide -- rekordbox numbers playlists from 1, and 0 is
    // what it uses to mean "no parent".
    QList<quint32> everything;
    everything.reserve(contents.tracks.size());
    for (const PdbTrack& track : contents.tracks) {
        everything.append(track.id);
    }
    const int allTracksId = addPlaylist(0,
            0,
            QStringLiteral("All tracks"),
            QStringLiteral("All tracks"),
            false,
            0);
    if (allTracksId >= 0) {
        addEntries(allTracksId, everything);
    }

    if (rowIds.isEmpty() && !contents.tracks.isEmpty()) {
        // Every row failed. Loud, because the visible symptom -- an empty track
        // list -- looks identical to a medium that genuinely has no tracks, and
        // the two want completely different investigations.
        kLogger.warning() << "wrote NO tracks for" << medium.key() << "despite"
                          << contents.tracks.size()
                          << "parsed; the insert is failing, see the query above";
        transaction.commit();
    } else {
        transaction.commit();
        kLogger.info() << "wrote" << result.trackCount << "tracks,"
                       << result.playlistCount << "playlists and"
                       << result.folderCount << "folders for" << medium.key();
    }
    return result;
}

} // namespace deck
} // namespace mixxx
