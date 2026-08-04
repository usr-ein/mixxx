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
/// instead would draw every colour at half its saturation — plausible,
/// uniformly wrong, and with nothing on screen to say so.
constexpr int kColourMax = 127;

/// Columns a `PWV4` carries, against the kColumns drawn.
constexpr int kColourColumns = 1200;
constexpr int kColourPerColumn = kColourColumns / PreviewWaveform::kColumns;

/// The CDJ-blue a monochrome preview starts from, going white with the shade.
constexpr int kMonoRed = 0x33;
constexpr int kMonoGreen = 0x88;
constexpr int kMonoBlue = 0xdd;

quint8 scale(int value, int from) {
    return static_cast<quint8>(std::clamp(value * 255 / std::max(from, 1), 0, 255));
}

} // namespace

namespace mixxx {
namespace deck {

PreviewWaveform PreviewWaveform::fromAnlz(
        const QByteArray& pwav, const QVector<prolink::AnlzPreviewColumn>& colour) {
    const bool haveMono = pwav.size() == kColumns;
    const bool haveColour = colour.size() >= kColourColumns;

    PreviewWaveform preview;
    if (!haveMono && !haveColour) {
        return preview;
    }
    preview.m_columns.reserve(kColumns);

    for (int column = 0; column < kColumns; ++column) {
        Column out;

        // The colour, averaged over the three `PWV4` columns this one covers.
        // The **mean** and not the peak, because all that is being taken from
        // them is a hue: the loudest of three has no better claim to the colour
        // of the moment than its neighbours, and following it makes the strip
        // flicker between bands on every transient.
        if (haveColour) {
            int bass = 0;
            int mid = 0;
            int treble = 0;
            for (int step = 0; step < kColourPerColumn; ++step) {
                const prolink::AnlzPreviewColumn& entry =
                        colour.at(column * kColourPerColumn + step);
                bass += entry.bass;
                mid += entry.mid;
                treble += entry.treble;
            }
            out.red = scale(bass / kColourPerColumn, kColourMax);
            out.green = scale(mid / kColourPerColumn, kColourMax);
            out.blue = scale(treble / kColourPerColumn, kColourMax);
        }

        if (haveMono) {
            // The height, straight off `PWAV` -- one of its columns per one of
            // ours, so nothing is resampled and nothing is inflated by taking a
            // peak over a window.
            const auto packed = static_cast<quint8>(pwav.at(column));
            out.height = scale(packed & 0x1f, kPwavMaxHeight);
            if (!haveColour) {
                // No bands to colour it with, so blue going white with the
                // shade, the way a CDJ draws this tag on its own.
                const int shade = packed >> 5;
                out.red = static_cast<quint8>(
                        kMonoRed + (255 - kMonoRed) * shade / kPwavMaxShade);
                out.green = static_cast<quint8>(
                        kMonoGreen + (255 - kMonoGreen) * shade / kPwavMaxShade);
                out.blue = static_cast<quint8>(
                        kMonoBlue + (255 - kMonoBlue) * shade / kPwavMaxShade);
            }
        } else {
            // No `PWAV`. `PWV4`'s envelope is compressed enough that a whole
            // track reads as one block, but a compressed waveform beats none.
            int envelope = 0;
            for (int step = 0; step < kColourPerColumn; ++step) {
                envelope += colour.at(column * kColourPerColumn + step).envelope;
            }
            out.height = scale(envelope / kColourPerColumn, kColourMax);
        }

        preview.m_columns.append(out);
    }
    return preview;
}

PreviewWaveform PreviewWaveform::fromWire(const QByteArray& blob) {
    if (blob.size() < kWireBytes) {
        return PreviewWaveform();
    }
    // The wire is the file with each byte split in two, so putting it back
    // together is the whole conversion -- and then it is the same picture, by
    // the same code, as a track off a stick with no `.EXT`.
    QByteArray packed(kColumns, '\0');
    for (int column = 0; column < kColumns; ++column) {
        const auto height = static_cast<quint8>(blob.at(column * 2)) & 0x1f;
        const auto shade = static_cast<quint8>(blob.at(column * 2 + 1)) & 0x07;
        packed[column] = static_cast<char>(height | (shade << 5));
    }
    return fromAnlz(packed, {});
}

} // namespace deck
} // namespace mixxx
