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
        // Either nobody holds mastership — the seconds after power-on, before
        // anyone has claimed — or the holder is stopped or empty and publishes
        // the no-tempo sentinel. Following a zero would drag this deck to a
        // standstill, and a fader that does nothing because of a master that
        // does not exist is worse than no sync at all.
        return Source::Fader;
    }
    return Source::Master;
}

} // namespace prolink
} // namespace mixxx
