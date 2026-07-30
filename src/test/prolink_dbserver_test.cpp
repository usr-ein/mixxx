#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include "network/prolink/dbserver/dbservermessage.h"

namespace {

using mixxx::prolink::MediaSlot;
using mixxx::prolink::dbserver::Field;
using mixxx::prolink::dbserver::makeDescriptor;
using mixxx::prolink::dbserver::MenuTarget;
using mixxx::prolink::dbserver::Message;
using mixxx::prolink::dbserver::MessageType;

QByteArray hex(const char* text) {
    return QByteArray::fromHex(QByteArray(text));
}

/// The vectors below were produced by the Python proof-of-concept
/// (`prolinks_poc.proto.dbserver`), which is itself validated against captured
/// hardware traffic: 1957 messages from two CDJ-2000NXS re-encode byte for byte
/// (F7). So a mismatch here means the port drifted, not that the format is in
/// doubt.

TEST(ProLinkDbServerTest, PreambleIsABareUInt32One) {
    EXPECT_EQ(hex("1100000001"), mixxx::prolink::dbserver::preamble());
}

TEST(ProLinkDbServerTest, PortQueryIsNineteenBytes) {
    const QByteArray query = mixxx::prolink::dbserver::portQuery();
    EXPECT_EQ(19, query.size());
    EXPECT_EQ(hex("0000000f52656d6f7465444253657276657200"), query);
}

TEST(ProLinkDbServerTest, IntroduceMatchesTheCapturedEncoding) {
    EXPECT_EQ(hex("11872349ae"          // magic
                  "11fffffffe"          // the setup transaction id
                  "100000"              // type Introduce
                  "0f01"                // one argument
                  "140000000c"          // the 12-byte argument-tag blob...
                  "060000000000000000000000" // ...UInt32, then padding
                  "1100000002"),        // our device number
            mixxx::prolink::dbserver::makeIntroduce(2).encode());
}

/// D << 24 | M << 16 | Sr << 8 | Tr, with the binary menu target that every
/// artwork, waveform and beat-grid request uses.
TEST(ProLinkDbServerTest, DescriptorPacksRequesterMenuSlotType) {
    EXPECT_EQ(0x02080301u,
            makeDescriptor(2, MediaSlot::Usb, MenuTarget::Binary));
    EXPECT_EQ(0x03080201u,
            makeDescriptor(3, MediaSlot::Sd, MenuTarget::Binary));
}

TEST(ProLinkDbServerTest, GetArtworkMatchesTheCapturedEncoding) {
    EXPECT_EQ(hex("11872349ae"
                  "1103800001"          // first ordinary transaction id
                  "102003"              // type GetArtwork
                  "0f02"                // two arguments
                  "140000000c"
                  "060600000000000000000000" // both UInt32
                  "1102080301"          // the descriptor
                  "1100001234"),        // the artwork id
            mixxx::prolink::dbserver::makeGetArtwork(0x03800001,
                    makeDescriptor(2, MediaSlot::Usb, MenuTarget::Binary),
                    0x1234)
                    .encode());
}

/// The reply envelope: [request type, 0, byte length, blob].
TEST(ProLinkDbServerTest, DecodesAnArtworkReply) {
    const QByteArray wire = hex(
            "11872349ae"
            "1103800001"
            "104002"                       // type Artwork
            "0f04"                         // four arguments
            "140000000c"
            "060606030000000000000000"     // three UInt32 then a Binary
            "1100002003"
            "1100000000"
            "1100000006"
            "1400000006000102030405");

    int offset = 0;
    Message message;
    ASSERT_EQ(Message::DecodeStatus::Ok, Message::decode(wire, &offset, &message));
    EXPECT_EQ(wire.size(), offset);
    EXPECT_EQ(static_cast<quint16>(MessageType::Artwork), message.type());
    EXPECT_EQ(0x03800001u, message.transactionId());
    EXPECT_EQ(hex("000102030405"), message.blob(3));
}

/// A track with no art. The zero-length blob is **omitted from the wire
/// entirely** — the preceding length argument is what says it is absent — so a
/// decoder that expects it there reads the next message's magic as a field and
/// desynchronises the whole stream.
TEST(ProLinkDbServerTest, AnOmittedEmptyBlobStillDecodesAsAnArgument) {
    const QByteArray wire = hex(
            "11872349ae"
            "1103800002"
            "104002"
            "0f04"
            "140000000c"
            "060606030000000000000000"
            "1100002003"
            "1100000000"
            "1100000000");                 // length 0, and no blob follows

    int offset = 0;
    Message message;
    ASSERT_EQ(Message::DecodeStatus::Ok, Message::decode(wire, &offset, &message));
    EXPECT_EQ(wire.size(), offset);
    EXPECT_EQ(4, message.args().size());
    EXPECT_TRUE(message.blob(3).isEmpty());
}

/// And the same rule going out: an empty binary argument is counted in the
/// header and in the tag blob, but not written.
TEST(ProLinkDbServerTest, AnEmptyBlobIsNotWrittenButIsStillCounted) {
    const Message message(0x03800002,
            MessageType::Artwork,
            {Field::u32(0x2003), Field::u32(0), Field::u32(0), Field::binary(QByteArray())});
    EXPECT_EQ(hex("11872349ae"
                  "1103800002"
                  "104002"
                  "0f04"
                  "140000000c"
                  "060606030000000000000000"
                  "1100002003"
                  "1100000000"
                  "1100000000"),
            message.encode());
}

TEST(ProLinkDbServerTest, IntroduceReplyCarriesThePeersDeviceNumber) {
    const QByteArray wire = hex(
            "11872349ae"
            "11fffffffe"
            "104000"                       // type Success
            "0f02"
            "140000000c"
            "060600000000000000000000"
            "1100000000"                   // echoes the request type
            "1100000003");                 // the *server's* player number

    int offset = 0;
    Message message;
    ASSERT_EQ(Message::DecodeStatus::Ok, Message::decode(wire, &offset, &message));
    EXPECT_EQ(static_cast<quint16>(MessageType::Success), message.type());
    EXPECT_EQ(3u, message.number(1));
}

/// Nothing frames a dbserver message but its own contents, so "is it complete?"
/// can only be answered by trying to parse it. Every truncation of a valid
/// message must come back Incomplete rather than Malformed: the first would wait
/// for more bytes, the second would drop the connection.
TEST(ProLinkDbServerTest, EveryTruncationIsIncompleteRatherThanMalformed) {
    const QByteArray whole =
            mixxx::prolink::dbserver::makeGetArtwork(0x03800001, 0x02080301, 7)
                    .encode();
    for (int length = 0; length < whole.size(); ++length) {
        int offset = 0;
        Message message;
        EXPECT_EQ(Message::DecodeStatus::Incomplete,
                Message::decode(whole.left(length), &offset, &message))
                << "at length " << length;
        EXPECT_EQ(0, offset) << "a failed decode must not consume anything";
    }
}

TEST(ProLinkDbServerTest, ABadMagicIsMalformed) {
    const QByteArray wire = hex("11deadbeef11fffffffe1000000f01140000000c"
                                "0600000000000000000000001100000002");
    int offset = 0;
    Message message;
    EXPECT_EQ(Message::DecodeStatus::Malformed,
            Message::decode(wire, &offset, &message));
}

/// A length field a decoder trusts is a decoder that can be made to allocate
/// four gigabytes by one bad packet.
TEST(ProLinkDbServerTest, AnAbsurdBlobLengthDoesNotAllocate) {
    const QByteArray wire = hex("11872349ae"
                                "1103800001"
                                "104002"
                                "0f01"
                                "140000000c"
                                "030000000000000000000000"
                                "14ffffffff");
    int offset = 0;
    Message message;
    const Message::DecodeStatus status = Message::decode(wire, &offset, &message);
    EXPECT_NE(Message::DecodeStatus::Ok, status);
    EXPECT_EQ(0, offset);
}

/// Two messages in one TCP read is routine, and the second must be found.
TEST(ProLinkDbServerTest, DecodesBackToBackMessages) {
    const QByteArray first =
            mixxx::prolink::dbserver::makeGetArtwork(1, 0x02080301, 11).encode();
    const QByteArray second =
            mixxx::prolink::dbserver::makeGetArtwork(2, 0x02080301, 22).encode();
    const QByteArray wire = first + second;

    int offset = 0;
    Message message;
    ASSERT_EQ(Message::DecodeStatus::Ok, Message::decode(wire, &offset, &message));
    EXPECT_EQ(1u, message.transactionId());
    EXPECT_EQ(first.size(), offset);
    ASSERT_EQ(Message::DecodeStatus::Ok, Message::decode(wire, &offset, &message));
    EXPECT_EQ(2u, message.transactionId());
    EXPECT_EQ(22u, message.number(1));
    EXPECT_EQ(wire.size(), offset);
}

/// UTF-16 **big**-endian, counted in characters including the trailing NUL --
/// the exact opposite of the little-endian, byte-counted strings the NFS side of
/// the same protocol uses. Confusing the two produces CJK-looking noise rather
/// than an obvious failure.
TEST(ProLinkDbServerTest, StringsAreUtf16BeCountedInCharactersWithTheNul) {
    const Message message(1, MessageType::Introduce, {Field::string(QStringLiteral("ab"))});
    const QByteArray wire = message.encode();
    // 3 characters announced for a 2-character string, then 6 bytes.
    EXPECT_TRUE(wire.endsWith(hex("2600000003006100620000")));

    int offset = 0;
    Message decoded;
    ASSERT_EQ(Message::DecodeStatus::Ok, Message::decode(wire, &offset, &decoded));
    ASSERT_EQ(1, decoded.args().size());
    EXPECT_EQ(QStringLiteral("ab"), decoded.args().at(0).text);
}

} // namespace
