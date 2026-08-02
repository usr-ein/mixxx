#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "library/deck/mediumid.h"

namespace mixxx {
namespace deck {

/// The whole browse hierarchy, as SQL.
///
/// Every track list in the browser is one SELECT yielding `(track_id, position)`
/// and every menu above it is one GROUP BY. Keeping them together means the
/// hierarchy can be read in one place and tested without a UI.
///
/// **The queries return finished SQL, not bound statements**, because they end
/// up inside a `CREATE TEMPORARY VIEW` (see DeckTrackModel) and a view cannot
/// carry placeholders. Every value that reaches one goes through FieldEscaper,
/// which is not decoration: genre and album names come off a stranger's USB
/// stick and routinely contain apostrophes.
namespace query {

/// Everything on the medium, in the order rekordbox stored it.
QString allTracks(QSqlDatabase& db, const MediumId& medium);
/// One playlist, in the DJ's own order.
QString playlist(int playlistId);
/// Tracks whose *column* equals *value*; an empty value means the "—" row, i.e.
/// everything with nothing in that field.
QString byTextColumn(QSqlDatabase& db,
        const MediumId& medium,
        const QString& column,
        const QString& value);
/// One artist's one album. Both may be empty, meaning "not set".
QString byArtistAlbum(QSqlDatabase& db,
        const MediumId& medium,
        const QString& artist,
        const QString& album);
QString byKeyId(QSqlDatabase& db, const MediumId& medium, int keyId);
/// A tempo bucket: `lo <= bpm < hi`.
QString byBpmRange(QSqlDatabase& db, const MediumId& medium, double lo, double hi);
/// Title, artist and album only — genre has its own category, and including it
/// turns one-word queries into noise (browser-prd.md 10).
QString search(QSqlDatabase& db, const MediumId& medium, const QString& needle);
/// This medium's tracks, most recently played first, merging the stick's own
/// rekordbox history with what this deck has played since boot.
QString lastPlayed(QSqlDatabase& db, const MediumId& medium);

} // namespace query

/// One row of a category menu — a genre, an album, a key, a tempo bucket.
struct CategoryEntry {
    /// What the row shows.
    QString display;
    /// What identifies it in a follow-up query. Same as `display` for text
    /// categories; the bucket bounds and the key id live in the fields below.
    QString value;
    int count = 0;
    /// One cover for the whole group, for the categories that show art. Empty
    /// where nothing under the value has any, and for the text-only categories.
    QString coverPath;
    int keyId = -1;
    double bpmLo = 0.0;
    double bpmHi = 0.0;
};

/// One row of the Playlists menu: a playlist, or a folder of them.
struct PlaylistEntry {
    int id = 0; ///< deck_playlists.id, for query::playlist()
    int rbId = 0;
    QString name;
    bool isFolder = false;
    int trackCount = 0;
    int durationSeconds = 0;
    int childCount = 0; ///< Folders only: how many entries are inside.
    /// Up to four covers, for the 2x2 stitch. Playlists only.
    QStringList coverPaths;
};

/// Distinct values of a text column, alphabetically, each with its track count.
/// *withCover* adds one cover per value, for Album and Label.
QList<CategoryEntry> textCategory(QSqlDatabase& db,
        const MediumId& medium,
        const QString& column,
        bool withCover);

/// The albums one artist appears on, with a cover each.
QList<CategoryEntry> albumsForArtist(QSqlDatabase& db,
        const MediumId& medium,
        const QString& artist);

/// Keys in **Camelot wheel order** — 1A, 1B, 2A, 2B … 12B — which is not
/// alphabetical and not numeric: as text, `10A` sorts between `1A` and `11A`.
QList<CategoryEntry> keyCategory(QSqlDatabase& db, const MediumId& medium);

/// Dates as rekordbox stored them, newest first.
QList<CategoryEntry> dateCategory(QSqlDatabase& db, const MediumId& medium);

/// Playlists and folders directly inside *parentRbId* (0 = the top level).
QList<PlaylistEntry> playlistsIn(QSqlDatabase& db,
        const MediumId& medium,
        int parentRbId);

/// How many entries each category would show, for the medium menu's counts.
int categoryCount(QSqlDatabase& db, const MediumId& medium, const QString& column);
int trackCount(QSqlDatabase& db, const MediumId& medium);
int playlistCount(QSqlDatabase& db, const MediumId& medium);

/// Tempo buckets — the BPM menu (browser-prd.md 7.4).
namespace bpm {

/// One fader's reach, as a fraction: the deck's rate range, 0.06 for +/-6 %.
/// Capped at +/-20 BPM in WIDE, where the range is +/-100 % and a single bucket
/// would otherwise swallow the medium.
constexpr double kMaxHalfWidthBpm = 20.0;

/// The tempo the list should open on, in this order:
///  1. what this deck is playing (rate-adjusted -- what you would mix against),
///  2. failing that, what a Pro DJ Link player is playing,
///  3. failing that, the medium's own density peak.
/// The caller supplies 1 and 2; this computes 3.
///
/// The peak is the centre of the 5-BPM-wide window holding the most tracks,
/// which puts the list where the music actually is instead of at 60 BPM.
double densityPeak(QSqlDatabase& db, const MediumId& medium);

/// Buckets tiled **outward from** *referenceBpm*, so the bucket the list opens
/// on is exactly centred on what is playing rather than happening to contain it
/// near an edge -- where half the mixable tracks would be in the next bucket.
///
/// Returns them low to high; *pAnchorIndex* receives the index of the bucket
/// built around the reference, which is the row to select.
QList<CategoryEntry> buckets(QSqlDatabase& db,
        const MediumId& medium,
        double rateRange,
        double referenceBpm,
        int* pAnchorIndex);

} // namespace bpm

/// Harmonic compatibility, for colouring a key green (browser-prd.md 8.3).
namespace key {

/// True when *candidate* mixes with *playing*: the same key, its relative
/// major/minor, or one step round the Camelot wheel either way. Nothing else --
/// energy jumps are a matter of taste and would turn half the list green.
///
/// Both are Mixxx key ids (`KeyUtils::KeyId`); an invalid id is never
/// compatible, so a track with no key and a deck with nothing loaded both
/// simply stay uncoloured.
bool isCompatible(int playingKeyId, int candidateKeyId);

} // namespace key

} // namespace deck
} // namespace mixxx
