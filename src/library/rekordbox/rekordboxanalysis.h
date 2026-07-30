#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"
#include "util/color/rgbcolor.h"

/// Reading rekordbox ANLZ side-files onto a Track: beat grid, hot cues, loops,
/// memory cues and the track colour.
///
/// Extracted from `rekordboxfeature.cpp`, which had it in an anonymous
/// namespace, when the ProLink feature needed exactly the same thing for a track
/// pulled off a CDJ over the network. The two differ only in how the files got
/// onto local disk; everything from there on is identical, and duplicating a few
/// hundred lines of beat-grid arithmetic is how two copies drift apart.
///
/// The `rekordbox_anlz` Kaitai types this uses are already vendored at
/// `lib/rekordbox-metadata/` and shared by both.
namespace mixxx {
namespace rekordbox {

/// Rekordbox's eight fixed cue colours, by the id stored in the pdb.
RgbColor colorFromID(int colorID);

/// Read one ANLZ file and apply what it holds to *track*.
///
/// *ignoreCues* reads the beat grid and nothing else, which is how the pair of
/// files is combined: grids are only correct in the legacy `.DAT`, everything
/// else is better in the `.EXT`.
///
/// A missing or unparseable file is silently nothing — analysis is an
/// enhancement, and a track with no grid still plays.
void readAnalyze(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

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
void applyAnalysis(TrackPointer track, const QString& anlzPath);

} // namespace rekordbox
} // namespace mixxx
