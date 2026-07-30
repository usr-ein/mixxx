#include "library/prolink/prolinkpdbimport.h"

#include <QByteArray>
#include <QStringDecoder>
#include <string>

#include "rekordbox_pdb.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkPdb");

template<typename Base, typename T>
bool isA(const T* pointer) {
    return dynamic_cast<const Base*>(pointer) != nullptr;
}

/// Decode one DeviceSQL string, in whichever of its three forms it is stored.
///
/// QStringDecoder rather than QTextCodec: the latter is Qt5 compatibility API,
/// and the Rekordbox feature's use of it is the older idiom rather than the one
/// to copy.
QString textOf(rekordbox_pdb_t::device_sql_string_t* pString) {
    if (pString == nullptr) {
        return QString();
    }
    auto* pBody = pString->body();
    if (isA<rekordbox_pdb_t::device_sql_short_ascii_t>(pBody)) {
        return QString::fromStdString(
                static_cast<rekordbox_pdb_t::device_sql_short_ascii_t*>(pBody)->text());
    }
    if (isA<rekordbox_pdb_t::device_sql_long_ascii_t>(pBody)) {
        return QString::fromStdString(
                static_cast<rekordbox_pdb_t::device_sql_long_ascii_t*>(pBody)->text());
    }
    if (isA<rekordbox_pdb_t::device_sql_long_utf16le_t>(pBody)) {
        const std::string& raw =
                static_cast<rekordbox_pdb_t::device_sql_long_utf16le_t*>(pBody)->text();
        // Little-endian, and the schema says so. Do not "fix" this to BE.
        QStringDecoder decoder(QStringDecoder::Utf16LE);
        QString text = decoder(QByteArrayView(raw.data(), static_cast<qsizetype>(raw.size())));
        // The form carries a trailing NUL inside its length.
        while (text.endsWith(QChar(0))) {
            text.chop(1);
        }
        return text;
    }
    return QString();
}
} // namespace

