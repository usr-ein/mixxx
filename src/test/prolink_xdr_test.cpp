#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include "network/prolink/rpc/xdrbuffer.h"

namespace {

using mixxx::prolink::XdrReader;
using mixxx::prolink::XdrWriter;

QByteArray hex(const char* text) {
    return QByteArray::fromHex(QByteArray(text));
}

TEST(ProLinkXdrTest, U32IsBigEndian) {
    XdrWriter writer;
    writer.u32(0x01020304);
    EXPECT_EQ(hex("01020304"), writer.data());
}

TEST(ProLinkXdrTest, OpaqueVarPadsToFourBytes) {
    XdrWriter writer;
    writer.opaqueVar(QByteArrayLiteral("abc"));
    // length 3, then "abc", then one pad byte.
    EXPECT_EQ(hex("00000003") + QByteArrayLiteral("abc") + hex("00"), writer.data());
    EXPECT_EQ(0, writer.size() % 4);
}

TEST(ProLinkXdrTest, OpaqueFixedPadsButHasNoLengthPrefix) {
    XdrWriter writer;
    writer.opaqueFixed(QByteArrayLiteral("ab"));
    EXPECT_EQ(QByteArrayLiteral("ab") + hex("0000"), writer.data());
}

/// The one non-standard detail of the whole file-access path.
///
/// Standard NFS/MOUNT would send "/C/" as 3 ASCII bytes. Pioneer sends it as
/// UTF-16LE with a prefix counting *bytes*, so 6. Getting this wrong is the
/// most likely cause of a mount that fails for no visible reason, so it is
/// pinned as literal bytes rather than by round-trip.
TEST(ProLinkXdrTest, MountPathIsUtf16LeCountedInBytes) {
    XdrWriter writer;
    writer.stringUtf16Le(QStringLiteral("/C/"));

    const QByteArray expected = hex("00000006") // 6 bytes, not 3 characters
            + hex("2f00")                       // '/'
            + hex("4300")                       // 'C'
            + hex("2f00")                       // '/'
            + hex("0000");                      // 6 bytes of payload pad to 8
    EXPECT_EQ(expected, writer.data());
    // 4-byte prefix plus 8 padded payload bytes. Asserted separately from the
    // literal above on purpose: when the two disagree, one of them is a typo,
    // and having both is what tells you which.
    EXPECT_EQ(12, writer.size());
}

TEST(ProLinkXdrTest, Utf16LeHasNoBomAndSurvivesNonAscii) {
    XdrWriter writer;
    // U+30AB U+3099 -- the decomposed form of a character that actually appears
    // on the author's own media, where the database stores NFC and the
    // filesystem reports NFD. Whatever we are handed must go out verbatim.
    writer.stringUtf16Le(QString::fromUtf8("ガ"));
    EXPECT_EQ(hex("00000004") + hex("ab30") + hex("9930"), writer.data());
}

TEST(ProLinkXdrTest, Utf16LeRoundTrips) {
    const QString original = QStringLiteral("PIONEER/rekordbox/export.pdb");
    XdrWriter writer;
    writer.stringUtf16Le(original);

    XdrReader reader(writer.data());
    EXPECT_EQ(original, reader.stringUtf16Le());
    EXPECT_TRUE(reader.isValid());
    EXPECT_TRUE(reader.atEnd());
}

TEST(ProLinkXdrTest, ReaderRejectsAnAbsurdLengthWithoutAllocating) {
    // The property under test is that this does not attempt a 4 GiB allocation.
    // We cannot assert on allocator behaviour directly, but the check happens
    // before any allocation, and a regression here would abort the test run
    // rather than fail it -- which is itself the signal.
    XdrReader reader(hex("ffffffff"));
    const QByteArray value = reader.opaqueVar();
    EXPECT_TRUE(value.isEmpty());
    EXPECT_FALSE(reader.isValid());
}

TEST(ProLinkXdrTest, ReaderRejectsALengthThatOverrunsTheDatagram) {
    // Prefix says 16 bytes; only 4 follow.
    XdrReader reader(hex("00000010") + hex("deadbeef"));
    EXPECT_TRUE(reader.opaqueVar().isEmpty());
    EXPECT_FALSE(reader.isValid());
}

TEST(ProLinkXdrTest, ReaderRejectsAFieldWhosePaddingRunsOffTheEnd) {
    // 3 bytes of data present, but the 4th padding byte is missing. Accepting
    // this would leave the cursor past the end for the next read to trip over.
    XdrReader reader(hex("00000003") + hex("616263"));
    EXPECT_TRUE(reader.opaqueVar().isEmpty());
    EXPECT_FALSE(reader.isValid());
}

TEST(ProLinkXdrTest, ReaderRejectsOddLengthUtf16) {
    // An odd byte count cannot be UTF-16. Silently truncating would return a
    // string that looks plausible and is wrong by a character.
    XdrReader reader(hex("00000003") + hex("2f004300"));
    EXPECT_TRUE(reader.stringUtf16Le().isEmpty());
    EXPECT_FALSE(reader.isValid());
}

TEST(ProLinkXdrTest, FailureIsStickyAndSubsequentReadsAreNoOps) {
    // So a caller can decode a whole structure and check validity once, rather
    // than testing every field -- which is what keeps half-checked decoders
    // from existing.
    XdrReader reader(hex("ffffffff"));
    reader.opaqueVar();
    ASSERT_FALSE(reader.isValid());

    EXPECT_EQ(0u, reader.u32());
    EXPECT_TRUE(reader.opaqueFixed(4).isEmpty());
    EXPECT_TRUE(reader.stringAscii().isEmpty());
    EXPECT_FALSE(reader.isValid());
}

TEST(ProLinkXdrTest, EmptyStringIsAPrefixAndNothingElse) {
    // AUTH_UNIX sends machine_name as an empty string, so this is a real case
    // on every single RPC call rather than an edge one.
    XdrWriter writer;
    writer.stringAscii(QString());
    EXPECT_EQ(hex("00000000"), writer.data());

    XdrReader reader(writer.data());
    EXPECT_TRUE(reader.stringAscii().isEmpty());
    EXPECT_TRUE(reader.isValid());
    EXPECT_TRUE(reader.atEnd());
}

TEST(ProLinkXdrTest, FilehandleIsAnUninterpretedThirtyTwoByteToken) {
    QByteArray handle;
    for (int i = 0; i < 32; ++i) {
        handle.append(static_cast<char>(i));
    }
    XdrWriter writer;
    writer.opaqueFixed(handle);
    // 32 is already a multiple of 4, so no padding and no prefix: exactly the
    // bytes we were given, which is the whole contract for a filehandle.
    EXPECT_EQ(handle, writer.data());
    EXPECT_EQ(32, writer.size());

    XdrReader reader(writer.data());
    EXPECT_EQ(handle, reader.opaqueFixed(32));
    EXPECT_TRUE(reader.isValid());
}

} // namespace
