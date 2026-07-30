#include "network/prolink/server/prolinkdbserverd.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>
#include <functional>

#include "moc_prolinkdbserverd.cpp"
#include "network/prolink/server/prolinkanalysiswire.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkDbServer");

using namespace mixxx::prolink;
using namespace mixxx::prolink::server;
using dbserver::Field;
using dbserver::ItemType;
using dbserver::Message;
using dbserver::MessageType;
using dbserver::SortOrder;

/// The port a real player answers the "where is your dbserver?" question on.
constexpr quint16 kQueryPort = kDbServerQueryPort;

/// A filter of 0xffffffff is the ALL entry: do not narrow at this level.
constexpr quint32 kFilterAll = 0xFFFFFFFFu;

/// A track item's type is its second column's type in the high bytes over this.
constexpr quint32 kTrackItemSuffix = 0x04;

// -- musical keys ------------------------------------------------------------

/// Camelot / Open Key notation: a wheel position and a letter, e.g. `12A`.
const QRegularExpression& camelotPattern() {
    static const QRegularExpression pattern(
            QStringLiteral("^\\s*(\\d{1,2})\\s*([ABab])\\s*$"));
    return pattern;
}

/// Classical key names to their Camelot wheel position. rekordbox writes either
/// notation depending on a preference, and the harmonic menu has to work for
/// both — the reference capture used classical names throughout.
const QHash<QString, QPair<int, QChar>>& classicalKeys() {
    static const QHash<QString, QPair<int, QChar>> table = {
            {QStringLiteral("abm"), {1, 'A'}}, {QStringLiteral("g#m"), {1, 'A'}},
            {QStringLiteral("b"), {1, 'B'}},
            {QStringLiteral("ebm"), {2, 'A'}}, {QStringLiteral("d#m"), {2, 'A'}},
            {QStringLiteral("f#"), {2, 'B'}}, {QStringLiteral("gb"), {2, 'B'}},
            {QStringLiteral("bbm"), {3, 'A'}}, {QStringLiteral("a#m"), {3, 'A'}},
            {QStringLiteral("db"), {3, 'B'}}, {QStringLiteral("c#"), {3, 'B'}},
            {QStringLiteral("fm"), {4, 'A'}}, {QStringLiteral("ab"), {4, 'B'}},
            {QStringLiteral("g#"), {4, 'B'}},
            {QStringLiteral("cm"), {5, 'A'}}, {QStringLiteral("eb"), {5, 'B'}},
            {QStringLiteral("d#"), {5, 'B'}},
            {QStringLiteral("gm"), {6, 'A'}}, {QStringLiteral("bb"), {6, 'B'}},
            {QStringLiteral("a#"), {6, 'B'}},
            {QStringLiteral("dm"), {7, 'A'}}, {QStringLiteral("f"), {7, 'B'}},
            {QStringLiteral("am"), {8, 'A'}}, {QStringLiteral("c"), {8, 'B'}},
            {QStringLiteral("em"), {9, 'A'}}, {QStringLiteral("g"), {9, 'B'}},
            {QStringLiteral("bm"), {10, 'A'}}, {QStringLiteral("d"), {10, 'B'}},
            {QStringLiteral("f#m"), {11, 'A'}}, {QStringLiteral("gbm"), {11, 'A'}},
            {QStringLiteral("a"), {11, 'B'}},
            {QStringLiteral("dbm"), {12, 'A'}}, {QStringLiteral("c#m"), {12, 'A'}},
            {QStringLiteral("e"), {12, 'B'}},
    };
    return table;
}

/// A key's Camelot position, or (0, 0) if we cannot place it on the wheel.
///
/// Accepts either notation, so a library in classical names gets harmonic
/// matching too.
QPair<int, QChar> wheelPosition(const QString& key) {
    const auto match = camelotPattern().match(key);
    if (match.hasMatch()) {
        const int position = match.captured(1).toInt();
        if (position >= 1 && position <= 12) {
            return {position, match.captured(2).at(0).toUpper()};
        }
        return {0, QChar()};
    }
    return classicalKeys().value(key.trimmed().toLower(), {0, QChar()});
}

/// Sort key for a musical key, ordering Camelot notation **numerically**.
///
/// A CDJ sorts these as text, so a library using alphanumeric keys comes out
/// `1A 1B 10A 10B 11A 11B 12A 12B 2A 2B`: the wheel positions interleave and two
/// harmonically adjacent keys end up eleven screens apart. That is a bug in the
/// hardware, not a convention worth reproducing, and nothing obliges a server to
/// hand back the order a CDJ would have computed for itself. So Camelot keys
/// sort by (position, letter); anything else keeps alphabetical order and sorts
/// after, since mixing two notations in one ordering has no meaningful answer.
QString keySortValue(const QString& key) {
    const auto match = camelotPattern().match(key);
    if (match.hasMatch()) {
        return QStringLiteral("0%1%2")
                .arg(match.captured(1).toInt(), 2, 10, QLatin1Char('0'))
                .arg(match.captured(2).toUpper());
    }
    return QStringLiteral("1") + key.toLower();
}

/// How far from the selected key each harmonic tolerance reaches.
///
/// Read straight off a real reply for `Abm` (1A): level 0 offered `Abm`, level 1
/// `Abm, B` — the relative major at the same position — and level 2
/// `Abm, B, Dbm, Ebm`, adding the two adjacent positions in the same mode.
constexpr int kHarmonicLevels = 3;

QList<QPair<int, QChar>> harmonicSet(int position, QChar letter, int tolerance) {
    const QChar other = letter == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
    QList<QPair<int, QChar>> matches{{position, letter}};
    if (tolerance >= 1) {
        matches.append({position, other});
    }
    if (tolerance >= 2) {
        matches.append({(position - 2 + 12) % 12 + 1, letter});
        matches.append({position % 12 + 1, letter});
    }
    return matches;
}

// -- track ordering ----------------------------------------------------------

QString lower(const QString& text) {
    return text.toLower();
}