namespace mixxx {
namespace prolink {

int PdbContents::folderCount() const {
    int count = 0;
    for (const PdbPlaylist& playlist : playlists) {
        if (playlist.isFolder) {
            count++;
        }
    }
    return count;
}

int PdbContents::playlistCount() const {
    return playlists.size() - folderCount();
}

PdbContents parsePdb(const QByteArray& data) {
    PdbContents contents;

    // Kaitai throws on anything it cannot make sense of. A pdb arrives over the
    // network from a device we do not control, so a malformed one must be an
    // error value rather than an exception escaping into the network thread.
    try {
        const std::string buffer(data.constData(), data.size());
        kaitai::kstream stream(buffer);
        rekordbox_pdb_t database(&stream);

        // Sanity-check the header before trusting the walk.
        //
        // Kaitai does not object to a buffer of zeroes: num_tables comes out 0,
        // the walk finds nothing, and we would report a perfectly valid database
        // that happens to contain no tracks. That is the worst possible outcome
        // for a file arriving over a network -- a corrupt or truncated download
        // would present as an empty medium, which looks exactly like a stick
        // with nothing on it and gives the user nothing to act on.
        constexpr quint32 kPageSize = 4096;
        if (database.len_page() != kPageSize || database.num_tables() == 0) {
            contents.error = QStringLiteral(
                    "not a rekordbox database: page size %1, %2 tables")
                                     .arg(database.len_page())
                                     .arg(database.num_tables());
            kLogger.warning() << contents.error;
            return contents;
        }

        // Same table order as the Rekordbox feature, and for the same reason:
        // the lookup tables must be populated before the track rows that
        // reference them are read. HISTORY is skipped -- its rows do not contain
        // valid row_ref_t structures, which the Rekordbox feature discovered the
        // hard way.
        const rekordbox_pdb_t::page_type_t tableOrder[] = {
                rekordbox_pdb_t::PAGE_TYPE_KEYS,
                rekordbox_pdb_t::PAGE_TYPE_GENRES,
                rekordbox_pdb_t::PAGE_TYPE_ARTISTS,
                rekordbox_pdb_t::PAGE_TYPE_ALBUMS,
                rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES,
                rekordbox_pdb_t::PAGE_TYPE_TRACKS,
                rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE,
        };

        QHash<quint32, QString> keys;
        QHash<quint32, QString> genres;
        QHash<quint32, QString> artists;
        QHash<quint32, QString> albums;
        // playlist id -> entry index -> track id. A map, not a list, because
        // entry indices are one-based and need not arrive in order.
        QMap<quint32, QMap<quint32, quint32>> playlistEntries;

        for (const auto pageType : tableOrder) {
            for (const auto& table : *database.tables()) {
                if (table->type() != pageType) {
                    continue;
                }
                const uint16_t lastIndex = table->last_page()->index();
                rekordbox_pdb_t::page_ref_t* pCurrent = table->first_page();

                while (true) {
                    rekordbox_pdb_t::page_t* pPage = pCurrent->body();
                    // "Strange" chain-head pages carry no rows and must be
                    // stepped over rather than treated as empty data pages.
                    if (pPage->is_data_page()) {
                        for (const auto& rowGroup : *pPage->row_groups()) {
                            for (const auto& rowRef : *rowGroup->rows()) {
                                // The presence bitmask: an absent slot is a
                                // deleted row, and its bytes are stale.
                                if (!rowRef->present()) {
                                    continue;
                                }
                                switch (pageType) {
                                case rekordbox_pdb_t::PAGE_TYPE_KEYS: {
                                    auto* pRow = static_cast<rekordbox_pdb_t::key_row_t*>(
                                            rowRef->body());
                                    keys[pRow->id()] = textOf(pRow->name());
                                } break;
                                case rekordbox_pdb_t::PAGE_TYPE_GENRES: {
                                    auto* pRow = static_cast<rekordbox_pdb_t::genre_row_t*>(
                                            rowRef->body());
                                    genres[pRow->id()] = textOf(pRow->name());
                                } break;
                                case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: {
                                    auto* pRow = static_cast<rekordbox_pdb_t::artist_row_t*>(
                                            rowRef->body());
                                    artists[pRow->id()] = textOf(pRow->name());
                                } break;
                                case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: {
                                    auto* pRow = static_cast<rekordbox_pdb_t::album_row_t*>(
                                            rowRef->body());
                                    albums[pRow->id()] = textOf(pRow->name());
                                } break;
                                case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES: {
                                    auto* pRow = static_cast<
                                            rekordbox_pdb_t::playlist_entry_row_t*>(
                                            rowRef->body());
                                    playlistEntries[pRow->playlist_id()]
                                                   [pRow->entry_index()] =
                                            pRow->track_id();
                                } break;
                                case rekordbox_pdb_t::PAGE_TYPE_TRACKS: {
                                    auto* pRow = static_cast<rekordbox_pdb_t::track_row_t*>(
                                            rowRef->body());
                                    PdbTrack track;
                                    track.id = pRow->id();
                                    track.title = textOf(pRow->title());
                                    track.artist = artists.value(pRow->artist_id());
                                    track.album = albums.value(pRow->album_id());
                                    track.genre = genres.value(pRow->genre_id());
                                    track.key = keys.value(pRow->key_id());
                                    track.comment = textOf(pRow->comment());
                                    track.filePath = textOf(pRow->file_path());
                                    track.analyzePath = textOf(pRow->analyze_path());
                                    track.year = pRow->year();
                                    track.durationSeconds = pRow->duration();
                                    track.bitrate = pRow->bitrate();
                                    track.tempoCentiBpm = pRow->tempo();
                                    track.rating = pRow->rating();
                                    track.colorId = pRow->color_id();
                                    track.artworkId = pRow->artwork_id();
                                    // Row offset 0x5a. The schema leaves it
                                    // unnamed -- hence _unnamed29, the 30th
                                    // field in the seq -- and documents it as
                                    // flesniak's "always 1?", which holds only
                                    // if every track is an MP3. It is the
                                    // container: mp3 1, m4a 4, flac 5, wav 11,
                                    // aiff 12, across 651 rows with no
                                    // exceptions (F34). Announcing the wrong one
                                    // makes a deck fetch a file, try to decode
                                    // an AAC as an MP3, and give up.
                                    track.fileType = pRow->_unnamed29();
                                    contents.tracks.append(track);
                                } break;
                                case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE: {
                                    auto* pRow = static_cast<
                                            rekordbox_pdb_t::playlist_tree_row_t*>(
                                            rowRef->body());
                                    PdbPlaylist playlist;
                                    playlist.id = pRow->id();
                                    playlist.parentId = pRow->parent_id();
                                    playlist.sortOrder = pRow->sort_order();
                                    playlist.name = textOf(pRow->name());
                                    playlist.isFolder = pRow->is_folder();
                                    contents.playlists.insert(playlist.id, playlist);
                                } break;
                                default:
                                    break;
                                }
                            }
                        }
                    }
                    if (pCurrent->index() == lastIndex) {
                        break;
                    }
                    pCurrent = pPage->next_page();
                }
            }
        }

        // Attach entries once every playlist row is known, since entries and
        // tree rows live in different tables and either can be read first.
        for (auto it = playlistEntries.constBegin(); it != playlistEntries.constEnd(); ++it) {
            auto playlist = contents.playlists.find(it.key());
            if (playlist == contents.playlists.end()) {
                continue;
            }
            // QMap iterates by key, so the curated order is preserved.
            for (const quint32 trackId : it.value()) {
                playlist->trackIds.append(trackId);
            }
        }

        contents.ok = true;
    } catch (const std::exception& e) {
        contents.error = QStringLiteral("could not parse export.pdb: %1")
                                 .arg(QString::fromUtf8(e.what()));
        kLogger.warning() << contents.error;
    } catch (...) {
        contents.error = QStringLiteral("could not parse export.pdb");
        kLogger.warning() << contents.error;
    }
    return contents;
}

} // namespace prolink
} // namespace mixxx
