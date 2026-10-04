#include "network/prolink/audiblebeatclock.h"

#include <algorithm>

#include "control/controlproxy.h"
#include "mixer/playerinfo.h"
#include "track/track.h"
#include "waveform/visualplayposition.h"

namespace mixxx {
namespace prolink {

AudibleBeatClock::AudibleBeatClock(const QString& group, QObject* pParent)
        : m_group(group),
          m_pVisualPlayPosition(VisualPlayPosition::getVisualPlayPosition(group)) {
    const auto control = [&](const QString& controlGroup, const char* item) {
        return std::make_unique<ControlProxy>(controlGroup,
                QString::fromLatin1(item),
                pParent,
                ControlFlag::NoWarnIfMissing);
    };
    m_pPlayPosition = control(group, "playposition");
    m_pDuration = control(group, "duration");
    m_pBpm = control(group, "bpm");
    m_pIntroStart = control(group, "intro_start_position");
    m_pRateRatio = control(group, "rate_ratio");
    m_pTrimMs = control(QStringLiteral("[ProLink]"), "phase_trim_ms");
}

AudibleBeatClock::~AudibleBeatClock() = default;

BeatPosition AudibleBeatClock::now() const {
    const double duration = m_pDuration->get();
    // `bpm` is 0 for a moment after a load, before the engine has the grid,
    // while the track already has one: no phase until both agree there is a
    // grid, so the meter, SYNC and the network agree on it too.
    if (duration <= 0.0 || m_pBpm->get() <= 0.0) {
        return BeatPosition();
    }
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(m_group);
    const mixxx::BeatsPointer pBeats = pTrack ? pTrack->getBeats() : mixxx::BeatsPointer();
    const double sampleRate = pTrack ? pTrack->getSampleRate().toDouble() : 0.0;
    if (!pBeats || sampleRate <= 0.0) {
        return BeatPosition();
    }

    // Where the DAC is in the track, in seconds: the engine's position, moved
    // by how far the DAC is behind it, and the trim -- real seconds, which the
    // deck's own rate turns into track seconds.
    const double audibleOffset = m_pVisualPlayPosition
            ? m_pVisualPlayPosition->getAudibleOffsetNow()
            : 0.0;
    const double trimSeconds = m_pTrimMs->valid() ? m_pTrimMs->get() / 1000.0 : 0.0;
    const double trackSeconds = (m_pPlayPosition->get() + audibleOffset) * duration -
            trimSeconds * m_pRateRatio->get();
    const auto here = mixxx::audio::FramePos(trackSeconds * sampleRate);

    // **On the grid itself**, not elapsed time times file_bpm: that assumed a
    // constant tempo from the start of the track, so on a variable-tempo grid
    // the count drifted and jumped a beat mid-beat.
    auto next = pBeats->iteratorFrom(here);
    auto prev = next - 1;
    if (*next <= here) {
        prev = next;
        ++next;
    }
    const double beatFrames = *next - *prev;
    if (beatFrames <= 0.0) {
        return BeatPosition();
    }

    // **Counted from the downbeat**: rekordbox's first downbeat is imported as
    // the intro cue, and it is the red line the waveform draws. Counting from
    // the first beat instead put bar 1 on an arbitrary beat three times in
    // four, on the meter and for every CDJ lining its bars up with ours (owner
    // decision 14). No intro cue: the first beat, as before.
    const auto first = pBeats->iteratorFrom(pBeats->firstBeat());
    const auto intro = mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
            m_pIntroStart->get());
    const auto anchor = intro.isValid()
            ? pBeats->iteratorFrom(pBeats->findClosestBeat(intro))
            : first;
    // Beats are numbered from 1 at the first beat, as the wire expects, and
    // padded so the anchor falls on a downbeat: (number - 1) % 4 is then the
    // place in the bar counted from the anchor.
    const int beforeAnchor = anchor - first;
    const int pad = ((-beforeAnchor) % kBeatsPerBar + kBeatsPerBar) % kBeatsPerBar;
    const int number = (prev - first) + 1 + pad;

    BeatPosition position;
    // Before the first beat -- the lead-in -- is beat 1.
    position.number = static_cast<quint32>(std::max(number, 1));
    position.fraction = std::clamp((here - *prev) / beatFrames, 0.0, 1.0);
    return position;
}

} // namespace prolink
} // namespace mixxx
