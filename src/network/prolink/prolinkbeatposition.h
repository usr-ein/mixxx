#pragma once

#include <QString>

#include <cmath>

#include "control/controlobject.h"
#include "control/controlproxy.h"

namespace mixxx {
namespace prolink {

/// Where a deck is on its beat grid, in the terms Pro DJ Link uses.
///
/// `number` counts beats from 1 and is 0 when there is no grid to count on.
/// `fraction` is how far through that beat the playhead is.
struct BeatPosition {
    quint32 number = 0;
    double fraction = 0.0;

    bool isValid() const {
        return number > 0;
    }
};

/// Beats per bar. Four everywhere in this protocol.
constexpr int kBeatsPerBar = 4;

/// Read a deck's place on its grid **from the playhead**, not from a counter.
///
/// The elapsed track time times the file's tempo is which beat we are on, which
/// means a loop replays the same beats instead of marching on through the bar.
/// A counter driven by `beat_active` does the opposite, and that is what made a
/// one-beat loop walk the bar marker all the way round while the audio repeated
/// two beats.
///
/// **The two sources must not be allowed to disagree.** `beat_distance` is the
/// engine's own sub-beat phase and is exact; the beat *index* has to come from
/// the playhead, because nothing in Mixxx counts bars. But a real track's grid
/// does not start at zero — its first beat is a few milliseconds in — so
/// `floor(beatsElapsed)` flips to the next integer at a slightly different
/// instant than `beat_distance` wraps to 0. For that one frame the index says
/// beat 3 while the fraction still says 0.98, and anything drawn or broadcast
/// from the pair jumps a whole beat forward and then back.
///
/// Subtracting the fraction before rounding pins the index to whatever
/// `beat_distance` currently believes: the difference is near-integral by
/// construction, so the index can only change at the exact moment the fraction
/// wraps, and the two can no longer contradict each other.
inline BeatPosition beatPositionOf(double playPosition,
        double duration,
        double fileBpm,
        double beatDistance) {
    BeatPosition position;
    if (fileBpm <= 0.0 || duration <= 0.0) {
        return position;
    }
    const double trackSeconds = playPosition * duration;
    const double beatsElapsed = trackSeconds * fileBpm / 60.0;
    const auto index = static_cast<long long>(std::llround(beatsElapsed - beatDistance));
    // Beat 1 is the first beat of the track, and nothing before it exists: a
    // playhead sitting in the lead-in is on beat 1 rather than on beat -3.
    position.number = static_cast<quint32>(index < 0 ? 1 : index + 1);
    position.fraction = beatDistance;
    return position;
}

/// Where in the four-beat bar that position is, `0.0` on the downbeat.
///
/// The bar is `((number - 1) % 4) + 1`, which is a **convention rather than a
/// measurement**: nothing in a Mixxx beat grid names a downbeat. It has to
/// match what the library publishes to the network, or the meter on screen and
/// the meter on a CDJ would disagree about our own bar.
inline double barPhaseOf(const BeatPosition& position) {
    if (!position.isValid()) {
        return -1.0;
    }
    const auto inBar = static_cast<int>((position.number - 1) % kBeatsPerBar);
    return (inBar + position.fraction) / kBeatsPerBar;
}

} // namespace prolink
} // namespace mixxx