/// Order tracks by *sort*, leaving an unknown order untouched.
///
/// Every id here comes from a real SORT menu. Reordering by something we do not
/// understand would be worse than the library's own order, which is at least
/// predictable.
void sortTracks(QList<const PdbTrack*>* pTracks, quint32 sort) {
    const auto byTitle = [](const PdbTrack* a, const PdbTrack* b) {
        return lower(a->title) < lower(b->title);
    };
    switch (static_cast<SortOrder>(sort)) {
    case SortOrder::Title:
        std::sort(pTracks->begin(), pTracks->end(), byTitle);
        return;
    case SortOrder::Artist:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return lower(a->artist) != lower(b->artist) ? lower(a->artist) < lower(b->artist)
                                                        : byTitle(a, b);
        });
        return;
    case SortOrder::Album:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            if (lower(a->album) != lower(b->album)) {
                return lower(a->album) < lower(b->album);
            }
            if (a->trackNumber != b->trackNumber) {
                return a->trackNumber < b->trackNumber;
            }
            return byTitle(a, b);
        });
        return;
    case SortOrder::Bpm:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return a->tempoCentiBpm != b->tempoCentiBpm
                    ? a->tempoCentiBpm < b->tempoCentiBpm
                    : byTitle(a, b);
        });
        return;
    case SortOrder::Rating:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return a->rating != b->rating ? a->rating > b->rating : byTitle(a, b);
        });
        return;
    case SortOrder::Genre:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return lower(a->genre) != lower(b->genre) ? lower(a->genre) < lower(b->genre)
                                                      : byTitle(a, b);
        });
        return;
    case SortOrder::Label:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return lower(a->label) != lower(b->label) ? lower(a->label) < lower(b->label)
                                                      : byTitle(a, b);
        });
        return;
    case SortOrder::Key:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            const QString ka = keySortValue(a->key);
            const QString kb = keySortValue(b->key);
            return ka != kb ? ka < kb : byTitle(a, b);
        });
        return;
    case SortOrder::Bitrate:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return a->bitrate != b->bitrate ? a->bitrate < b->bitrate : byTitle(a, b);
        });
        return;
    case SortOrder::DateAdded:
        std::sort(pTracks->begin(), pTracks->end(), [&](const PdbTrack* a, const PdbTrack* b) {
            return a->dateAdded != b->dateAdded ? a->dateAdded < b->dateAdded
                                                : byTitle(a, b);
        });
        return;
    case SortOrder::PlayCount:
        std::sort(pTracks->begin(), pTracks->end(), byTitle);
        return;
    default:
        // Including Default, which means the library's own artist-then-title
        // order -- already applied -- and is why it is not simply "unsorted".
        return;
    }
}

/// What each sort order puts in a track item's **second column**.
///
/// Read off a real server. Three things follow, and all three are load-bearing:
///
/// * the item type is `(column type << 8) | 0x04`, so the familiar `0x0704` is
///   not "title and artist" — it is "a track whose second column is the ARTIST
///   field", and `0x0d04` is the same item with a BPM column;
/// * numeric columns send an **empty** label and put the number in argument 0;
///   the deck formats it. Text columns send both;
/// * argument 0 for a text column is the *referenced row's* id.
///
/// This is what makes sorting useful rather than cosmetic: sort by BPM and the
/// BPM appears beside every title (F43).
struct SortColumn {
    ItemType type = ItemType::Artist;
    /// Which text field to send, or empty for a numeric column.
    const char* textField = "artist";
    const char* valueField = "artist_id";
};

SortColumn sortColumnFor(quint32 sort) {
    switch (static_cast<SortOrder>(sort)) {
    case SortOrder::Album:
        return {ItemType::Album, "album", "album_id"};
    case SortOrder::Bpm:
        return {ItemType::Tempo, nullptr, "bpm"};
    case SortOrder::Rating:
        return {ItemType::Rating, nullptr, "rating"};
    case SortOrder::Genre:
        return {ItemType::Genre, "genre", "genre_id"};
    case SortOrder::Label:
        return {ItemType::Label, "label", "label_id"};
    case SortOrder::Key:
        return {ItemType::Key, "key", "key_id"};
    case SortOrder::Bitrate:
        return {ItemType::Bitrate, nullptr, "bitrate"};
    case SortOrder::PlayCount:
        return {ItemType::PlayCount, nullptr, "play_count"};
    case SortOrder::DateAdded:
        return {ItemType::DateAdded, "date_added", "id"};
    default:
        // Default, Title and Artist all show the artist.
        return {ItemType::Artist, "artist", "artist_id"};
    }
}

QString columnText(const PdbTrack& track, const char* field) {
    if (!field) {
        return QString();
    }
    const QLatin1String name(field);
    if (name == QLatin1String("artist")) {
        return track.artist;
    }
    if (name == QLatin1String("album")) {
        return track.album;
    }
    if (name == QLatin1String("genre")) {
        return track.genre;
    }
    if (name == QLatin1String("label")) {
        return track.label;
    }
    if (name == QLatin1String("key")) {
        return track.key;
    }
    if (name == QLatin1String("date_added")) {
        return track.dateAdded;
    }
    return QString();
}

quint32 columnValue(const PdbTrack& track, const char* field) {
    const QLatin1String name(field);
    if (name == QLatin1String("artist_id")) {
        return track.artistId;
    }
    if (name == QLatin1String("album_id")) {
        return track.albumId;
    }
    if (name == QLatin1String("genre_id")) {
        return track.genreId;
    }
    if (name == QLatin1String("label_id")) {
        return track.labelId;
    }
    if (name == QLatin1String("key_id")) {
        return track.keyId;
    }
    if (name == QLatin1String("bpm")) {
        return track.tempoCentiBpm;
    }
    if (name == QLatin1String("rating")) {
        return track.rating;
    }
    if (name == QLatin1String("bitrate")) {
        return track.bitrate;
    }
    if (name == QLatin1String("play_count")) {
        return track.playCount;
    }
    if (name == QLatin1String("id")) {
        return track.id;
    }
    return 0;
}

QList<Message> trackItems(const QList<const PdbTrack*>& tracks, quint32 sort) {
    const SortColumn column = sortColumnFor(sort);
    const quint32 itemType =
            (static_cast<quint32>(column.type) << 8) | kTrackItemSuffix;
    QList<Message> items;
    items.reserve(tracks.size());
    for (const PdbTrack* pTrack : tracks) {
        items.append(dbserver::makeMenuItem(columnValue(*pTrack, column.valueField),
                pTrack->id,
                pTrack->title,
                columnText(*pTrack, column.textField),
                itemType,
                pTrack->artworkId));
    }
    return items;
}

