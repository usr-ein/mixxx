#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

/// Reading rekordbox's per-track analysis files, `ANLZ####.DAT` and `.EXT`.
///
/// A Qt-native view of what `prolink-rekordbox` parses, in the same shape and
/// for the same reason as `prolinkpdb.h` beside it: nothing outside this pair
/// includes the generated cxx header, so the bridge stays one file wide.
///
/// **One parser for one file format.** These files used to be read a second
/// time by a vendored Kaitai parser compiled into Mixxx, which is how two
/// readers of one format drift — the argument `browser-prd.md` §11.4 made about
/// `export.pdb`, applied to the files beside it. The Rust reader was already
/// the one the Pro DJ Link serve side used to answer a CDJ asking us for a
/// track's waveform, so this is that reader reached from one more place rather
/// than a new one.
namespace mixxx {
namespace prolink {

/// One beat of a `PQTZ` grid.
///
/// Both the position and the tempo are given because they disagree, and which
/// to believe depends on the track. See `rekordboxanalysis.cpp`, which picks.
struct AnlzBeat {
    /// Which beat of the bar, 1–4.
    quint16 beatNumber = 0;
    /// Centi-BPM at this beat, so 145.00 is 14500.
    quint16 tempo = 0;
    /// Milliseconds from the start of the track.
    quint32 timeMs = 0;
};

/// One cue or loop, from either cue tag.
struct AnlzCue {
    /// True when this came from `PCO2`, and so when the comment and the
    /// colours below mean anything.
    bool extended = false;
    /// True for the hot-cue list, false for the memory-cue list.
    bool hotList = false;
    /// 0 for a memory cue, 1 for hot cue A, 2 for B, and so on.
    quint32 hotCue = 0;
    /// True when this is a loop and `loopTimeMs` means something.
    bool isLoop = false;
    quint32 timeMs = 0;
    /// Where a loop jumps back from. Zero unless `isLoop`.
    quint32 loopTimeMs = 0;
    /// The DJ's comment, already out of the file's UTF-16BE.
    QString comment;
    /// Row id in rekordbox's colour table.
    quint32 colorId = 0;
    quint8 colorRed = 0;
    quint8 colorGreen = 0;
    quint8 colorBlue = 0;
};

/// One column of the `PWV5` colour scrolling waveform.
///
/// **The three bands are a hue and `height` is the amplitude.** The band names
/// are not the colour names: `treble` is drawn blue and `mid` green.
struct AnlzColourColumn {
    /// Bits 15–13, drawn red.
    quint8 bass = 0;
    /// Bits 9–7, drawn green.
    quint8 mid = 0;
    /// Bits 12–10, drawn blue.
    quint8 treble = 0;
    /// Bits 6–2. The envelope, 0–31.
    quint8 height = 0;
};

/// Everything read out of one analysis file.
///
/// A `.DAT` carries the grid, the plain cues and the preview; a `.EXT` the
/// extended cues and the colour waveform. An absent tag is an empty member and
/// not a failure — which tags a file has depends on the rekordbox version that
/// wrote it.
struct AnlzContents {
    /// False only when the file could not be read or its container did not
    /// parse. A single tag that does not parse costs that tag and leaves this
    /// true.
    bool ok = false;
    /// Why not. Empty when `ok`.
    QString error;
    QVector<AnlzBeat> beats;
    QVector<AnlzCue> cues;
    QVector<AnlzColourColumn> colourDetail;
    /// `PWAV`: 400 packed bytes, height in the low five bits and shade in the
    /// top three.
    QByteArray preview;
};

/// One column of the `PWV4` colour preview waveform.
///
/// The band names are not the colour names, and which byte is which was
/// measured rather than read off the field order — see the bridge's own note.
struct AnlzPreviewColumn {
    /// Bottom third. Drawn red.
    quint8 bass = 0;
    /// Middle third. Drawn green.
    quint8 mid = 0;
    /// Top third. Drawn blue.
    quint8 treble = 0;
    /// Energy of the bottom half, and the best proxy for the column's height:
    /// it is the byte that tracks the monochrome preview.
    quint8 envelope = 0;
};

/// The preview waveforms of one analysis file, and nothing else from it.
struct AnlzPreview {
    bool ok = false;
    QString error;
    /// `PWAV`: 400 packed bytes. Empty when the file has none.
    QByteArray mono;
    /// `PWV4`: 1200 colour columns. Empty when the file has none.
    QVector<AnlzPreviewColumn> colour;
};

/// Read only the preview waveforms out of one analysis file.
///
/// **Not `readAnlz(path).preview`**, and the difference is the point: this
/// seeks past every tag it was not asked for, so reaching the 1200 colour
/// columns of a 157 kB `.EXT` costs about 10 kB of reads and does not decode
/// the two fifty-thousand-entry detail waveforms sitting beside them.
///
/// That matters because it runs once per row a DJ pauses on, off a stick that
/// may be busy copying the track they are about to play.
///
/// Safe to call from any thread, like readAnlz().
AnlzPreview readAnlzPreview(const QString& path);

/// Read one analysis file.
///
/// Never throws and never blocks on anything but the filesystem. A missing or
/// unreadable file comes back with `ok` false rather than as an error worth
/// stopping for: analysis is an enhancement, and a track with no grid still
/// plays.
///
/// **Safe to call from any thread**, which is the other half of why it is a
/// free function: it opens no session and touches no runtime.
AnlzContents readAnlz(const QString& path);

} // namespace prolink
} // namespace mixxx
