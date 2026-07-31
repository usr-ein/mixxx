#include <gtest/gtest.h>

#include <QByteArray>

#include "network/prolink/server/prolinkanalysiswire.h"

namespace {

using namespace mixxx::prolink::server;

QByteArray hex(const char* text) {
    return QByteArray::fromHex(QByteArray(text));
}

void appendU32Be(QByteArray* pOut, quint32 value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        pOut->append(static_cast<char>((value >> shift) & 0xFF));
    }
}

/// Wrap real tag bytes in a minimal `PMAI` header.
QByteArray container(const QByteArray& tags) {
    QByteArray out(QByteArrayLiteral("PMAI"));
    appendU32Be(&out, 28);
    appendU32Be(&out, 28 + tags.size());
    out.append(16, '\0');
    out.append(tags);
    return out;
}

/// One tagged section: fourcc, header length, total length, extended header,
/// payload.
QByteArray tag(const char* fourcc,
        quint32 headerSize,
        const QByteArray& payload,
        const QByteArray& extra = QByteArray()) {
    QByteArray out(fourcc);
    appendU32Be(&out, headerSize);
    appendU32Be(&out, static_cast<quint32>(12 + extra.size() + payload.size()));
    out.append(extra);
    out.append(payload);
    return out;
}

/// Three real cues from track 0xc8, newest-first as rekordbox wrote them.
const char* kPcob =
        "50434f4200000018000000c00000000100000003ffffffff504350540000001c"
        "00000038000000030000000000010000ffffffff010003e800015d0bffffffff"
        "00000000000000000000000000000000504350540000001c0000003800000002"
        "0000000000010000ffffffff010003e80000f99effffffff0000000000000000"
        "0000000000000000504350540000001c00000038000000010000000000010000"
        "ffffffff010003e80000010fffffffff00000000000000000000000000000000";

/// The first three beats of that track's real grid: beat number, tempo x100,
/// time in ms, big-endian.
const char* kPqtzBeats = "000133910000010f00023391000002d5000333910000049c";
/// The tag's own extended header: unknown, the 0x80000 constant, beat count.
///
/// The real capture's header says 0x40e beats because the real tag carries all
/// 1038 of them; this fixture carries three, so it says three. The Python side
/// of these vectors gets away with the mismatch because it slices bytes and
/// never reads the count — the Kaitai parser this uses does read it, and a
/// header promising 1038 beats over a 24-byte body is a malformed tag, which is
/// exactly what it reports.
const char* kPqtzExtra = "000000000008000000000003";

quint32 readU32Le(const QByteArray& data, int offset) {
    return static_cast<quint32>(static_cast<quint8>(data.at(offset))) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 8) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 3))) << 24);
}

/// Golden tests for the ANLZ-to-dbserver conversions.
///
/// Every expected value here comes from `captures/S06-load-and-play` — a real
/// CDJ-2000NXS answering another one's load of track 0xc8 — paired with that
/// same track's `ANLZ0000.DAT`/`.EXT` from the stick that was in the serving
/// deck. Having both the input and the output of a real implementation is what
/// makes these confirmations rather than guesses, and it is the only reason we
/// know the wire format differs from the file format at all.
///
/// The same vectors are asserted by `prolinks-compat/tests/test_analysis_wire.py`
/// against the Python proof of concept, so the two implementations are held to
/// one set of captured bytes rather than to each other.

TEST(ProLinkAnalysisWireTest, CuePointsMatchARealReplyByteForByte) {
    QByteArray records;
    QByteArray times;
    int count = 0;
    analysiswire::cuePoints(container(hex(kPcob)), &records, &count, &times);

    EXPECT_EQ(3, count);
    // Sorted by time, though the file stores them 3, 2, 1.
    EXPECT_EQ(hex("0f010000ffffffff9ef90000ffffffff0b5d0100ffffffff"), times);
    EXPECT_EQ(hex("000101000000000000000000280000000000000000000000000000000000000000000000"
                  "000102000000000000000000712500000000000000000000000000000000000000000000"
                  "0001030000000000000000005b3400000000000000000000000000000000000000000000"),
            records);
    EXPECT_EQ(count * analysiswire::kCueEntrySize, records.size());
}

TEST(ProLinkAnalysisWireTest, BeatGridPrefixAndEntries) {
    const QByteArray grid = analysiswire::beatGrid(
            container(tag("PQTZ", 24, hex(kPqtzBeats), hex(kPqtzExtra))));

    ASSERT_EQ(20 + 3 * 16, grid.size());
    EXPECT_EQ(0x80000u, readU32Le(grid, 0));
    EXPECT_EQ(3u, readU32Le(grid, 4));
    EXPECT_EQ(48u, readU32Le(grid, 8));
    EXPECT_EQ(1u, readU32Le(grid, 12));
    // Must be non-zero and deck-shaped: with zero here the main waveform does
    // not draw on real hardware.
    EXPECT_GE(readU32Le(grid, 16), 0x06000000u);

    // The file's 8-byte big-endian entry becomes 8 little-endian bytes plus an
    // 0xff pad, for 16 -- confirmed against all 1038 entries of the capture.
    EXPECT_EQ(hex("010091330f010000").append(8, '\xff'), grid.mid(20, 16));
    EXPECT_EQ(hex("02009133d5020000").append(8, '\xff'), grid.mid(36, 16));
}