/// Named rows as menu items, alphabetically unless *keyOrder* says otherwise.
QList<Message> itemsByName(const QHash<quint32, QString>& mapping,
        ItemType itemType,
        bool keyOrder = false) {
    QList<QPair<quint32, QString>> rows;
    rows.reserve(mapping.size());
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            rows.append({it.key(), it.value()});
        }
    }
    std::sort(rows.begin(),
            rows.end(),
            [keyOrder](const QPair<quint32, QString>& a, const QPair<quint32, QString>& b) {
                return keyOrder ? keySortValue(a.second) < keySortValue(b.second)
                                : a.second.toLower() < b.second.toLower();
            });
    QList<Message> items;
    items.reserve(rows.size());
    for (const auto& row : rows) {
        items.append(dbserver::makeMenuItem(
                0, row.first, row.second, QString(), static_cast<quint32>(itemType)));
    }
    return items;
}

/// The `ALL` entry, byte for byte as a real player sends it.
Message allItem() {
    return dbserver::makeMenuItem(0,
            kFilterAll,
            dbserver::menuLabel(QStringLiteral("ALL")),
            QString(),
            static_cast<quint32>(ItemType::All),
            0,
            0,
            0);
}

/// Root-menu categories: `(id, item type, label)`, **read off a real player**
/// rather than derived.
///
/// Two derivations were tried and both had exceptions. F26 computed the id from
/// the menu *request* type's low byte, which gave KEY the id BITRATE uses — so a
/// deck opening our KEY category asked for bitrates. F40 replaced that with
/// `item type - 0x7f`, right for eleven of the twelve and wrong for DATE ADDED,
/// which is 0x1b/0x8c. Now that all twelve have been observed there is nothing
/// left to derive, so they are listed.
///
/// We serve the subset we can actually answer. FOLDER is omitted: it browses
/// unanalysed files by directory, using a descriptor with track type 2, and we
/// have no such browse — and an unimplemented category is indistinguishable from
/// an empty one on the deck's screen (F40).
struct RootCategory {
    quint32 id;
    quint32 itemType;
    const char* label;
};
constexpr RootCategory kRootCategories[] = {
        {0x01, 0x80, "GENRE"},
        {0x02, 0x81, "ARTIST"},
        {0x03, 0x82, "ALBUM"},
        {0x04, 0x83, "TRACK"},
        {0x05, 0x84, "PLAYLIST"},
        {0x0A, 0x89, "LABEL"},
        {0x0C, 0x8B, "KEY"},
        {0x12, 0x91, "SEARCH"},
        {0x14, 0x93, "BITRATE"},
        {0x16, 0x95, "HISTORY"},
        {0x1B, 0x8C, "DATE ADDED"},
};

/// Sort orders a real player offers, in the order it lists them, with the item
/// type each carries. Read off a real SORT menu.
struct SortOption {
    SortOrder value;
    quint32 itemType;
    const char* label;
};
constexpr SortOption kSortOptions[] = {
        {SortOrder::Default, static_cast<quint32>(ItemType::SortDefault), "DEFAULT"},
        {SortOrder::Title, static_cast<quint32>(ItemType::SortAlphabet), "ALPHABET"},
        {SortOrder::Artist, static_cast<quint32>(ItemType::MenuArtist), "ARTIST"},
        {SortOrder::Album, static_cast<quint32>(ItemType::MenuAlbum), "ALBUM"},
        {SortOrder::Bpm, static_cast<quint32>(ItemType::MenuBpm), "BPM"},
        {SortOrder::Rating, static_cast<quint32>(ItemType::MenuRating), "RATING"},
        {SortOrder::Key, static_cast<quint32>(ItemType::MenuKey), "KEY"},
        {SortOrder::Bitrate, static_cast<quint32>(ItemType::MenuBitrate), "BITRATE"},
        {SortOrder::PlayCount, static_cast<quint32>(ItemType::MenuPlayCount), "DJ PLAY COUNT"},
        {SortOrder::Genre, static_cast<quint32>(ItemType::MenuGenre), "GENRE"},
        {SortOrder::DateAdded, static_cast<quint32>(ItemType::MenuDateAdded), "DATE ADDED"},
        {SortOrder::Label, static_cast<quint32>(ItemType::MenuLabel), "LABEL"},
};

/// Which filter field each drill level narrows by, per category.
///
/// A real player addresses every drill-down with one systematic request type,
/// `0x1000 | depth << 8 | category`, and each level adds one filter id to the
/// arguments. So this is a **grid**, not a handful of special cases — and
/// implementing it as a grid is what makes LABEL, BITRATE and KEY work for free
/// alongside GENRE, ARTIST and ALBUM (F42).
///
/// The chains are read off a real session: GENRE narrows to an artist, then an
/// album, then tracks; ARTIST skips straight to albums. The last entry in each
/// chain is the level that yields tracks.
enum class Filter { Genre, Artist, Album, Label, Key, Bitrate };

QList<Filter> drillChain(quint32 category) {
    switch (category) {
    case static_cast<quint32>(MessageType::MenuGenre) & 0xFF:
        return {Filter::Genre, Filter::Artist, Filter::Album};
    case static_cast<quint32>(MessageType::MenuArtist) & 0xFF:
        return {Filter::Artist, Filter::Album};
    case static_cast<quint32>(MessageType::MenuAlbum) & 0xFF:
        return {Filter::Album};
    case static_cast<quint32>(MessageType::MenuLabel) & 0xFF:
        return {Filter::Label, Filter::Artist, Filter::Album};
    case static_cast<quint32>(MessageType::MenuBitrate) & 0xFF:
        return {Filter::Bitrate};
    case static_cast<quint32>(MessageType::MenuKey) & 0xFF:
        return {Filter::Key};
    default:
        return {};
    }
}

quint32 filterValueOf(const PdbTrack& track, Filter filter) {
    switch (filter) {
    case Filter::Genre:
        return track.genreId;
    case Filter::Artist:
        return track.artistId;
    case Filter::Album:
        return track.albumId;
    case Filter::Label:
        return track.labelId;
    case Filter::Key:
        return track.keyId;
    case Filter::Bitrate:
        return track.bitrate;
    }
    return 0;
}

