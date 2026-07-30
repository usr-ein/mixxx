#pragma once

#include <QSqlDatabase>
#include <QString>

#include "library/prolink/prolinkpdbimport.h"
#include "network/prolink/prolinkdefs.h"

namespace mixxx {
namespace prolink {

/// Temporary tables holding what we have browsed this session.
///
/// Temporary in the same sense as the Rekordbox feature's: created at startup,
/// dropped at shutdown, never migrated. Nothing here is user data — it is a
/// cache of what is on someone else's USB stick, and it is worthless the moment
/// that stick is unplugged.
extern const QString kProLinkLibraryTable;
extern const QString kProLinkPlaylistsTable;
extern const QString kProLinkPlaylistTracksTable;

/// The `device` column value for one medium: the owning player's MAC and its
/// slot. Two players holding clones of the same stick must not collide, and two
/// slots on one player must not either.
QString deviceKeyFor(const QByteArray& mac, MediaSlot slot);

/// Create the tables if they are not there. Safe to call repeatedly.
bool createProLinkTables(QSqlDatabase& database);
void dropProLinkTables(QSqlDatabase& database);

/// Delete everything belonging to one medium, so a re-fetch replaces rather
/// than duplicates.
void clearMedium(QSqlDatabase& database, const QString& deviceKey);

/// Write a parsed medium into the tables.
///
/// *localRoot* is where the medium's files will live once fetched; track
/// locations are built as `localRoot + filePath`, with the path taken verbatim
/// from the database. Concatenating rather than translating is deliberate — the
/// pdb's paths are already medium-relative with a leading slash, so mirroring
/// the player's tree locally means the two never have to be reconciled.
///
/// Returns the number of tracks written.
int writeMedium(QSqlDatabase& database,
        const PdbContents& contents,
        const QString& deviceKey,
        const QString& localRoot);

} // namespace prolink
} // namespace mixxx
