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
    const PreviewWaveform preview = PreviewWaveform::fromPwav(pwav());
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
    EXPECT_TRUE(PreviewWaveform::fromPwav(QByteArray(399, kPacked)).isNull());
    EXPECT_TRUE(PreviewWaveform::fromPwav(QByteArray(401, kPacked)).isNull());
    EXPECT_TRUE(PreviewWaveform::fromPwav(QByteArray()).isNull());
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

    const PreviewWaveform fromFile = PreviewWaveform::fromPwav(pwav());
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

TEST(PreviewWaveformTest, ColourPreviewIsSevenBitAndFillsTheStrip) {
    const PreviewWaveform preview =
            PreviewWaveform::fromColourPreview(colourPreview());
    ASSERT_FALSE(preview.isNull());
    // 127 is the ceiling, so the loudest column fills the strip. Scaling by 255
    // instead would draw every waveform at half height -- plausible, uniformly
    // wrong, and with nothing on screen to say so.
    EXPECT_EQ(255, preview.at(0).height);
    EXPECT_EQ(255, preview.at(0).red);
    EXPECT_EQ(0, preview.at(0).green);
    EXPECT_EQ(0, preview.at(0).blue);
}

TEST(PreviewWaveformTest, ColourPreviewKeepsTheLoudestOfEachThree) {
    // 1200 columns into 400. The peak rather than the mean, and the peak's own
    // colour: the bar drawn is the moment that set the height, not an average
    // of three moments that never happened.
    QVector<AnlzPreviewColumn> columns = colourPreview();
    // Make the middle of the first group the loud one, and a different colour.
    columns[0].envelope = 10;
    columns[1].envelope = static_cast<quint8>(kColourMax);
    columns[1].bass = 0;
    columns[1].mid = static_cast<quint8>(kColourMax);
    columns[2].envelope = 20;

    const PreviewWaveform preview = PreviewWaveform::fromColourPreview(columns);
    ASSERT_FALSE(preview.isNull());
    EXPECT_EQ(255, preview.at(0).height) << "the loudest of the three";
    EXPECT_EQ(255, preview.at(0).green) << "and its own colour, not a blend";
    EXPECT_EQ(0, preview.at(0).red);
}

TEST(PreviewWaveformTest, AShortColourPreviewIsRefused) {
    EXPECT_TRUE(PreviewWaveform::fromColourPreview(colourPreview(1199)).isNull());
    EXPECT_TRUE(PreviewWaveform::fromColourPreview({}).isNull());
}

TEST(PreviewWaveformTest, OutOfRangeColumnsAreZeroRatherThanACrash) {
    const PreviewWaveform preview = PreviewWaveform::fromPwav(pwav());
    EXPECT_EQ(0, preview.at(-1).height);
    EXPECT_EQ(0, preview.at(PreviewWaveform::kColumns).height);
    EXPECT_EQ(0, preview.at(PreviewWaveform::kColumns + 100).height);
}

} // namespace