ItemType itemTypeOf(Filter filter) {
    switch (filter) {
    case Filter::Genre:
        return ItemType::Genre;
    case Filter::Artist:
        return ItemType::Artist;
    case Filter::Album:
        return ItemType::Album;
    case Filter::Label:
        return ItemType::Label;
    case Filter::Key:
        return ItemType::Key;
    case Filter::Bitrate:
        return ItemType::Bitrate;
    }
    return ItemType::Artist;
}

/// The names table a filter draws its labels from, or nullptr for BITRATE, whose
/// value *is* its label and which the deck formats itself.
const QHash<quint32, QString>* namesFor(const PdbContents& library, Filter filter) {
    switch (filter) {
    case Filter::Genre:
        return &library.genres;
    case Filter::Artist:
        return &library.artists;
    case Filter::Album:
        return &library.albums;
    case Filter::Label:
        return &library.labels;
    case Filter::Key:
        return &library.keys;
    case Filter::Bitrate:
        return nullptr;
    }
    return nullptr;
}

/// The envelope every binary reply shares:
/// `[request type, 0, byte length, blob, *trailing]`.
///
/// Argument 0 echoes the **request's message type**, not the track id. Every
/// reply a real player sends does; ours did not, and it cost the load.
///
/// A zero-length binary argument is omitted from the wire entirely, so "no data"
/// and "here is the data" share one shape and a missing tag needs no special
/// case.
Message binaryReply(quint32 transactionId,
        MessageType responseType,
        quint32 requestType,
        const QByteArray& blob,
        const QList<quint32>& trailing = {}) {
    QList<Field> args{Field::u32(requestType),
            Field::u32(0),
            Field::u32(static_cast<quint32>(blob.size())),
            Field::binary(blob)};
    for (const quint32 value : trailing) {
        args.append(Field::u32(value));
    }
    return Message(transactionId, responseType, args);
}

QString typeName(quint16 type) {
    return QStringLiteral("0x%1").arg(type, 4, 16, QLatin1Char('0'));
}

} // namespace

