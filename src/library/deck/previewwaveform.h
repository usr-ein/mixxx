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

    /// From a track's two analysis files: `PWAV` out of the `.DAT`, `PWV4` out
    /// of the `.EXT`. Either may be empty.
    ///
    /// **The height comes from `PWAV` and the colour from `PWV4`**, because
    /// each is better at one of them and neither is good at both.
    ///
    /// `PWV4`'s envelope byte is badly compressed: measured over five tracks it
    /// puts the median column at 87–98 % of full height and the lower quartile
    /// at 58–89 %, so a whole track draws as one solid block and a breakdown is
    /// the only thing that reads. `PWAV`'s height, an independent encoding of
    /// the same music, puts those at 58–77 % and 39–74 % on the same tracks —
    /// consistently the wider range, and it is what a CDJ draws its own preview
    /// from. It is also exactly kColumns long, so the height needs no
    /// resampling at all.
    ///
    /// What `PWV4` is good at is the three frequency bands, which `PWAV` does
    /// not carry. So: its bands, `PWAV`'s envelope.
    ///
    /// Falls back gracefully. No `PWV4` — an older rekordbox, or a `.DAT` with
    /// no `.EXT` beside it — draws blue going white with `PWAV`'s shade, the way
    /// a CDJ draws that tag. No `PWAV` uses `PWV4`'s envelope after all, which
    /// is compressed but is not nothing.
    static PreviewWaveform fromAnlz(const QByteArray& pwav,
            const QVector<prolink::AnlzPreviewColumn>& colour);

    /// From a `GET_WAVEFORM_PREVIEW` reply, as a player puts it on the wire.
    ///
    /// 900 bytes: the packed columns split into 800 bytes of (height, shade)
    /// pairs, then the 100-byte `PWV2` tiny waveform appended, which this
    /// ignores. Accepts anything holding at least the 800.
    static PreviewWaveform fromWire(const QByteArray& blob);


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
