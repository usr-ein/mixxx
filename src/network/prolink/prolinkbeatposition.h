#pragma once

#include <QtGlobal>

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

/// Where in the four-beat bar that position is, `0.0` on the downbeat.
///
/// The bar is `((number - 1) % 4) + 1`. For this deck AudibleBeatClock numbers
/// beats so that the rekordbox downbeat (the intro cue) falls on 1; a track
/// with no intro cue counts from its first beat, which is a convention rather
/// than a measurement. It has to match what we publish to the network, or the
/// meter on screen and the meter on a CDJ would disagree about our own bar.
inline double barPhaseOf(const BeatPosition& position) {
    if (!position.isValid()) {
        return -1.0;
    }
    const auto inBar = static_cast<int>((position.number - 1) % kBeatsPerBar);
    return (inBar + position.fraction) / kBeatsPerBar;
}

} // namespace prolink
} // namespace mixxx
