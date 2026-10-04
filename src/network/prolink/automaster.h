#pragma once

#include <vector>

#include "network/prolink/syncsource.h"

namespace mixxx {
namespace prolink {

/// When this deck takes or hands over tempo master on its own, the way a CDJ
/// does (owner decisions 2, 3 and 13). Pure rules; the service gathers the
/// inputs and acts.
namespace automaster {

/// How long the network must have had no master before a deck that may claim
/// it does: 3 s, doubled per collision, plus 1 s per player number above 1.
///
/// The per-number step is the tie-break. Two TriMixxx decks that lose the
/// master together both wait, and the lower-numbered one's claim is on the
/// wire a second before the other's deadline -- which then sees a master and
/// does not claim. If packet loss makes both claim anyway, both stand down
/// (an auto-claim yields to any rival) and wait twice as long, in the same
/// order.
int claimDelayMs(int ours, int collisions);

/// The most collisions counted; the delay stops doubling there (48 s plus
/// the step).
constexpr int kMaxCollisions = 4;

/// After yielding or standing down, how long before an auto-claim: a deck
/// that has just handed master over must not take it straight back.
constexpr int kHoldOffMs = 10000;

struct ClaimInputs {
    /// Our player number; 1-4 is a player, anything else cannot be master.
    int ours = 0;
    bool weAreMaster = false;
    /// Any deck we can hear claims master, or is handing it over.
    bool anyClaim = false;
    /// Our deck is playing a track with a tempo.
    bool playingWithTempo = false;
    /// SYNC is following another deck: decision 3, no claim then.
    bool following = false;
    /// Within kHoldOffMs of yielding or standing down.
    bool holdingOff = false;
};

/// Whether this deck should be counting towards an auto-claim at all.
bool mayClaim(const ClaimInputs& inputs);

/// The deck to hand master to when we are master and our deck has stopped:
/// the lowest-numbered player that is synced and playing, heard from
/// recently. 0 for none.
int successorWhenStopped(const std::vector<SyncPeer>& peers, int ours);

} // namespace automaster
} // namespace prolink
} // namespace mixxx
