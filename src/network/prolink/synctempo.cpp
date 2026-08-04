#include "network/prolink/synctempo.h"

namespace mixxx {
namespace prolink {

void SyncTempo::reset() {
    m_effectiveBpm = 0.0;
    m_haveEffective = false;
    m_faderInControl = true;
    m_faderWasAbove = false;
}

SyncTempo::Output SyncTempo::update(const Inputs& in) {
    if (in.fileBpm <= 0.0) {
        // No track, so no tempo and nothing to hand over. Deliberately a reset
        // rather than a hold: the next track arrives with the fader wherever it
        // is, and making it catch up to a tempo from a track that is no longer
        // loaded would be a fader that does nothing for no visible reason.
        reset();
        return {};
    }

    const double faderBpm = in.fileBpm * (1.0 + in.faderPercent / 100.0);
    const bool following = in.syncEnabled && !in.isMaster && in.masterBpm > 0.0;

    if (following) {
        m_effectiveBpm = in.masterBpm;
        m_haveEffective = true;
        // Recomputed every update rather than once on entry: the master's tempo
        // is moving, so which side the stationary fader is on can change while
        // we follow. What matters is the side it is on at the moment we take
        // over, and that is whatever this last recorded.
        m_faderInControl = false;
        m_faderWasAbove = faderBpm > m_effectiveBpm;
        return {m_effectiveBpm, false};
    }

    if (!m_haveEffective) {
        // Nothing has ever driven this deck's tempo, so there is nothing for
        // the fader to catch up to.
        m_faderInControl = true;
    }

    if (!m_faderInControl) {
        const bool caught = m_faderWasAbove ? (faderBpm <= m_effectiveBpm)
                                            : (faderBpm >= m_effectiveBpm);
        m_faderInControl = caught;
    }

    if (m_faderInControl) {
        m_effectiveBpm = faderBpm;
        m_haveEffective = true;
    }
    return {m_effectiveBpm, m_faderInControl};
}

} // namespace prolink
} // namespace mixxx
