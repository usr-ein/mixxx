#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"
#include "util/color/rgbcolor.h"

class AnalysisDao;

/// Applying rekordbox's ANLZ side-files to a Track: beat grid, hot cues, loops,
/// memory cues and the track colour.
///
/// Extracted from the old rekordbox library feature, which had it in an
/// anonymous namespace, when the ProLink feature needed exactly the same thing
/// for a track pulled off a CDJ over the network. The two differ only in how the
/// files got onto local disk; everything from there on is identical, and
/// duplicating a few hundred lines of beat-grid arithmetic is how two copies
/// drift apart.
///
/// **This module applies; it does not parse.** The files are read by
/// `prolink-rekordbox` through `network/prolink/prolinkanlz.h`, which is also
/// what the Pro DJ Link serve side reads them with. What lives here is
/// everything that is Mixxx's rather than rekordbox's: milliseconds into frame
/// positions, the decoder timing offset, and Mixxx's own cue objects.
namespace mixxx {
namespace rekordbox {

/// Rekordbox's eight fixed cue colours, by the id stored in the pdb.
RgbColor colorFromID(int colorID);

/// The decoder timing offset for a local audio file, in frames.
///
/// Rekordbox's positions are relative to its own decoder, and different MP3
/// decoders disagree about where a file starts. mp3guessenc classifies the file
/// and the offset compensates; see mixxxdj/mixxx#2119.
int timingOffsetForFile(const QString& location);

/// The whole job: work out the timing offset, pick the right combination of
/// `.DAT` and `.EXT`, and apply both.
///
/// *anlzPath* is the `.DAT`; the `.EXT` is derived from it. Callers that have
/// both files locally should use this rather than calling readAnalyze directly,
/// so the "grids only from the legacy file" rule lives in one place.
/// *pAnalysisDao* is optional and used only to persist an imported waveform; see
/// rekordboxwaveform.h. Without it the waveform still applies for this session.
/// *sampleRate* overrides the track's own, for a caller that knows it before
/// anything has decoded the audio. **A streamed track needs this.** Everything
/// rekordbox stores is in milliseconds and has to be converted to frames, and
/// asking the decoder for the rate means blocking on bytes that have not
/// arrived -- on the GUI thread, which is the only thread that announces their
/// arrival, so it does not block, it deadlocks. A rekordbox `export.pdb`
/// carries the sample rate for every track, which settles it for free.
void applyAnalysis(TrackPointer track,
        const QString& anlzPath,
        AnalysisDao* pAnalysisDao = nullptr,
        audio::SampleRate sampleRate = audio::SampleRate());

} // namespace rekordbox
} // namespace mixxx
