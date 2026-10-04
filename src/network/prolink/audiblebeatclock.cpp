#include "network/prolink/audiblebeatclock.h"

#include <cmath>

#include "control/controlproxy.h"
#include "waveform/visualplayposition.h"

namespace mixxx {
namespace prolink {

AudibleBeatClock::AudibleBeatClock(const QString& group, QObject* pParent)
        : m_pVisualPlayPosition(VisualPlayPosition::getVisualPlayPosition(group)) {
    const auto control = [&](const QString& controlGroup, const char* item) {
        return std::make_unique<ControlProxy>(controlGroup,
                QString::fromLatin1(item),
                pParent,
                ControlFlag::NoWarnIfMissing);
    };
    m_pPlayPosition = control(group, "playposition");
    m_pBeatDistance = control(group, "beat_distance");
    m_pDuration = control(group, "duration");
    m_pFileBpm = control(group, "file_bpm");
    m_pRateRatio = control(group, "rate_ratio");
    m_pTrimMs = control(QStringLiteral("[ProLink]"), "phase_trim_ms");
}

AudibleBeatClock::~AudibleBeatClock() = default;

BeatPosition AudibleBeatClock::now() const {
    const double duration = m_pDuration->get();
    const double fileBpm = m_pFileBpm->get();
    if (duration <= 0.0 || fileBpm <= 0.0) {
        return BeatPosition();
    }
    // How far the DAC is from the engine, in track seconds, and the trim, in
    // real seconds -- which the deck's own rate turns into track seconds.
    const double audibleOffset = m_pVisualPlayPosition
            ? m_pVisualPlayPosition->getAudibleOffsetNow()
            : 0.0;
    const double trimSeconds = m_pTrimMs->valid() ? m_pTrimMs->get() / 1000.0 : 0.0;
    const double trackSeconds = audibleOffset * duration - trimSeconds * m_pRateRatio->get();
    // The grid is laid out at file_bpm, so that is the tempo track seconds
    // turn into beats at; see beatPositionOf().
    const double shiftBeats = trackSeconds * fileBpm / 60.0;
    double beatDistance = m_pBeatDistance->get() + shiftBeats;
    beatDistance -= std::floor(beatDistance);
    return beatPositionOf(m_pPlayPosition->get() + trackSeconds / duration,
            duration,
            fileBpm,
            beatDistance);
}

} // namespace prolink
} // namespace mixxx
