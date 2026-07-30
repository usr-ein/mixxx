#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include "network/prolink/prolinkdefs.h"

namespace mixxx {
namespace prolink {
namespace dbserver {

/// Present as a UInt32 at the head of every message.
constexpr quint32 kMagic = 0x872349AEu;

/// The transaction id reserved for `Introduce` and `Disconnect`.
constexpr quint32 kSetupTransactionId = 0xFFFFFFFEu;
/// Where a real player's ordinary transaction ids start. Nothing depends on the
/// value -- the server only echoes it -- but matching what hardware does keeps
/// our stream indistinguishable from a deck's in a capture.
constexpr quint32 kFirstTransactionId = 0x03800001u;

/// The 19-byte query that asks port 12523 where the dbserver is listening:
/// a big-endian length, the ASCII name, a NUL. The reply is two bytes.
constexpr char kPortQueryName[] = "RemoteDBServer";

/// The tag byte that precedes every value.
enum class FieldType : quint8 {
    UInt8 = 0x0F,
    UInt16 = 0x10,
    UInt32 = 0x11,
    Binary = 0x14,
    String = 0x26,
};

/// The *other* numbering, used only inside the header's 12-byte type blob.
///
/// Two independent numberings for the same five types is a wart of the protocol,
/// not of this code: the tag byte in front of a value and the tag byte in the
/// header's blob describing that same value do not agree, and a message whose
/// two disagree is rejected.
enum class ArgType : quint8 {
    String = 0x02,
    Binary = 0x03,
    UInt8 = 0x04,
    UInt16 = 0x05,
    UInt32 = 0x06,
};

/// Message types, in both directions: what we send as a client, and what a real
/// player sends us as a server.
enum class MessageType : quint16 {
    Introduce = 0x0000,
    /// Sent by a real CDJ after it has finished with a menu, 23 times in one
    /// browse session, reusing the transaction id of the `RenderMenu` it
    /// follows — and drawing **no reply at all**. Most likely "release that
    /// menu's state". Answering it risks desynchronising a client that is not
    /// listening for one.
    MenuClose = 0x0001,
    Disconnect = 0x0100,

    MenuRoot = 0x1000,
    MenuGenre = 0x1001,
    MenuArtist = 0x1002,
    MenuAlbum = 0x1003,
    MenuTrack = 0x1004,
    MenuLabel = 0x100A,
    MenuTime = 0x1010,
    MenuBitrate = 0x1011,
    MenuHistory = 0x1012,
    MenuKey = 0x1014,
    MenuPlaylist = 0x1105,
    MenuSearch = 0x1300,
    /// "Which sort orders does this menu offer?" The reply is the list a deck
    /// shows under SORT, and it is the same twelve whatever argument 2 names.
    MenuSort = 0x1400,

    GetMetadata = 0x2002,
    GetArtwork = 0x2003,
    GetWaveformPreview = 0x2004,
    GetTrackInfo = 0x2102,
    GetCuePoints = 0x2104,
    GetGenericMetadata = 0x2202,
    GetBeatGrid = 0x2204,
    /// Undocumented, and the likely gate on playback: a real deck answers with
    /// 1604 bytes, exactly the size of a `PVBR` payload — the MP3
    /// variable-bitrate seek index. Without a time-to-byte-offset table a player
    /// cannot seek in the file, which is what a load that resolves the path and
    /// then never issues a single READ looks like.
    GetVbrIndex = 0x2504,
    GetWaveformDetail = 0x2904,

    RenderMenu = 0x3000,
    /// Undocumented. A player sends it mid-load, between `GetTrackInfo` and the
    /// analysis fetches, and a real deck answers with a bare `Success` echoing
    /// the type.
    Unknown3100 = 0x3100,
    /// Undocumented, two arguments, sent once during playback. Like
    /// `Unknown3E03` it appears only when a player is browsing a *foreign*
    /// device, and no capture shows a real answer — so ours is modelled on the
    /// `0x3100` acknowledgement. **Guessed**, but erroring on an unknown request
    /// is what stopped browsing dead in F25.
    Unknown3D03 = 0x3D03,
    /// Undocumented, sent immediately after `Introduce` by a player browsing a
    /// foreign device — it does not appear between two CDJs. Answering it with
    /// an error makes the player fetch the root menu and then disconnect without
    /// drilling in, which is how F25 presented: every category listed, every one
    /// of them empty.
    Unknown3E03 = 0x3E03,

