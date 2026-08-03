#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QString>

namespace mixxx {
namespace prolink {

/// One track, as read out of a player's `export.pdb`.
struct PdbTrack {
    quint32 id = 0;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString key;
    QString label;
    QString color;
    QString comment;
    /// Relative to the medium root, with a leading slash, exactly as the
    /// database stores it — which is what a fetch takes verbatim.
    QString filePath;
    /// The `.DAT` beside it, holding the beat grid and the waveforms.
    QString analyzePath;
    QString artworkPath;
    QString dateAdded;
    quint32 year = 0;
    quint32 durationSeconds = 0;
    quint32 bitrate = 0;
    /// Hundredths of a BPM, so 145.00 is 14500.
    quint32 tempoCentiBpm = 0;
    quint32 rating = 0;
    quint32 artworkId = 0;
    quint32 sampleRate = 0;
    quint32 fileSize = 0;
    quint32 trackNumber = 0;
    quint32 discNumber = 0;
    quint32 playCount = 0;
    /// The container byte, which is what a deck uses to decide how to decode
    /// the file.
    quint32 fileType = 0;
    quint32 artistId = 0;
    quint32 albumId = 0;
    quint32 genreId = 0;
    /// Per-medium, and not a canonical index: `11A` is row 12 on one export
    /// and row 2 on another.
    quint32 keyId = 0;
    quint32 labelId = 0;
    quint32 colorId = 0;

    /// The `.EXT` beside the `.DAT`, holding what the newer players read.
    QString analyzeExtPath() const;
};

/// A playlist, or a folder of them.
struct PdbPlaylist {
    quint32 id = 0;
    quint32 parentId = 0;
    quint32 sortOrder = 0;
    QString name;
    bool isFolder = false;
    /// Empty for a folder, and in the DJ's own order for a playlist: one
    /// re-sorted alphabetically is a different playlist.
    QList<quint32> trackIds;
};

/// One history playlist: what a player played off this medium, in order.
///
/// rekordbox writes one every time a player mounts the medium.
///
/// **No time of its own.** The row is an id and a name (`HISTORY 001`) and
/// nothing else, and the entries under it are track/playlist/index — verified
/// against a real 651-track export. So ordering is all these carry: a later id
/// is a later session, and a later position within one is later in that
/// session. That is enough to rank them among themselves, and not enough to
/// interleave them with plays this deck made — hence the tiering in
/// `query::lastPlayed()`.
///
/// **A date does exist elsewhere, undecoded.** The export also has a `history`
/// table (page type 19) that nothing here reads: 96 fixed 40-byte rows on the
/// sample, each carrying an ascending id and an ASCII `YYYY-MM-DD`, all
/// `2026-01-04` on that stick. Decoding it would give these sessions a real
/// date. It is not decoded because one export with a single distinct date is
/// not enough to tell which field means what — the row also holds two small
/// integers and a four-character string (`1000`) that could plausibly be a
/// time, a player number or neither. It wants a second export from a different
/// day before anyone guesses.
struct PdbHistoryPlaylist {
    quint32 id = 0;
    QString name; ///< `HISTORY 001` and so on.
    /// In play order.
    QList<quint32> trackIds;
};

/// Everything read out of one `export.pdb`.
struct PdbContents {
    /// Whether it parsed. On false the rest is empty and `error` says why.
    bool ok = false;
    QString error;
    QList<PdbTrack> tracks;
    /// Keyed by id, because the tree is built by following `parentId`.
    QMap<quint32, PdbPlaylist> playlists;
    /// One per player mount, oldest first.
    QList<PdbHistoryPlaylist> history;
    QHash<quint32, QString> artists;
    QHash<quint32, QString> albums;
    QHash<quint32, QString> genres;
    QHash<quint32, QString> keys;
    QHash<quint32, QString> labels;
    QHash<quint32, QString> colors;
    QHash<quint32, QString> artwork;

    /// How many of the entries are folders rather than playlists.
    int folderCount() const;
    /// How many are playlists.
    int playlistCount() const;
};

/// Read a rekordbox `export.pdb`.
///
/// **The parsing is not here.** It is `lib/prolink`'s, which reads the same
/// format, is pinned against a real 651-track export, and carries fields the
/// Kaitai schema this replaced did not — the container byte among them, which
/// is what a deck uses to decide how to decode a file.
///
/// Pure: no database, no `TreeItem`, no Qt GUI types. That is what lets it be
/// tested against a real pdb with no hardware and no Library.
PdbContents parsePdb(const QByteArray& data);

} // namespace prolink
} // namespace mixxx
