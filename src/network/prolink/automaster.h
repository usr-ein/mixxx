#pragma once

#include <functional>
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
/// master over too. Two things keep it to one successor at a time:
///
///  * **The session refuses an offer while one is in flight**, until it is
///    taken up or withdrawn (two seconds, `KeepIt`). A player claims master
///    12-69 ms after it is named (S28), so naming a second deck over the
///    first could leave two of them claiming it.
///  * **A deck counts as offered only once the session took the offer**, and
///    is never offered again in the same stop: it would be renewed every two
///    seconds for as long as it plays, and every listener reading `0x9f`
///    would take it for the master meanwhile.
class StopOffers {
  public:
    /// The deck to offer master to now, or 0: the lowest-numbered deck the
    /// rule names that has not taken an offer during this stop.
    int candidate(const std::vector<SyncPeer>& peers, int ours, bool weAreSynced) const;
    /// The session took our offer to *deck*: not offered it again this stop.
    void offered(int deck);
    /// One poll: offer the candidate through *offer*, the session's, and
    /// count it only if the session took it. Returns the deck now offered, or
    /// 0 for none (no candidate, or the session refused, an offer being in
    /// flight).
    int offerNext(const std::vector<SyncPeer>& peers,
            int ours,
            bool weAreSynced,
            const std::function<bool(int)>& offer);
    /// Our deck plays again: its next stop starts afresh.
    void reset();

  private:
    std::vector<int> m_offered;
};

/// Whether every player we know of has sent status we still believe.
///
/// "Nobody is master" is known only then, rather than not yet heard: a peer
/// unicasts status only once it has heard our keep-alive, and a claim older
/// than a second is not believed. *players* are the online player numbers in
/// the device table; *ours* is skipped.
bool everyPlayerHeard(const std::vector<SyncPeer>& peers,
        const std::vector<int>& players,
        int ours);

/// Whether an empty mastership is claimed at once, rather than after
/// claimDelayMs(): our deck has just started to play, every player has been
/// heard, and no claim of ours has collided.
///
/// The first deck to play takes master as it starts (S28 22.058, one status
/// packet later; E01 23.269, the same packet). claimDelayMs() is owner
/// decision 2, for a master lost while we play, and it still covers that,
/// and a collision.
bool claimsAtOnce(bool startedPlaying, bool everyPlayerHeard, int collisions);

} // namespace automaster
} // namespace prolink
} // namespace mixxx
