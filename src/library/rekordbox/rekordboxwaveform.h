#pragma once

#include <QString>

#include "audio/types.h"
#include "track/track_decl.h"

class AnalysisDao;

/// Building Mixxx waveforms from rekordbox's own, so a track exported from
/// rekordbox does not have to be decoded again just to draw it.
///
/// Without this, loading a rekordbox track shows "Ready to play, analyzing..."
/// while Mixxx decodes the whole file: the beat grid and cues come from the ANLZ
/// files and every other analyzer is skipped, but the waveform is stored in a
/// Mixxx-specific format that has nothing to reuse. On a Raspberry Pi that is
/// tens of seconds of background work per track.
///
/// rekordbox has already done the analysis. Its `PWV5` colour waveform carries
/// three frequency bands and an overall magnitude at 150 points per second,
/// which is very nearly what Mixxx's `WaveformData` wants.
namespace mixxx {
namespace rekordbox {

/// Fill *track*'s waveform and waveform summary from the colour waveform in
/// *anlzPathExt*, and persist them through *pAnalysisDao*.
///
/// Returns false if the file has no colour waveform — a track analysed by an
/// older rekordbox legitimately lacks one — in which case Mixxx's own analyzer
/// runs as before. *pAnalysisDao* may be null, in which case the waveforms are
/// set on the track but not saved, which is still enough to skip the decode for
/// this session.
/// *sampleRate* overrides the track's own, for a caller that knows it before
/// anything has decoded the audio -- see applyAnalysis().
bool importWaveforms(TrackPointer track,
        const QString& anlzPathExt,
        AnalysisDao* pAnalysisDao,
        mixxx::audio::SampleRate sampleRate = mixxx::audio::SampleRate());

} // namespace rekordbox
} // namespace mixxx
