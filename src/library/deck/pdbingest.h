#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

#include "library/deck/mediumid.h"
#include "network/prolink/prolinkpdb.h"

namespace mixxx {
namespace deck {

/// The deck's browsing tables, and the one thing that writes them.
///
/// **One schema for both sources.** A stick in this deck and a slot on another
/// player carry the same file format, and until now they were read by two
/// parsers into two tables with two sets of columns — which is how `key_id` came
/// to exist on one side only, and how every Camelot-sorted list on the other
/// came back empty. They are the same thing and they now land in the same place,
/// told apart by `deck_library.medium` alone.
///
/// Temporary in the same sense the tables they replace were: created at startup,
/// dropped at shutdown, never migrated. Nothing here is the user's data — it is
/// a cache of what is on a USB stick, and it is worthless the moment that stick
/// is unplugged.
extern const QString kLibraryTable;      ///< deck_library
extern const QString kPlaylistsTable;    ///< deck_playlists
extern const QString kPlaylistTracksTable; ///< deck_playlist_tracks

/// Create the tables if they are not there. Safe to call repeatedly.
bool createTables(QSqlDatabase& database);
void dropTables(QSqlDatabase& database);

/// Delete everything belonging to one medium, so a re-read replaces rather than
/// duplicates.
void clearMedium(QSqlDatabase& database, const MediumId& medium);

/// The cache key for one cover, derived from its **medium-relative** artwork
/// path rather than from the image.
///
/// CoverArtCache indexes its pixmaps by this and loads the image from the
/// location beside it, so it never has to be the digest of any actual bytes —
/// which is what makes it usable for a remote medium, whose images have not
/// been downloaded when the row is written. Paths are unique per image on a
/// medium, so two covers cannot collide.
///
/// Shared rather than computed at each site because the two sites have to
/// agree: `deck_library.coverart_digest` and the CoverInfo the deck is handed
/// key the same pixmap cache, and a disagreement means the same image cached
/// and decoded twice under two names.
QByteArray artworkDigest(const QString& artworkPath);

/// What one ingest produced, for the source row's summary line.
struct IngestResult {
    int trackCount = 0;
    int playlistCount = 0; ///< Playlists only; folders are not offered as one.
    int folderCount = 0;
    /// Rows written to `deck_history`: every track of every past session the
    /// medium carries, so a busy stick contributes far more than it has tracks.
    int historyCount = 0;
};

/// Write a parsed medium into the tables, replacing whatever was there for it.
///
/// *localRoot* is the prefix track locations are built from: the mount point for
/// a stick, the download cache for a remote medium. Paths are **concatenated,
/// never translated** — the pdb stores them medium-relative with a leading
/// slash, so mirroring the player's tree locally means the two never have to be
/// reconciled afterwards.
IngestResult writeMedium(QSqlDatabase& database,
        const mixxx::prolink::PdbContents& contents,
        const MediumId& medium,
        const QString& localRoot);

} // namespace deck
} // namespace mixxx
