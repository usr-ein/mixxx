#include "network/prolink/automaster.h"

#include <algorithm>

namespace mixxx {
namespace prolink {
namespace automaster {

namespace {
constexpr int kBaseDelayMs = 3000;
constexpr int kStepPerNumberMs = 1000;
} // namespace

int claimDelayMs(int ours, int collisions) {
    const int doubled = kBaseDelayMs << std::clamp(collisions, 0, kMaxCollisions);
    return doubled + std::max(0, ours - 1) * kStepPerNumberMs;
}

bool mayClaim(const ClaimInputs& inputs) {
    return isOurPlayerNumber(inputs.ours) && !inputs.weAreMaster && !inputs.anyClaim &&
            inputs.playingWithTempo && !inputs.following && !inputs.holdingOff;
}

int successorWhenStopped(const std::vector<SyncPeer>& peers, int ours) {
    int best = 0;
    for (const SyncPeer& peer : peers) {
        if (!isHeardPlayer(peer, ours) || !peer.isSynced || !peer.playing) {
            continue;
        }
        if (best == 0 || peer.number < best) {
            best = peer.number;
        }
    }
    return best;
}

} // namespace automaster
} // namespace prolink
} // namespace mixxx
