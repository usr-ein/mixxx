#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>

namespace mixxx {
namespace prolink {

/// One track, as read out of a player's `export.pdb`.
///
/// Paths are exactly as the database stores them — relative to the medium root,
/// with a leading slash — because that is what has to be handed back to the NFS
/// layer to fetch the file. Nothing is rewritten here.
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
    QString filePath;
    QString analyzePath;
    /// Cover image on the medium, resolved from the ARTWORK table via
    /// `artworkId`. Empty when the track has no art.
    QString artworkPath;
    QString dateAdded;
    quint32 year = 0;
    quint32 durationSeconds = 0;
    quint32 bitrate = 0;
    /// Tempo in centi-BPM, as stored. Kept integral deliberately: 128.00 BPM is
    /// 12800 on the wire, and going through a double and back is how a grid ends
    /// up one part in ten thousand off.
    quint32 tempoCentiBpm = 0;
    quint32 rating = 0;
    quint32 artworkId = 0;
    quint32 sampleRate = 0;
    quint32 fileSize = 0;
    quint32 trackNumber = 0;
    quint32 discNumber = 0;
    quint32 playCount = 0;
    /// Container from row offset 0x5a: mp3 1, m4a 4, flac 5, wav 11, aiff 12.
    /// dysentery calls this `unknown6`; 651 rows on one medium had no exceptions
    /// (F34).
    quint32 fileType = 0;

    /// The raw foreign keys, kept alongside the resolved names above.
    ///
    /// Serving needs both. A dbserver metadata item carries the **referenced
    /// row's** id — artist 122, album 86 — not the track's own, because that is
    /// how a player offers "more by this artist" from the metadata panel.
    /// Sending the track id there is wrong in a way that still renders
    /// perfectly, so it survives casual inspection (F32).
    quint32 artistId = 0;
    quint32 albumId = 0;
    quint32 genreId = 0;
    quint32 keyId = 0;
    quint32 labelId = 0;
    quint32 colorId = 0;

    /// The `.EXT` companion of `analyzePath`, by extension swap.
    QString analyzeExtPath() const;
};

/// A playlist or folder in the tree.
struct PdbPlaylist {
    quint32 id = 0;
    quint32 parentId = 0;
    quint32 sortOrder = 0;
    QString name;
    bool isFolder = false;
    /// Track ids in curated order. Empty for a folder.
    QList<quint32> trackIds;
};

/// Everything we take from one `export.pdb`.
struct PdbContents {
    bool ok = false;
    QString error;
    QList<PdbTrack> tracks;
    /// Keyed by id, so the tree can be walked by parent.
    QMap<quint32, PdbPlaylist> playlists;

    /// The side tables, id to name, kept rather than only folded into the
    /// tracks.
    ///
    /// Consuming a library needs the names; **serving** one needs the tables
    /// themselves. A deck browsing us asks for the list of artists before it
    /// asks for any track, and a drill-down narrows by id at each level — so
    /// "every artist on this medium" has to be answerable without walking the
    /// tracks and deduplicating strings, which would lose the ids the next
    /// request is keyed on.
    QHash<quint32, QString> artists;
    QHash<quint32, QString> albums;
    QHash<quint32, QString> genres;
    QHash<quint32, QString> keys;
    QHash<quint32, QString> labels;
    QHash<quint32, QString> colors;
    /// Artwork id to its path on the medium.
    QHash<quint32, QString> artwork;

    int folderCount() const;
    int playlistCount() const;
};

/// Parse a rekordbox `export.pdb`.
///
/// Uses the Kaitai types already vendored at `lib/rekordbox-metadata/`, which
/// the Rekordbox feature also uses — the same parser, the same schema, so a pdb
/// that browses correctly from a mounted USB browses correctly over the network.
///
/// Notably that schema declares the UTF-16 string form as **little**-endian
/// (`device_sql_long_utf16le_t`), which is correct. Our own Python proof of
/// concept had it as big-endian from the wrong offset, and the two errors
/// cancelled exactly for ASCII — a 692-track library parsed cleanly and only
/// non-ASCII titles came out as mojibake (O6). Reusing this parser rather than
/// reimplementing the string decoding avoids re-introducing that.
///
/// **Parsing needs the whole file resident**, because `page_ref_t::body()`
/// seeks. It is a megabyte, and we have it in memory from the transfer anyway.
///
/// Pure: no database, no TreeItem, no Qt GUI types. That is what lets it be
/// tested against a real pdb with no hardware and no Library.
PdbContents parsePdb(const QByteArray& data);

} // namespace prolink
} // namespace mixxx
