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

/// Only the message types this client actually sends or expects.
enum class MessageType : quint16 {
    Introduce = 0x0000,
    Disconnect = 0x0100,
    GetArtwork = 0x2003,
    Success = 0x4000,
    Artwork = 0x4002,
    Error = 0x4003,
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

} // namespace dbserver
} // namespace prolink
} // namespace mixxx
