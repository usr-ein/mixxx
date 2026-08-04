#include "library/deck/previewwaveform.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QVector>

namespace {

using mixxx::deck::PreviewWaveform;
using mixxx::prolink::AnlzPreviewColumn;

/// One packed `PWAV` byte: `0b101_00110` is shade 5, height 6. Spelled out as a
/// literal because this is the one fact the monochrome path rests on, and it is
/// confirmed by what a real deck puts on the wire rather than by a schema.
constexpr char kPacked = static_cast<char>(0b1010'0110);

QByteArray pwav(char fill = kPacked) {
    return QByteArray(PreviewWaveform::kColumns, fill);
}

/// `PWV4`'s bytes are seven bits: no byte of any column exceeded 127 on any
/// track measured, and one track hit exactly 127 on all four.
constexpr int kColourMax = 127;

QVector<AnlzPreviewColumn> colourPreview(int count = 1200) {
    QVector<AnlzPreviewColumn> columns;
    for (int i = 0; i < count; ++i) {
        AnlzPreviewColumn column;
        column.envelope = static_cast<quint8>(kColourMax);
        column.bass = static_cast<quint8>(kColourMax);
        column.mid = 0;
        column.treble = 0;
        columns.append(column);
    }
    return columns;
}

TEST(PreviewWaveformTest, DefaultIsNull) {
    EXPECT_TRUE(PreviewWaveform().isNull());
    EXPECT_EQ(0, PreviewWaveform().at(0).height);
}

TEST(PreviewWaveformTest, PwavSplitsHeightFromShade) {
    const PreviewWaveform preview = PreviewWaveform::fromAnlz(pwav(), {});
    ASSERT_FALSE(preview.isNull());
    // Height 6 of 31, scaled to the 0..255 the panel draws from.
    EXPECT_EQ(6 * 255 / 31, preview.at(0).height);
    // Shade 5 of 7 takes the CDJ blue five sevenths of the way to white.
    EXPECT_GT(preview.at(0).red, 0x33);
    EXPECT_GT(preview.at(0).blue, 0xdd);
}

TEST(PreviewWaveformTest, AWrongLengthPwavIsRefused) {
    // Not stretched, not padded. A preview of the wrong length is a tag we
    // cannot read, and a plausible-but-wrong waveform is worse than none
    // because nothing on screen would say it is wrong.
    EXPECT_TRUE(PreviewWaveform::fromAnlz(QByteArray(399, kPacked), {}).isNull());
    EXPECT_TRUE(PreviewWaveform::fromAnlz(QByteArray(401, kPacked), {}).isNull());
    EXPECT_TRUE(PreviewWaveform::fromAnlz(QByteArray(), {}).isNull());
}

TEST(PreviewWaveformTest, TheWireAndTheFileDrawTheSamePicture) {
    // The two monochrome decoders are one picture by two routes, which is what
    // makes a stick and a CDJ agree (browser-prd.md 11.4). Split a packed
    // payload the way a player does and it has to come back the same.
    QByteArray blob;
    for (int column = 0; column < PreviewWaveform::kColumns; ++column) {
        const auto packed = static_cast<quint8>(kPacked);
        blob.append(static_cast<char>(packed & 0x1f));
        blob.append(static_cast<char>(packed >> 5));
    }
    blob.append(QByteArray(100, '\x01')); // the PWV2 the wire appends

    const PreviewWaveform fromFile = PreviewWaveform::fromAnlz(pwav(), {});
    const PreviewWaveform fromWire = PreviewWaveform::fromWire(blob);
    ASSERT_FALSE(fromWire.isNull());
    for (int column = 0; column < PreviewWaveform::kColumns; ++column) {
        ASSERT_EQ(fromFile.at(column).height, fromWire.at(column).height) << column;
        ASSERT_EQ(fromFile.at(column).red, fromWire.at(column).red) << column;
        ASSERT_EQ(fromFile.at(column).blue, fromWire.at(column).blue) << column;
    }
}

TEST(PreviewWaveformTest, ATruncatedWireBlobIsRefused) {
    EXPECT_TRUE(PreviewWaveform::fromWire(QByteArray(799, '\x01')).isNull());
}

TEST(PreviewWaveformTest, ColourIsSevenBitAndSaturates) {
    const PreviewWaveform preview =
            PreviewWaveform::fromAnlz(pwav(), colourPreview());
    ASSERT_FALSE(preview.isNull());
    // 127 is the ceiling, so a band at 127 is fully saturated. Scaling by 255
    // instead would draw every colour at half strength -- plausible, uniformly
    // wrong, and with nothing on screen to say so.
    EXPECT_EQ(255, preview.at(0).red);
    EXPECT_EQ(0, preview.at(0).green);
    EXPECT_EQ(0, preview.at(0).blue);
}

TEST(PreviewWaveformTest, TheHeightComesFromPwavAndNotFromTheColourEnvelope) {
    // The whole point of reading both files. PWV4's envelope is compressed --
    // it puts a whole track in the top fifth of the strip -- so the height is
    // PWAV's, which is the wider range and needs no resampling.
    QVector<AnlzPreviewColumn> columns = colourPreview();
    for (AnlzPreviewColumn& column : columns) {
        column.envelope = static_cast<quint8>(kColourMax); // "full height"
    }
    // PWAV says this column is quiet: height 6 of 31.
    const PreviewWaveform preview = PreviewWaveform::fromAnlz(pwav(), columns);
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(6 * 255 / 31, preview.at(0).height)
            << "the envelope byte must not win the argument";
}

TEST(PreviewWaveformTest, ColourIsTheMeanOfTheThreeItCovers) {
    // A hue, averaged. The loudest of three has no better claim to the colour
    // of the moment than its neighbours, and following it makes the strip
    // flicker between bands on every transient.
    QVector<AnlzPreviewColumn> columns = colourPreview();
    columns[0].mid = 0;
    columns[1].mid = static_cast<quint8>(kColourMax);
    columns[2].mid = 0;

    const PreviewWaveform preview = PreviewWaveform::fromAnlz(pwav(), columns);
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(255 / 3, preview.at(0).green);
}

TEST(PreviewWaveformTest, AShortColourPreviewFallsBackToMonochrome) {
    // Not a failure: an older rekordbox wrote no PWV4 at all, and a CDJ hands
    // over none. The strip then draws blue going white, as that tag is meant to.
    const PreviewWaveform preview =
            PreviewWaveform::fromAnlz(pwav(), colourPreview(1199));
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(6 * 255 / 31, preview.at(0).height);
    EXPECT_GT(preview.at(0).blue, preview.at(0).red) << "the CDJ blue";
}

TEST(PreviewWaveformTest, WithNoPwavTheEnvelopeIsBetterThanNothing) {
    const PreviewWaveform preview =
            PreviewWaveform::fromAnlz(QByteArray(), colourPreview());
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(255, preview.at(0).height);
}

TEST(PreviewWaveformTest, OutOfRangeColumnsAreZeroRatherThanACrash) {
    const PreviewWaveform preview = PreviewWaveform::fromAnlz(pwav(), {});
    EXPECT_EQ(0, preview.at(-1).height);
    EXPECT_EQ(0, preview.at(PreviewWaveform::kColumns).height);
    EXPECT_EQ(0, preview.at(PreviewWaveform::kColumns + 100).height);
}

} // namespace
