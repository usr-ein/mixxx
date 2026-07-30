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
    QString comment;
    QString filePath;
    QString analyzePath;
    quint32 year = 0;
    quint32 durationSeconds = 0;
    quint32 bitrate = 0;
    /// Tempo in centi-BPM, as stored. Kept integral deliberately: 128.00 BPM is
    /// 12800 on the wire, and going through a double and back is how a grid ends
    /// up one part in ten thousand off.
    quint32 tempoCentiBpm = 0;
    quint32 rating = 0;
    quint32 colorId = 0;
    quint32 artworkId = 0;
    /// Container from row offset 0x5a: mp3 1, m4a 4, flac 5, wav 11, aiff 12.
    /// dysentery calls this `unknown6`; 651 rows on one medium had no exceptions
    /// (F34).
    quint32 fileType = 0;
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