    Success = 0x4000,
    MenuHeader = 0x4001,
    Artwork = 0x4002,
    Error = 0x4003,
    MenuItem = 0x4101,
    MenuFooter = 0x4201,
    WaveformPreview = 0x4402,
    VbrIndex = 0x4502,
    BeatGrid = 0x4602,
    CuePoints = 0x4702,
    WaveformDetail = 0x4A02,
    /// The reply to `Unknown3E03`, observed as `[0x3e03, 0, our number, ""]`.
    Unknown4B02 = 0x4B02,
};

/// Menu-item kinds, carried in argument 7 of a `MenuItem`.
///
/// CDJ-3000s pack extra information into the two high bytes, so mask with
/// 0xffff before comparing.
enum class ItemType : quint32 {
    Path = 0x0000,
    Folder = 0x0001,
    Album = 0x0002,
    /// The title in a `GetMetadata` reply — but the **container** in a
    /// `GetTrackInfo` reply, where the label is empty and the id holds a
    /// rekordbox file type. The same byte means different things in the two
    /// replies (F35).
    TrackTitle = 0x0004,
    Genre = 0x0006,
    Artist = 0x0007,
    Playlist = 0x0008,
    Rating = 0x000A,
    Duration = 0x000B,
    Tempo = 0x000D,
    Label = 0x000E,
    Key = 0x000F,
    Bitrate = 0x0010,
    Year = 0x0011,
    Color = 0x0013,
    Comment = 0x0023,
    HistoryPlaylist = 0x0024,
    DateAdded = 0x002E,
    /// The sixth item of a `GetTrackInfo` reply. Observed as a constant 1 across
    /// MP3, AAC, WAV and AIFF in a real deck-to-deck load, so it is *not* the
    /// container — that is item 1. Meaning still unknown (F35).
    Unknown2F = 0x002F,
    /// Heads a filtered list when there is more than one entry: id 0xffffffff,
    /// label `ALL` wrapped like a category. Choosing it sends 0xffffffff as that
    /// level's filter, meaning "do not narrow here".
    All = 0x00A0,
    SortDefault = 0x00A1,
    SortAlphabet = 0x00A2,
    PlayCount = 0x002A,
    MenuGenre = 0x0080,
    MenuArtist = 0x0081,
    MenuAlbum = 0x0082,
    MenuTrack = 0x0083,
    MenuPlaylist = 0x0084,
    MenuBpm = 0x0085,
    MenuRating = 0x0086,
    MenuLabel = 0x0089,
    MenuKey = 0x008B,
    MenuDateAdded = 0x008C,
    MenuBitrate = 0x0093,
    MenuPlayCount = 0x0097,
};

/// Argument 1 of a track-list request.
///
/// Also selects the **second column** returned in each item, which is why
/// `Default` is not simply "unsorted": sort by BPM and the BPM appears beside
/// every title (F43).
enum class SortOrder : quint32 {
    Default = 0x00,
    Title = 0x01,
    Artist = 0x02,
    Album = 0x03,
    Bpm = 0x04,
    Rating = 0x05,
    Genre = 0x06,
    Comment = 0x07,
    Time = 0x08,
    Remixer = 0x09,
    Label = 0x0A,
    OriginalArtist = 0x0B,
    Key = 0x0C,
    Bitrate = 0x0D,
    PlayCount = 0x10,
    DateAdded = 0x11,
};

/// Byte `M` of a request descriptor: which part of the player's display the
/// answer is destined for. Binary loads -- artwork, waveforms, beat grids --
/// use their own value rather than a menu location.
enum class MenuTarget : quint8 {
    Main = 0x01,
    Binary = 0x08,
};

/// Byte `T` of a request descriptor.
enum class TrackType : quint8 {
    Rekordbox = 1,
};

/// Pack the `r:m:s:t` UInt32 that opens nearly every request:
/// `D << 24 | M << 16 | Sr << 8 | Tr`.
///
/// `D` is *our* device number, and the server validates it: it must be 1-4, must
/// belong to a device actually on the network, and must not be the player being
/// asked. That is why dbserver queries cannot use the safe observer number 7,
/// and why the caller has to borrow a number rather than invent one.
constexpr quint32 makeDescriptor(int requesterNumber,
        MediaSlot slot,
        MenuTarget menu = MenuTarget::Main,
        TrackType trackType = TrackType::Rekordbox) {
    return (static_cast<quint32>(requesterNumber & 0xFF) << 24) |
            (static_cast<quint32>(menu) << 16) |
            (static_cast<quint32>(slot) << 8) |
            static_cast<quint32>(trackType);
}

/// One tagged argument.
///
/// A tagged union written as a struct with all three payloads present: there are
/// at most twelve of these per message and they live for the length of one
/// request, so the few spare bytes buy a type that is trivially copyable and
/// impossible to read through the wrong member.
struct Field {
    FieldType type = FieldType::UInt32;
    quint32 number = 0;
    QByteArray blob;
    QString text;