namespace mixxx {
namespace prolink {
namespace server {

// -- connection ----------------------------------------------------------------

ProLinkDbConnection::ProLinkDbConnection(ProLinkDbServer* pServer, QTcpSocket* pSocket)
        : QObject(pServer), m_pServer(pServer), m_pSocket(pSocket) {
    m_pSocket->setParent(this);
    connect(m_pSocket, &QTcpSocket::readyRead, this, &ProLinkDbConnection::onReadyRead);
    connect(m_pSocket,
            &QTcpSocket::disconnected,
            this,
            &ProLinkDbConnection::onDisconnected);
}

void ProLinkDbConnection::onDisconnected() {
    kLogger.info() << "dbserver client" << m_pSocket->peerAddress().toString()
                   << "disconnected";
    deleteLater();
}

void ProLinkDbConnection::send(const Message& reply) {
    const QByteArray encoded = reply.encode();
    m_pSocket->write(encoded);
}

void ProLinkDbConnection::onReadyRead() {
    m_buffer.append(m_pSocket->readAll());

    // The five-byte preamble is echoed verbatim before any message is
    // exchanged, in both directions.
    if (!m_preambleDone) {
        const QByteArray expected = dbserver::preamble();
        if (m_buffer.size() < expected.size()) {
            return;
        }
        m_buffer.remove(0, expected.size());
        m_preambleDone = true;
        m_pSocket->write(expected);
    }

    int offset = 0;
    while (offset < m_buffer.size()) {
        Message request;
        // The parse is the generated Kaitai reader, which is where the two tag
        // numberings, the omitted-empty-blob rule and the length caps live.
        const Message::DecodeStatus status =
                Message::decode(m_buffer, &offset, &request);
        if (status == Message::DecodeStatus::Incomplete) {
            break;
        }
        if (status == Message::DecodeStatus::Malformed) {
            // Messages are framed by nothing but their own contents, so a
            // desynchronised stream cannot be resynchronised. Dropping the
            // connection is the only remedy; the deck reconnects.
            kLogger.warning() << "malformed dbserver message from"
                              << m_pSocket->peerAddress().toString()
                              << "- dropping the connection";
            m_pSocket->disconnectFromHost();
            return;
        }
        handle(request);
    }
    m_buffer.remove(0, offset);
}

void ProLinkDbConnection::render(const Message& request) {
    const quint32 descriptor = request.number(0);
    const int offset = static_cast<int>(request.number(1));
    const int limit = static_cast<int>(request.number(2));
    const int total = static_cast<int>(request.number(4));

    // The client names the set it is paging by echoing the descriptor and the
    // size. Falling back to the most recent menu keeps a client that pages
    // something we never established from getting an empty page.
    QList<Message> items = m_menus.value({descriptor, total});
    if (items.isEmpty()) {
        items = m_lastMenu;
    }

    send(Message(request.transactionId(),
            MessageType::MenuHeader,
            {Field::u32(1), Field::u32(0)}));
    for (int i = offset; i < offset + limit && i < items.size(); ++i) {
        if (i < 0) {
            continue;
        }
        Message item = items.at(i);
        dbserver::stampTransactionId(&item, request.transactionId());
        send(item);
    }
    send(Message(request.transactionId(), MessageType::MenuFooter, {}));
}

void ProLinkDbConnection::handle(const Message& request) {
    const quint32 transaction = request.transactionId();
    const auto type = static_cast<MessageType>(request.type());
    const quint32 descriptor = request.number(0);
    const ProLinkServedMedium* pMedium = m_pServer->mediumFor(descriptor);
    m_pServer->countRequest(typeName(request.type()));

    switch (type) {
    case MessageType::Introduce:
        m_clientDeviceNumber = static_cast<int>(request.number(0));
        kLogger.info() << "client" << m_pSocket->peerAddress().toString()
                       << "introduced itself as device" << m_clientDeviceNumber;
        // Argument 1 is *our* player number here, not an item count.
        send(Message(transaction,
                MessageType::Success,
                {Field::u32(static_cast<quint32>(type)),
                        Field::u32(static_cast<quint32>(m_pServer->deviceNumber()))}));
        return;

    case MessageType::Disconnect:
        m_pSocket->disconnectFromHost();
        return;

    case MessageType::MenuClose:
        // Fire and forget: a real player sends this after finishing with a menu
        // and expects nothing back. Replying at all -- let alone with the
        // 0x4003 an unhandled type would produce -- risks desynchronising a
        // client that is not listening for one.
        //
        // It deliberately does **not** discard the result sets. "Release that
        // menu" was an inference from its position in the stream, and acting on
        // it broke pagination: a deck sends this while still scrolling the list
        // it is supposedly finished with.
        return;

    case MessageType::Unknown3E03:
        // Modelled on a real reply captured between two players. Its meaning is
        // unknown; erroring on it is what stopped a deck browsing past our root
        // menu (F25).
        send(Message(transaction,
                MessageType::Unknown4B02,
                {Field::u32(static_cast<quint32>(type)),
                        Field::u32(0),
                        Field::u32(static_cast<quint32>(m_pServer->deviceNumber())),
                        Field::string(QString())}));
        return;

    case MessageType::Unknown3100:
    case MessageType::Unknown3D03:
        // For 0x3100 this is what a real deck sends, verbatim. For 0x3d03 it is
        // a guess -- no capture contains a reply -- but it was the last request
        // we still answered with an error, and F25 is the precedent for what
        // erroring on an unknown request costs.
        send(Message(transaction,
                MessageType::Success,
                {Field::u32(static_cast<quint32>(type)), Field::u32(0)}));
        return;

    case MessageType::GetArtwork: {
        const QByteArray image =
                pMedium ? pMedium->artwork(request.number(1)) : QByteArray();
        send(binaryReply(transaction,
                MessageType::Artwork,
                static_cast<quint32>(type),
                image));
        return;
    }

    case MessageType::GetCuePoints: {
        QByteArray dat;
        if (pMedium) {
            pMedium->analysisFiles(request.number(1), &dat, nullptr);
        }
        QByteArray records;
        QByteArray times;
        int count = 0;
        analysiswire::cuePoints(dat, &records, &count, &times);
        // The one reply carrying two blobs: fixed-size cue records, then a
        // (time, loop time) pair per cue.
        send(Message(transaction,
                MessageType::CuePoints,
                {Field::u32(static_cast<quint32>(type)),
                        Field::u32(0),
                        Field::u32(static_cast<quint32>(records.size())),
                        Field::binary(records),
                        Field::u32(analysiswire::kCueEntrySize),
                        Field::u32(static_cast<quint32>(count)),
                        Field::u32(0),
                        Field::u32(static_cast<quint32>(times.size())),
                        Field::binary(times)}));
        return;
    }

    case MessageType::GetWaveformPreview:
    case MessageType::GetBeatGrid:
    case MessageType::GetWaveformDetail:
    case MessageType::GetVbrIndex: {
        // **GetWaveformPreview does not carry the track id where its siblings
        // do**: its arguments are [descriptor, 3, track id, 0, ""], so the id is
        // at index 2. Reading index 1 asks for analysis of track 3, gets
        // nothing, and answers with an empty blob -- which is what happened.
        const int idIndex = type == MessageType::GetWaveformPreview ? 2 : 1;
        QByteArray dat;
        QByteArray ext;
        if (pMedium) {
            pMedium->analysisFiles(request.number(idIndex), &dat, &ext);
        }
        QByteArray blob;
        MessageType responseType = MessageType::Error;
        QList<quint32> trailing;
        switch (type) {
        case MessageType::GetWaveformPreview:
            responseType = MessageType::WaveformPreview;
            blob = analysiswire::waveformPreview(dat);
            break;
        case MessageType::GetBeatGrid:
            responseType = MessageType::BeatGrid;
            blob = analysiswire::beatGrid(dat);
            trailing = {0};
            break;
        case MessageType::GetWaveformDetail:
            responseType = MessageType::WaveformDetail;
            blob = analysiswire::waveformDetail(ext);
            break;
        default:
            responseType = MessageType::VbrIndex;
            blob = analysiswire::vbrIndex(dat);
            break;
        }
        send(binaryReply(
                transaction, responseType, static_cast<quint32>(type), blob, trailing));
        return;
    }

    case MessageType::RenderMenu:
        render(request);
        return;

    default:
        break;
    }

    bool handled = false;
    const QList<Message> items = m_pServer->buildMenu(request, pMedium, &handled);
    if (!handled) {
        kLogger.info() << "unsupported request" << typeName(request.type()) << "from"
                       << m_pSocket->peerAddress().toString();
        send(Message(transaction,
                MessageType::Error,
                {Field::u32(static_cast<quint32>(type)), Field::u32(0)}));
        return;
    }

    // Establish the result set, then answer with its size. The client follows up
    // with 0x3000 to page through it -- possibly interleaved with other menus,
    // hence keying rather than replacing.
    m_menus.insert({descriptor, items.size()}, items);
    m_lastMenu = items;
    send(Message(transaction,
            MessageType::Success,
            {Field::u32(static_cast<quint32>(type)),
                    Field::u32(static_cast<quint32>(items.size()))}));
}

// -- server --------------------------------------------------------------------

ProLinkDbServer::ProLinkDbServer(QObject* parent)
        : QObject(parent) {
}

ProLinkDbServer::~ProLinkDbServer() = default;

bool ProLinkDbServer::start() {
    if (m_pListener) {
        return m_port != 0;
    }
    m_pListener = new QTcpServer(this);
    m_pQueryListener = new QTcpServer(this);
    connect(m_pListener,
            &QTcpServer::newConnection,
            this,
            &ProLinkDbServer::onDbConnection);
    connect(m_pQueryListener,
            &QTcpServer::newConnection,
            this,
            &ProLinkDbServer::onQueryConnection);

    if (!m_pListener->listen(QHostAddress::AnyIPv4, kDbServerPort)) {
        // The port is nominally dynamic -- that is what 12523 exists to answer --
        // so an ephemeral one is a legitimate fallback rather than a failure.
        if (!m_pListener->listen(QHostAddress::AnyIPv4, 0)) {
            kLogger.warning() << "could not listen for dbserver connections:"
                              << m_pListener->errorString();
            return false;
        }
    }
    m_port = m_pListener->serverPort();

    if (!m_pQueryListener->listen(QHostAddress::AnyIPv4, kQueryPort)) {
        // Fatal in practice: the query port is fixed, and a deck that cannot ask
        // where our dbserver is will never connect to it.
        kLogger.warning() << "could not listen on TCP" << kQueryPort << ":"
                          << m_pQueryListener->errorString()
                          << "- players will not find our dbserver";
    }
    kLogger.info() << "dbserver listening on TCP" << m_port << "(port query on"
                   << kQueryPort << ")";
    return true;
}

void ProLinkDbServer::stop() {
    if (m_pListener) {
        m_pListener->close();
    }
    if (m_pQueryListener) {
        m_pQueryListener->close();
    }
    m_port = 0;
}

void ProLinkDbServer::onDbConnection() {
    while (QTcpSocket* pSocket = m_pListener->nextPendingConnection()) {
        kLogger.info() << "dbserver client connected from"
                       << pSocket->peerAddress().toString();
        new ProLinkDbConnection(this, pSocket);
    }
}

void ProLinkDbServer::onQueryConnection() {
    while (QTcpSocket* pSocket = m_pQueryListener->nextPendingConnection()) {
        // The fixed 19-byte query in, two big-endian bytes out. Short enough
        // that it always arrives in one segment, and the reply does not depend
        // on the contents anyway.
        connect(pSocket, &QTcpSocket::readyRead, pSocket, [this, pSocket]() {
            pSocket->readAll();
            QByteArray reply;
            reply.append(static_cast<char>((m_port >> 8) & 0xFF));
            reply.append(static_cast<char>(m_port & 0xFF));
            pSocket->write(reply);
            pSocket->disconnectFromHost();
            kLogger.info() << "told" << pSocket->peerAddress().toString()
                           << "our dbserver is on port" << m_port;
        });
        connect(pSocket, &QTcpSocket::disconnected, pSocket, &QTcpSocket::deleteLater);
    }
}

void ProLinkDbServer::setMedium(MediaSlot slot, const ProLinkServedMedium& medium) {
    m_media.insert(static_cast<int>(slot), medium);
}

void ProLinkDbServer::removeMedium(MediaSlot slot) {
    m_media.remove(static_cast<int>(slot));
}

const ProLinkServedMedium* ProLinkDbServer::mediumFor(quint32 descriptor) const {
    const int slot = static_cast<int>((descriptor >> 8) & 0xFF);
    const auto found = m_media.constFind(slot);
    if (found != m_media.constEnd()) {
        return &found.value();
    }
    // Fall back to the only medium we have, which keeps a single-slot server
    // answering requests that name any slot.
    return m_media.size() == 1 ? &m_media.constBegin().value() : nullptr;
}

QList<Message> ProLinkDbServer::buildMenu(const Message& request,
        const ProLinkServedMedium* pMedium,
        bool* pHandled) const {
    *pHandled = true;
    const auto type = static_cast<MessageType>(request.type());
    const quint32 sort = request.number(1);

    if (type == MessageType::MenuRoot) {
        QList<Message> items;
        for (const RootCategory& category : kRootCategories) {
            // Three details are copied from a real player rather than invented,
            // because a deck that renders our labels perfectly well will still
            // refuse to *open* a category if they are wrong (F26): argument 1
            // carries a per-category id and not zero; the label is wrapped in
            // U+FFFA/U+FFFB; and argument 7 is zero, not the 0x01000000 a track
            // item carries.
            items.append(dbserver::makeMenuItem(0,
                    category.id,
                    dbserver::menuLabel(QString::fromLatin1(category.label)),
                    QString(),
                    category.itemType,
                    0,
                    0,
                    0));
        }
        return items;
    }

    if (type == MessageType::MenuSort) {
        // Independent of which menu is being sorted: a real player sends the
        // same twelve whatever argument 2 names.
        QList<Message> items;
        for (const SortOption& option : kSortOptions) {
            items.append(dbserver::makeMenuItem(0,
                    static_cast<quint32>(option.value),
                    dbserver::menuLabel(QString::fromLatin1(option.label)),
                    QString(),
                    option.itemType,
                    0,
                    0,
                    0));
        }
        return items;
    }

    if (!pMedium || !pMedium->isValid()) {
        // A slot we do not serve. An empty list rather than an error: the deck
        // asked about a medium that is simply not there.
        return {};
    }
    const PdbContents& library = pMedium->library();

    switch (type) {
    case MessageType::MenuTrack: {
        QList<const PdbTrack*> tracks = pMedium->tracksInDefaultOrder();
        sortTracks(&tracks, sort);
        return trackItems(tracks, sort);
    }

    case MessageType::MenuArtist:
        return itemsByName(library.artists, ItemType::Artist);
    case MessageType::MenuAlbum:
        return itemsByName(library.albums, ItemType::Album);
    case MessageType::MenuGenre:
        return itemsByName(library.genres, ItemType::Genre);
    case MessageType::MenuLabel:
        return itemsByName(library.labels, ItemType::Label);
    case MessageType::MenuKey:
        // Wheel order, not alphabetical. See keySortValue.
        return itemsByName(library.keys, ItemType::Key, true);

    case MessageType::MenuHistory:
        // Empty on a medium that has never played anywhere, which is the normal
        // state for a stick written straight from rekordbox. The HISTORY table
        // is not walked by our pdb parser -- its rows do not carry valid row
        // references -- so an empty list is also the only honest answer.
        return {};

    case MessageType::MenuTime: {
        // Distinct date-added values, newest first.
        QStringList dates;
        for (const PdbTrack& track : library.tracks) {
            if (!track.dateAdded.isEmpty() && !dates.contains(track.dateAdded)) {
                dates.append(track.dateAdded);
            }
        }
        std::sort(dates.begin(), dates.end(), std::greater<QString>());
        QList<Message> items;
        quint32 index = 1;
        for (const QString& date : dates) {
            items.append(dbserver::makeMenuItem(0,
                    index++,
                    date,
                    QString(),
                    static_cast<quint32>(ItemType::DateAdded)));
        }
        return items;
    }

    case MessageType::MenuBitrate: {
        // A real server sends the value in the id and leaves both labels empty;
        // the deck formats the number itself.
        QList<quint32> rates;
        for (const PdbTrack& track : library.tracks) {
            if (track.bitrate && !rates.contains(track.bitrate)) {
                rates.append(track.bitrate);
            }
        }
        std::sort(rates.begin(), rates.end());
        QList<Message> items;
        for (const quint32 rate : rates) {
            items.append(dbserver::makeMenuItem(0,
                    rate,
                    QString(),
                    QString(),
                    static_cast<quint32>(ItemType::Bitrate)));
        }
        return items;
    }

    case MessageType::MenuPlaylist: {
        const quint32 playlistId = request.number(2);
        const bool isFolder = request.number(3) != 0;
        if (isFolder) {
            QList<PdbPlaylist> children;
            for (auto it = library.playlists.constBegin();
                    it != library.playlists.constEnd();
                    ++it) {
                const bool isRoot = !library.playlists.contains(it->parentId);
                if (playlistId == 0 ? isRoot : it->parentId == playlistId) {
                    children.append(it.value());
                }
            }
            std::sort(children.begin(),
                    children.end(),
                    [](const PdbPlaylist& a, const PdbPlaylist& b) {
                        return a.sortOrder != b.sortOrder ? a.sortOrder < b.sortOrder
                                                          : a.name < b.name;
                    });
            QList<Message> items;
            quint32 position = 1;
            for (const PdbPlaylist& playlist : children) {
                items.append(dbserver::makeMenuItem(0,
                        playlist.id,
                        playlist.name,
                        QString(),
                        static_cast<quint32>(playlist.isFolder ? ItemType::Folder
                                                               : ItemType::Playlist),
                        0,
                        position++));
            }
            return items;
        }

        QList<const PdbTrack*> tracks;
        const auto playlist = library.playlists.constFind(playlistId);
        if (playlist != library.playlists.constEnd()) {
            for (const quint32 trackId : playlist->trackIds) {
                if (const PdbTrack* pTrack = pMedium->track(trackId)) {
                    tracks.append(pTrack);
                }
            }
        }
        // A playlist honours the sort too -- most browsing happens inside one,
        // so ignoring it here is ignoring it almost everywhere. DEFAULT keeps
        // the curated order, which is the whole point of a playlist and must not
        // be replaced by an alphabetical one.
        if (static_cast<SortOrder>(sort) != SortOrder::Default) {
            sortTracks(&tracks, sort);
        }
        QList<Message> items = trackItems(tracks, sort);
        for (int i = 0; i < items.size(); ++i) {
            dbserver::setArgument(&items[i], 9, static_cast<quint32>(i + 1));
        }
        return items;
    }

    case MessageType::MenuSearch: {
        // Argument 2 is the term's UTF-16 byte length, argument 3 the text.
        // Reading 2 searches for a number and matches nothing.
        QString term;
        if (request.args().size() > 3) {
            term = request.args().at(3).text;
        }
        const QString needle = term.toLower();
        QList<const PdbTrack*> matches;
        for (const PdbTrack* pTrack : pMedium->tracksInDefaultOrder()) {
            if (pTrack->title.toLower().contains(needle) ||
                    pTrack->artist.toLower().contains(needle) ||
                    pTrack->album.toLower().contains(needle)) {
                matches.append(pTrack);
            }
        }
        sortTracks(&matches, sort);
        return trackItems(matches, sort);
    }

    case MessageType::GetMetadata:
    case MessageType::GetGenericMetadata: {
        // One track's metadata: **thirteen** items in a fixed order, modelled
        // field for field on a real deck's reply and then checked against that
        // track's own row in export.pdb.
        //
        // Three things ours originally got wrong, none of which showed up on
        // screen (F32): four items were missing -- colour, date added, bitrate
        // and label, and a player renders the nine it gets and looks perfectly
        // correct; the referenced ids were the track's own, where an artist item
        // must carry the *artist's* row id so the player can offer "more by this
        // artist"; and the title item carries the artwork id, which ours left
        // zero.
        //
        // Items go out unconditionally, including the empty ones: a real deck
        // sends `label` with id 0 and no text rather than omitting it, and the
        // count is what the client pages against.
        const PdbTrack* pTrack = pMedium->track(request.number(1));
        if (!pTrack) {
            return {};
        }
        const auto item = [](quint32 first,
                                  quint32 mainId,
                                  ItemType itemType,
                                  const QString& label = QString(),
                                  quint32 artworkId = 0,
                                  quint32 flags = 0) {
            return dbserver::makeMenuItem(first,
                    mainId,
                    label,
                    QString(),
                    static_cast<quint32>(itemType),
                    artworkId,
                    0,
                    flags);
        };
        // Argument 0 is 1 on eight of the thirteen and 0 on the rest. The split
        // does not line up with anything we can name -- not "has a label"
        // (comment has one and gets 0), not "has a browse menu" (tempo has one
        // and gets 0) -- so it is reproduced as observed rather than derived
        // from a rule we would be inventing.
        return {
                item(1, pTrack->id, ItemType::TrackTitle, pTrack->title,
                        pTrack->artworkId, 0x01000000),
                item(1, pTrack->artistId, ItemType::Artist, pTrack->artist),
                item(1, pTrack->albumId, ItemType::Album, pTrack->album),
                // Numeric fields carry their value in the id, not the label.
                item(0, pTrack->durationSeconds, ItemType::Duration),
                item(0, pTrack->tempoCentiBpm, ItemType::Tempo),
                item(0, pTrack->id, ItemType::Comment, pTrack->comment),
                item(1, pTrack->keyId, ItemType::Key, pTrack->key),
                item(0, pTrack->rating, ItemType::Rating),
                item(0, pTrack->colorId, ItemType::Color, pTrack->color),
                item(1, pTrack->genreId, ItemType::Genre, pTrack->genre),
                item(1, pTrack->id, ItemType::DateAdded, pTrack->dateAdded),
                item(1, pTrack->bitrate, ItemType::Bitrate),
                item(1, pTrack->labelId, ItemType::Label, pTrack->label),
        };
    }

    case MessageType::GetTrackInfo: {
        // **Six** items, of which the path is only one.
        //
        // The path alone is enough for a player to render the file name and walk
        // it over NFS, and it is what we sent at first. It is not enough for the
        // player to *load* the track: without the rest a deck sat at
        // NOW LOADING and then reported that it could not decode the format --
        // having never read a byte of the file, so the verdict came from this
        // reply and nowhere else.
        const PdbTrack* pTrack = pMedium->track(request.number(1));
        if (!pTrack) {
            return {};
        }
        const auto item = [](quint32 mainId,
                                  ItemType itemType,
                                  const QString& label = QString(),
                                  quint32 parent = 0) {
            return dbserver::makeMenuItem(parent,
                    mainId,
                    label,
                    QString(),
                    static_cast<quint32>(itemType),
                    0,
                    0,
                    0);
        };
        return {
                // Not the title: in *this* reply the 0x04 slot carries the
                // container, with an empty label, and a player takes it at face
                // value -- so a wrong value makes it fetch the file and then
                // fail to decode it. Settled by capturing one deck loading each
                // format from another's USB: 1 for MP3, 4 for AAC, 11 for WAV,
                // 12 for AIFF, while item 6 held 1 throughout (F35).
                item(pTrack->fileType, ItemType::TrackTitle),
                item(pTrack->durationSeconds, ItemType::Duration),
                item(pTrack->tempoCentiBpm, ItemType::Tempo),
                item(pTrack->id, ItemType::Comment, pTrack->comment),
                // Argument 0 is zero on every other menu item ever captured; on
                // this one it is the **file size in bytes**. That is how a
                // player learns how much there is to read, and it is the one
                // field a load needs that browsing does not -- which fits a deck
                // that renders the track perfectly and then cannot open it
                // (F31).
                item(pTrack->id, ItemType::Path, pTrack->filePath, pTrack->fileSize),
                item(1, ItemType::Unknown2F),
        };
    }

    default:
        break;
    }

    // The drill-down grid: 0x1000 | depth << 8 | category.
    const quint32 raw = static_cast<quint32>(request.type());
    const quint32 depth = (raw >> 8) & 0x0F;
    if ((raw & 0xF000) == 0x1000 && depth > 0) {
        const quint32 category = raw & 0xFF;
        QList<quint32> filters;
        for (quint32 i = 0; i < depth; ++i) {
            filters.append(request.number(static_cast<int>(2 + i)));
        }

        if (category == (static_cast<quint32>(MessageType::MenuKey) & 0xFF)) {
            // KEY drills to a **harmonic tolerance** first, then to tracks.
            // Choosing a key does not go straight to that key's tracks: a real
            // player offers three widening matches -- the key alone, plus its
            // relative, plus the adjacent wheel positions -- and only then lists
            // tracks. It is the harmonic-mixing feature, and it is why KEY has an
            // extra level no other category has (F44).
            const quint32 keyId = filters.isEmpty() ? 0 : filters.first();
            const QPair<int, QChar> wheel = wheelPosition(library.keys.value(keyId));
            if (wheel.first == 0) {
                // A key we cannot place on the wheel: an exact match, which is at
                // least correct, rather than inventing neighbours.
                QList<const PdbTrack*> tracks;
                for (const PdbTrack& track : library.tracks) {
                    if (track.keyId == keyId) {
                        tracks.append(&track);
                    }
                }
                sortTracks(&tracks, sort);
                return trackItems(tracks, sort);
            }

            QHash<QString, QPair<quint32, QString>> byWheel;
            for (auto it = library.keys.constBegin(); it != library.keys.constEnd(); ++it) {
                const QPair<int, QChar> spot = wheelPosition(it.value());
                if (spot.first != 0) {
                    byWheel.insert(
                            QStringLiteral("%1%2").arg(spot.first).arg(spot.second),
                            {it.key(), it.value()});
                }
            }
            const auto spotKey = [](const QPair<int, QChar>& spot) {
                return QStringLiteral("%1%2").arg(spot.first).arg(spot.second);
            };

            if (depth <= 1) {
                QList<Message> items;
                for (int tolerance = 0; tolerance < kHarmonicLevels; ++tolerance) {
                    QStringList names;
                    for (const auto& spot :
                            harmonicSet(wheel.first, wheel.second, tolerance)) {
                        const auto found = byWheel.constFind(spotKey(spot));
                        if (found != byWheel.constEnd()) {
                            names.append(found->second);
                        }
                    }
                    items.append(dbserver::makeMenuItem(
                            static_cast<quint32>(tolerance),
                            keyId,
                            names.join(QStringLiteral(", ")),
                            QString(),
                            static_cast<quint32>(ItemType::Key)));
                }
                return items;
            }

            const int tolerance =
                    filters.size() > 1 ? static_cast<int>(filters.at(1)) : 0;
            QSet<quint32> wanted;
            for (const auto& spot : harmonicSet(wheel.first, wheel.second, tolerance)) {
                const auto found = byWheel.constFind(spotKey(spot));
                if (found != byWheel.constEnd()) {
                    wanted.insert(found->first);
                }
            }
            QList<const PdbTrack*> tracks;
            for (const PdbTrack& track : library.tracks) {
                if (wanted.contains(track.keyId)) {
                    tracks.append(&track);
                }
            }
            sortTracks(&tracks, sort);
            return trackItems(tracks, sort);
        }

        const QList<Filter> chain = drillChain(category);
        if (!chain.isEmpty()) {
            QList<const PdbTrack*> tracks;
            for (const PdbTrack& track : library.tracks) {
                bool matches = true;
                for (int level = 0; level < chain.size() && level < filters.size(); ++level) {
                    if (filters.at(level) == kFilterAll) {
                        continue;
                    }
                    if (filterValueOf(track, chain.at(level)) != filters.at(level)) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    tracks.append(&track);
                }
            }

            if (static_cast<int>(depth) >= chain.size()) {
                sortTracks(&tracks, sort);
                return trackItems(tracks, sort);
            }

            const Filter next = chain.at(static_cast<int>(depth));
            const QHash<quint32, QString>* pNames = namesFor(library, next);
            QList<Message> items;
            if (pNames) {
                QHash<quint32, QString> present;
                for (const PdbTrack* pTrack : tracks) {
                    const quint32 value = filterValueOf(*pTrack, next);
                    if (value) {
                        present.insert(value, pNames->value(value));
                    }
                }
                items = itemsByName(present, itemTypeOf(next), next == Filter::Key);
            } else {
                QList<quint32> values;
                for (const PdbTrack* pTrack : tracks) {
                    const quint32 value = filterValueOf(*pTrack, next);
                    if (value && !values.contains(value)) {
                        values.append(value);
                    }
                }
                std::sort(values.begin(), values.end());
                for (const quint32 value : values) {
                    items.append(dbserver::makeMenuItem(0,
                            value,
                            QString(),
                            QString(),
                            static_cast<quint32>(itemTypeOf(next))));
                }
            }
            // A real reply heads the list with ALL, but only when there is a
            // choice to make: a single-entry level goes out bare.
            if (items.size() > 1) {
                items.prepend(allItem());
            }
            return items;
        }
    }

    *pHandled = false;
    return {};
}

} // namespace server
} // namespace prolink
} // namespace mixxx
