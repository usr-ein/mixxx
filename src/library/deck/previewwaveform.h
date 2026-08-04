#pragma once

#include <QByteArray>
#include <QVector>

#include "network/prolink/prolinkanlz.h"

namespace mixxx {
namespace deck {

/// A track's preview waveform: 400 columns, each a colour and a height.
///
/// **One type from two very different tags**, because the panel that draws it
/// should not know which medium a track came from:
///
///  * `PWV4`, the colour preview in the `.EXT` — 1200 columns of bass, mid,
///    treble and an envelope. This is the good one, and the one a local stick
///    can always give.
///  * `PWAV`, the monochrome preview in the `.DAT` — 400 columns of a height
///    and a shade. The fallback, and **the only one a CDJ will hand over**: a
///    player answers `GET_WAVEFORM_PREVIEW` with `PWAV` and nothing else, so a
///    remote medium draws this one until `ANLZ_TAG_REQ` is implemented.
///
/// Both end up here as the same 400 columns, so a track off a stick and a track
/// off a player differ in colour and in nothing else.
///
/// A value type. It is built on a worker thread and read on the GUI thread.
class PreviewWaveform final {
  public:
    /// Columns drawn. Fixed by `PWAV`'s own length, and chosen because it is
    /// exactly the drawable width of the info panel — one column, one pixel, no
    /// resampling for the tag that sets it.
    static constexpr int kColumns = 400;

    /// One column, ready to draw.
    struct Column {
        quint8 red = 0;
        quint8 green = 0;
        quint8 blue = 0;
        /// 0..255 of the strip's height.
        quint8 height = 0;
    };

    PreviewWaveform() = default;

    /// From a `PWAV` payload, as a local `.DAT` carries it.
    ///
    /// Null unless the payload is exactly kColumns bytes. A shorter one is a
    /// tag we cannot read, and stretching it would draw a plausible waveform
    /// that is wrong about where everything in the track is — worse than
    /// drawing none, because nothing on screen would say so.
    ///
    /// Drawn blue going white with the shade rekordbox stored, which is how a
    /// CDJ draws this same tag.
    static PreviewWaveform fromPwav(const QByteArray& payload);

    /// From a `GET_WAVEFORM_PREVIEW` reply, as a player puts it on the wire.
    ///
    /// 900 bytes: the packed columns split into 800 bytes of (height, shade)
    /// pairs, then the 100-byte `PWV2` tiny waveform appended, which this
    /// ignores. Accepts anything holding at least the 800.
    static PreviewWaveform fromWire(const QByteArray& blob);

    /// From a `PWV4` colour preview.
    ///
    /// 1200 columns into 400, three to one, keeping the loudest of each three.
    /// The **peak** rather than the mean, because three inputs per output is a
    /// narrow window and averaging it would round off exactly the transients a
    /// preview is read for — the same rule, and the same reason, as the detail
    /// waveform's resampler.
    static PreviewWaveform fromColourPreview(const QVector<prolink::AnlzPreviewColumn>& columns);

    /// True for a track with no preview, and for one whose preview did not
    /// parse. The panel draws the same thing for both.
    bool isNull() const {
        return m_columns.isEmpty();
    }

    /// Zeroed outside the waveform, so a caller cannot walk off the end.
    Column at(int column) const {
        return column >= 0 && column < m_columns.size() ? m_columns.at(column) : Column();
    }

  private:
    /// kColumns entries, or empty.
    QVector<Column> m_columns;
};

} // namespace deck
} // namespace mixxx