    static Field u8(quint8 value);
    static Field u16(quint16 value);
    static Field u32(quint32 value);
    static Field binary(const QByteArray& value);
    static Field string(const QString& value);
};

/// One dbserver message: a fixed five-field header, then up to twelve arguments.
class Message {
  public:
    Message() = default;
    Message(quint32 transactionId, MessageType type, const QList<Field>& args)
            : m_transactionId(transactionId),
              m_type(static_cast<quint16>(type)),
              m_args(args) {
    }

    quint32 transactionId() const {
        return m_transactionId;
    }
    quint16 type() const {
        return m_type;
    }
    const QList<Field>& args() const {
        return m_args;
    }

    /// Argument *index* as a number, or *fallback* if it is absent or not one.
    quint32 number(int index, quint32 fallback = 0) const;
    /// Argument *index* as a blob, or empty if it is absent or not one.
    QByteArray blob(int index) const;

    QByteArray encode() const;

    enum class DecodeStatus {
        Ok,
        /// The buffer holds the start of a message but not all of it. The normal
        /// case when reading a TCP stream, and not an error: wait for more.
        Incomplete,
        /// Structurally wrong. The stream is desynchronised and cannot be
        /// resynchronised, because messages are framed by nothing but their own
        /// contents -- the only remedy is to drop the connection.
        Malformed,
    };

    /// Decode one message starting at `*pOffset`, advancing it past the message
    /// on success and leaving it untouched otherwise.
    ///
    /// The parse itself is the generated Kaitai reader from
    /// `prolinks-compat/ksy/prolink_dbserver.ksy` — the schema is the source of
    /// truth for the two tag numberings, the omitted-empty-blob rule and the
    /// length limits, and this only lifts its output into Qt types. Kaitai
    /// cannot generate C++ serializers, so encode() above stays hand-written and
    /// the unit tests hold it to vectors the schema also parses.
    static DecodeStatus decode(const QByteArray& buffer, int* pOffset, Message* pMessage);

    /// Human-readable for logs: `Artwork(tx=0x3800001) [0x2003, 0, <4211B>]`.
    QString toString() const;

  private:
    quint32 m_transactionId = 0;
    quint16 m_type = 0;
    QList<Field> m_args;
};

/// The five-byte connection preamble both peers send once, before any message:
/// a bare UInt32 field with value 1. It heads the byte stream in *both*
/// directions, so a decoder must step over it before looking for the first
/// magic.
QByteArray preamble();

/// The fixed 19-byte query for TCP 12523.
QByteArray portQuery();

Message makeIntroduce(int requesterNumber);
Message makeDisconnect();
Message makeGetArtwork(quint32 transactionId, quint32 descriptor, quint32 artworkId);

// -- the serve direction -------------------------------------------------------

/// Wrap a root-menu category label the way real hardware does.
///
/// Real players wrap these in U+FFFA (interlinear annotation anchor) and U+FFFB
/// (terminator) — `￺PLAYLIST￻`. Presumably a marker letting the player
/// substitute a localised string for a category it recognises. A bare label
/// renders perfectly well, and the deck then **declines to open the category**
/// (F26).
QString menuLabel(const QString& text);

/// Build a `0x4101` menu item — one row of a browse.
///
/// Always twelve arguments in a fixed order. Arguments 2 and 4 are the *byte*
/// lengths of the two labels, which is twice the character count plus the NUL,
/// and getting them wrong is one of the ways a player refuses to render a list.
///
/// **Argument 10 tracks argument 7.** Across all 1,700 menu items in the
/// reference captures the two are never independent: an item carrying
/// `flags = 0x01000000` also carries 0x100 here, and an item with zero flags has
/// zero here. Both are non-zero only on the two kinds of item that name a track.
/// Deriving it removes the chance of setting one and forgetting the other.
Message makeMenuItem(quint32 parentId,
        quint32 mainId,
        const QString& label1,
        const QString& label2 = QString(),
        quint32 itemType = static_cast<quint32>(ItemType::TrackTitle),
        quint32 artworkId = 0,
        quint32 playlistPosition = 0,
        quint32 flags = 0x01000000);

/// Overwrite a built item's transaction id. Items are built without one and
/// stamped with the render's, so a client can correlate a whole page.
void stampTransactionId(Message* pMessage, quint32 transactionId);

/// Overwrite argument *index* of a built item. Used for the playlist position,
/// which is only known once the list has been assembled.
void setArgument(Message* pMessage, int index, quint32 value);

} // namespace dbserver
} // namespace prolink
} // namespace mixxx
