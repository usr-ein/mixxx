#include "network/prolink/prolinkpdb.h"

#include "prolink-cxx/src/lib.rs.h"

namespace {
QString toQString(const ::rust::String& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QHash<quint32, QString> toHash(const ::rust::Vec<::prolink::PdbNamed>& table) {
    QHash<quint32, QString> out;
    out.reserve(static_cast<int>(table.size()));
    for (const ::prolink::PdbNamed& row : table) {
        out.insert(row.id, toQString(row.name));
    }
    return out;
}
} // namespace

namespace mixxx {
namespace prolink {

QString PdbTrack::analyzeExtPath() const {
    if (!analyzePath.endsWith(QStringLiteral(".DAT"))) {
        return QString();
    }
    // Upper case deliberately: rekordbox writes both extensions that way and a
    // deck looks for exactly those names.
    return analyzePath.left(analyzePath.size() - 4) + QStringLiteral(".EXT");
}

int PdbContents::folderCount() const {
    int count = 0;
    for (const PdbPlaylist& playlist : playlists) {
        if (playlist.isFolder) {
            ++count;
        }
    }
    return count;
}

int PdbContents::playlistCount() const {
    return static_cast<int>(playlists.size()) - folderCount();
}

PdbContents parsePdb(const QByteArray& data) {
    const ::prolink::PdbContents parsed = ::prolink::read_pdb_bytes(
            ::rust::Slice<const ::std::uint8_t>(
                    reinterpret_cast<const ::std::uint8_t*>(data.constData()),
                    static_cast<::std::size_t>(data.size())));

    PdbContents out;
    out.ok = parsed.ok;
    out.error = toQString(parsed.error);
    if (!out.ok) {
        return out;
    }

    out.tracks.reserve(static_cast<int>(parsed.tracks.size()));
    for (const ::prolink::PdbTrack& from : parsed.tracks) {
        PdbTrack track;
        track.id = from.id;
        track.title = toQString(from.title);
        track.artist = toQString(from.artist);
        track.album = toQString(from.album);
        track.genre = toQString(from.genre);
        track.key = toQString(from.key);
        track.label = toQString(from.label);
        track.color = toQString(from.color);
        track.comment = toQString(from.comment);
        track.filePath = toQString(from.file_path);
        track.analyzePath = toQString(from.analyze_path);
        track.artworkPath = toQString(from.artwork_path);
        track.dateAdded = toQString(from.date_added);
        track.year = from.year;
        track.durationSeconds = from.duration_seconds;
        track.bitrate = from.bitrate;
        track.tempoCentiBpm = from.tempo_centibpm;
        track.rating = from.rating;
        track.artworkId = from.artwork_id;
        track.sampleRate = from.sample_rate;
        track.fileSize = from.file_size;
        track.trackNumber = from.track_number;
        track.discNumber = from.disc_number;
        track.playCount = from.play_count;
        track.fileType = from.file_type;
        track.artistId = from.artist_id;
        track.albumId = from.album_id;
        track.genreId = from.genre_id;
        track.keyId = from.key_id;
        track.labelId = from.label_id;
        track.colorId = from.color_id;
        out.tracks.append(track);
    }

    for (const ::prolink::PdbPlaylist& from : parsed.playlists) {
        PdbPlaylist playlist;
        playlist.id = from.id;
        playlist.parentId = from.parent_id;
        playlist.sortOrder = from.sort_order;
        playlist.name = toQString(from.name);
        playlist.isFolder = from.is_folder;
        playlist.trackIds.reserve(static_cast<int>(from.track_ids.size()));
        for (const ::std::uint32_t id : from.track_ids) {
            playlist.trackIds.append(id);
        }
        out.playlists.insert(playlist.id, playlist);
    }

    out.history.reserve(static_cast<int>(parsed.history.size()));
    for (const ::prolink::PdbHistoryPlaylist& from : parsed.history) {
        PdbHistoryPlaylist playlist;
        playlist.id = from.id;
        playlist.name = toQString(from.name);
        playlist.trackIds.reserve(static_cast<int>(from.track_ids.size()));
        for (const ::std::uint32_t id : from.track_ids) {
            playlist.trackIds.append(id);
        }
        out.history.append(playlist);
    }

    out.artists = toHash(parsed.artists);
    out.albums = toHash(parsed.albums);
    out.genres = toHash(parsed.genres);
    out.keys = toHash(parsed.keys);
    out.labels = toHash(parsed.labels);
    out.colors = toHash(parsed.colors);
    out.artwork = toHash(parsed.artwork);
    return out;
}

} // namespace prolink
} // namespace mixxx