TEST(ProLinkAnalysisWireTest, VbrIndexByteSwapsEveryWord) {
    // The whole point: only one word in the real index is non-palindromic, and
    // it is reordered. Zeros hid the rule everywhere else.
    const QByteArray payload = QByteArray(1596, '\0').append(hex("0000000001597680"));
    const QByteArray out =
            analysiswire::vbrIndex(container(tag("PVBR", 16, payload, QByteArray(4, '\0'))));
    ASSERT_EQ(1604, out.size());
    EXPECT_EQ(hex("0000000080765901"), out.right(8));
}

TEST(ProLinkAnalysisWireTest, WaveformPreviewUnpacksHeightAndWhiteness) {
    // Each packed byte becomes (height, whiteness); the tiny waveform is
    // appended verbatim, which is what makes the reply 900 rather than 800.
    const QByteArray preview = hex("110ca2a28282a3a3");
    QByteArray tiny;
    for (int i = 0; i < 100; ++i) {
        tiny.append(static_cast<char>(i));
    }
    const QByteArray out = analysiswire::waveformPreview(
            container(tag("PWAV", 20, preview, QByteArray(8, '\0'))
                              .append(tag("PWV2", 20, tiny, QByteArray(8, '\0')))));

    EXPECT_EQ(hex("11000c00020502050204020403050305"), out.left(16));
    EXPECT_EQ(tiny, out.mid(preview.size() * 2));
    EXPECT_EQ(preview.size() * 2 + 100, out.size());
}

TEST(ProLinkAnalysisWireTest, WaveformDetailIsThePayloadVerbatimBehindAPrefix) {
    QByteArray payload;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int i = 0; i < 256; ++i) {
            payload.append(static_cast<char>(i));
        }
    }
    QByteArray extra;
    appendU32Be(&extra, 1);
    appendU32Be(&extra, static_cast<quint32>(payload.size()));
    appendU32Be(&extra, 0x960000);

    const QByteArray out =
            analysiswire::waveformDetail(container(tag("PWV3", 24, payload, extra)));
    ASSERT_EQ(20 + payload.size(), out.size());
    EXPECT_EQ(static_cast<quint32>(payload.size()), readU32Le(out, 0));
    EXPECT_EQ(1u, readU32Le(out, 4));
    EXPECT_EQ(static_cast<quint32>(payload.size()), readU32Le(out, 8));
    EXPECT_EQ(0x96u, readU32Le(out, 12));
    EXPECT_GE(readU32Le(out, 16), 0x06000000u);
    EXPECT_EQ(payload, out.mid(20));
}

TEST(ProLinkAnalysisWireTest, AMissingFileOrTagYieldsAnEmptyBlob) {
    // A track analysed by an older rekordbox lacks the newer tags, and the
    // protocol has a representation for an empty blob: a missing waveform must
    // cost the waveform, not the load.
    const QByteArray empty = container(QByteArray());
    for (const QByteArray& input : {QByteArray(), empty}) {
        EXPECT_TRUE(analysiswire::vbrIndex(input).isEmpty());
        EXPECT_TRUE(analysiswire::beatGrid(input).isEmpty());
        EXPECT_TRUE(analysiswire::waveformPreview(input).isEmpty());
        EXPECT_TRUE(analysiswire::waveformDetail(input).isEmpty());

        QByteArray records;
        QByteArray times;
        int count = -1;
        analysiswire::cuePoints(input, &records, &count, &times);
        EXPECT_EQ(0, count);
        EXPECT_TRUE(records.isEmpty());
        EXPECT_TRUE(times.isEmpty());
    }
}

TEST(ProLinkAnalysisWireTest, GarbageIsRejectedWithoutThrowing) {
    // This is a network input path in the serve direction: the bytes come off a
    // USB stick a stranger formatted. A truncated or hostile ANLZ must cost the
    // analysis blob and nothing else -- notably not an exception escaping onto
    // the network thread.
    const QByteArray truncated = container(hex(kPcob)).left(20);
    EXPECT_TRUE(analysiswire::beatGrid(truncated).isEmpty());
    EXPECT_TRUE(analysiswire::vbrIndex(QByteArray(64, '\xff')).isEmpty());
}

TEST(ProLinkAnalysisWireTest, TheOpaquePrefixWordIsNonZeroAndAdvances) {
    // A real deck's value is per-reply and monotonic, not a property of the
    // track: the two observed values are for the same track in the same load.
    // Sending zero was the last difference between our replies and a real one,
    // and with zero the main waveform does not draw.
    const quint32 first = analysiswire::prefixOpaque();
    EXPECT_NE(0u, first);
    EXPECT_GE(first, 0x06000000u);
    EXPECT_GE(analysiswire::prefixOpaque(), first);
}

} // namespace
