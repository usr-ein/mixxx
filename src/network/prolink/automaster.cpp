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

namespace {

/// Whether *peer* is a deck a stopped master hands over to.
bool takesOver(const SyncPeer& peer, int ours, bool weAreSynced) {
    return isHeardPlayer(peer, ours) && peer.playingFlag && (!weAreSynced || peer.isSynced);
}

/// The lowest-numbered deck that takes over, leaving out *offered*.
int lowestTakingOver(const std::vector<SyncPeer>& peers,
        int ours,
        bool weAreSynced,
        const std::vector<int>& offered) {
    int best = 0;
    for (const SyncPeer& peer : peers) {
        if (!takesOver(peer, ours, weAreSynced) ||
                std::find(offered.begin(), offered.end(), peer.number) != offered.end()) {
            continue;
        }
        if (best == 0 || peer.number < best) {
            best = peer.number;
        }
    }
    return best;
}

} // namespace

int successorWhenStopped(const std::vector<SyncPeer>& peers, int ours, bool weAreSynced) {
    return lowestTakingOver(peers, ours, weAreSynced, {});
}

int StopOffers::candidate(const std::vector<SyncPeer>& peers, int ours, bool weAreSynced) const {
    return lowestTakingOver(peers, ours, weAreSynced, m_offered);
}

void StopOffers::offered(int deck) {
    m_offered.push_back(deck);
}

int StopOffers::offerNext(const std::vector<SyncPeer>& peers,
        int ours,
        bool weAreSynced,
        const std::function<bool(int)>& offer) {
    const int successor = candidate(peers, ours, weAreSynced);
    if (successor == 0 || !offer(successor)) {
        return 0;
    }
    offered(successor);
    return successor;
}

void StopOffers::reset() {
    m_offered.clear();
}

bool everyPlayerHeard(const std::vector<SyncPeer>& peers,
        const std::vector<int>& players,
        int ours) {
    return std::all_of(players.begin(), players.end(), [&peers, ours](int number) {
        return number == ours ||
                std::any_of(peers.begin(), peers.end(), [number, ours](const SyncPeer& peer) {
                    return peer.number == number && isHeardPlayer(peer, ours);
                });
    });
}

bool claimsAtOnce(bool startedPlaying, bool everyPlayerHeard, int collisions) {
    return startedPlaying && everyPlayerHeard && collisions == 0;
}

} // namespace automaster
} // namespace prolink
} // namespace mixxx
