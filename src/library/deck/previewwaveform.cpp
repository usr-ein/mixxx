#include "library/deck/previewwaveform.h"

#include <algorithm>

namespace {

using mixxx::deck::PreviewWaveform;

/// Bytes the wire spends on the preview before the tiny waveform: two per
/// column, height then shade.
constexpr int kWireBytes = PreviewWaveform::kColumns * 2;

/// `PWAV`'s height, in the low five bits.
constexpr int kPwavMaxHeight = 31;
/// `PWAV`'s shade, in the top three.
constexpr int kPwavMaxShade = 7;

/// **`PWV4`'s bytes are seven bits, not eight.** Measured rather than assumed:
/// across the tracks checked no byte of any column ever exceeded 127, and one
/// track hit exactly 127 on the envelope and on all three bands. Scaling by 255
/// instead would draw every waveform at half the height of the strip and every
/// colour at half its saturation — plausible, uniformly wrong, and with nothing
/// on screen to say so.
constexpr int kColourMax = 127;

/// Columns a `PWV4` carries, against the 400 drawn.
constexpr int kColourColumns = 1200;

/// The CDJ-blue a monochrome preview starts from, going white with the shade.
constexpr int kMonoRed = 0x33;
constexpr int kMonoGreen = 0x88;
constexpr int kMonoBlue = 0xdd;

quint8 scale(int value, int from) {
    return static_cast<quint8>(std::clamp(value * 255 / std::max(from, 1), 0, 255));
}

/// One monochrome column: a height, and blue going white with the shade.
PreviewWaveform::Column monoColumn(quint8 packed) {
    const int height = packed & 0x1f;
    const int shade = packed >> 5;
    PreviewWaveform::Column column;
    column.height = scale(height, kPwavMaxHeight);
    column.red = static_cast<quint8>(kMonoRed + (255 - kMonoRed) * shade / kPwavMaxShade);
    column.green = static_cast<quint8>(kMonoGreen + (255 - kMonoGreen) * shade / kPwavMaxShade);
    column.blue = static_cast<quint8>(kMonoBlue + (255 - kMonoBlue) * shade / kPwavMaxShade);
    return column;
}

} // namespace

namespace mixxx {
namespace deck {

PreviewWaveform PreviewWaveform::fromPwav(const QByteArray& payload) {
    PreviewWaveform preview;
    if (payload.size() != kColumns) {
        return preview;
    }
    preview.m_columns.reserve(kColumns);
    for (int column = 0; column < kColumns; ++column) {
        preview.m_columns.append(monoColumn(static_cast<quint8>(payload.at(column))));
    }
    return preview;
}

PreviewWaveform PreviewWaveform::fromWire(const QByteArray& blob) {
    PreviewWaveform preview;
    if (blob.size() < kWireBytes) {
        return preview;
    }
    // The wire is the file with each byte split in two, so putting it back
    // together is the whole conversion — and then it is the same picture, drawn
    // by the same code, as a track off a stick.
    preview.m_columns.reserve(kColumns);
    for (int column = 0; column < kColumns; ++column) {
        const auto height = static_cast<quint8>(blob.at(column * 2)) & 0x1f;
        const auto shade = static_cast<quint8>(blob.at(column * 2 + 1)) & 0x07;
        preview.m_columns.append(
                monoColumn(static_cast<quint8>(height | (shade << 5))));
    }
    return preview;
}

PreviewWaveform PreviewWaveform::fromColourPreview(
        const QVector<prolink::AnlzPreviewColumn>& columns) {
    PreviewWaveform preview;
    if (columns.size() < kColourColumns) {
        // Short of what the format declares. Refused rather than stretched, for
        // the same reason a short PWAV is.
        return preview;
    }

    constexpr int kPerColumn = kColourColumns / kColumns;
    preview.m_columns.reserve(kColumns);
    for (int column = 0; column < kColumns; ++column) {
        // The loudest of the three, taken whole: its bands go with its
        // envelope, so the colour drawn is the colour of the moment that set
        // the height rather than an average of three that never happened.
        const prolink::AnlzPreviewColumn* pLoudest = nullptr;
        for (int step = 0; step < kPerColumn; ++step) {
            const prolink::AnlzPreviewColumn& candidate =
                    columns.at(column * kPerColumn + step);
            if (!pLoudest || candidate.envelope > pLoudest->envelope) {
                pLoudest = &candidate;
            }
        }

        Column out;
        out.height = scale(pLoudest->envelope, kColourMax);
        out.red = scale(pLoudest->bass, kColourMax);
        out.green = scale(pLoudest->mid, kColourMax);
        out.blue = scale(pLoudest->treble, kColourMax);
        preview.m_columns.append(out);
    }
    return preview;
}

} // namespace deck
} // namespace mixxx
