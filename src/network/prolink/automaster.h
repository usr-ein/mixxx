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
/// the lowest-numbered player heard from recently whose status says it plays
/// (`SyncPeer::playingFlag`), and a synced one only if we are synced. 0 for
/// none.
///
/// What the CDJ-2000NXS does (`docs/prolink-learnings.md`, L2). A master that
/// is not playing hands master to a deck that plays (S28 193.655 and 208.276;
/// E01, E05, E06 A, C, D), **unless it is synced and that deck is not** (E06
/// B), and gives it away the moment its own SYNC goes off (E06 E).
int successorWhenStopped(const std::vector<SyncPeer>& peers, int ours, bool weAreSynced);

/// The successors offered master during one stop of our deck.
///
/// The rule above is held for as long as we are master and stopped, as a
/// CDJ holds it: a deck that starts later, or our SYNC going off, hands
/// master over too. But an offer that is not taken up is never repeated to
/// the same deck in the same stop, or it would be renewed every two seconds
/// for as long as the deck plays, and every listener reading `0x9f` would
/// take that deck for the master meanwhile.
class StopOffers {
  public:
    /// The deck to offer master to now, or 0: the lowest-numbered deck the
    /// rule names that has not been offered it during this stop.
    int next(const std::vector<SyncPeer>& peers, int ours, bool weAreSynced);
    /// Our deck plays again: its next stop starts afresh.
    void reset();

  private:
    std::vector<int> m_offered;
};

} // namespace automaster
} // namespace prolink
} // namespace mixxx
