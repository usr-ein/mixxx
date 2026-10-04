#include "network/prolink/synctempo.h"

namespace mixxx {
namespace prolink {

SyncTempo::Source SyncTempo::decide(const State& state) {
    if (!state.syncEnabled) {
        // Free. This deck's own fader, ignoring everyone.
        return Source::Fader;
    }
    if (state.isMaster) {
        // SYNC on the master is inert: the reference cannot follow itself.
        return Source::Fader;
    }
    if (state.masterBpm <= 0.0) {
        // Nothing to follow: no master and nobody playing, a master that has
        // stopped (whose SYNC the service then releases, owner decision 15),
        // or a deck with no tempo to take. Following a zero would drag this
        // deck to a standstill, and a fader that does nothing because of a
        // deck that is not there is worse than no sync at all.
        return Source::Fader;
    }
    return Source::Master;
}

} // namespace prolink
} // namespace mixxx
