#include "network/prolink/automaster.h"

#include <gtest/gtest.h>

namespace {

using namespace mixxx::prolink;
using namespace mixxx::prolink::automaster;

ClaimInputs eligible() {
    ClaimInputs inputs;
    inputs.ours = 2;
    inputs.playingWithTempo = true;
    return inputs;
}

SyncPeer peer(int number, bool synced, bool playing) {
    SyncPeer p;
    p.number = number;
    p.hasStatus = true;
    p.statusAgeMs = 100.0;
    p.isSynced = synced;
    p.playing = playing;
    p.playingFlag = playing;
    return p;
}

} // namespace

TEST(AutoMaster, ClaimsOnlyWhenPlayingFreeAndNobodyIsMaster) {
    EXPECT_TRUE(mayClaim(eligible()));
    ClaimInputs inputs = eligible();
    inputs.anyClaim = true;
    EXPECT_FALSE(mayClaim(inputs));
    inputs = eligible();
    inputs.playingWithTempo = false;
    EXPECT_FALSE(mayClaim(inputs));
    inputs = eligible();
    inputs.weAreMaster = true;
    EXPECT_FALSE(mayClaim(inputs));
    inputs = eligible();
    inputs.holdingOff = true;
    EXPECT_FALSE(mayClaim(inputs));
}

// Owner decision 3: a deck following another through SYNC does not claim.
TEST(AutoMaster, NeverWhileFollowingAnotherDeck) {
    ClaimInputs inputs = eligible();
    inputs.following = true;
    EXPECT_FALSE(mayClaim(inputs));
}

// An observer or an unnumbered deck publishes no claim anyone would see.
TEST(AutoMaster, OnlyAPlayerNumberMayClaim) {
    ClaimInputs inputs = eligible();
    inputs.ours = 0;
    EXPECT_FALSE(mayClaim(inputs));
    inputs.ours = 7;
    EXPECT_FALSE(mayClaim(inputs));
}

// The tie-break: two decks that lose the master together claim a second apart,
// lower number first, and back off in the same order after a collision.
TEST(AutoMaster, LowerNumbersClaimFirstAndCollisionsBackOff) {
    EXPECT_EQ(3000, claimDelayMs(1, 0));
    EXPECT_EQ(4000, claimDelayMs(2, 0));
    EXPECT_LT(claimDelayMs(1, 1), claimDelayMs(2, 1));
    EXPECT_EQ(6000, claimDelayMs(1, 1));
    EXPECT_EQ(claimDelayMs(1, kMaxCollisions), claimDelayMs(1, kMaxCollisions + 5));
}

// Decision 13, as a CDJ-2000NXS does it: a stopped master hands master to a
// deck that plays, unless the master is synced and that deck is not. Each
// case of E06's four, and the hardware's (S28 193.655, 208.276).
TEST(AutoMaster, AStoppedMasterHandsOverAsACdjDoes) {
    // E06 D (S28 208.276): master in sync, the deck that plays in sync.
    EXPECT_EQ(4, successorWhenStopped({peer(4, true, true)}, 3, true));
    // E06 A: master not in sync, the deck in sync.
    EXPECT_EQ(4, successorWhenStopped({peer(4, true, true)}, 3, false));
    // E06 C, E01 78.031, E09 10.040: neither in sync.
    EXPECT_EQ(4, successorWhenStopped({peer(4, false, true)}, 3, false));
    // E06 B: master in sync, the deck not: the master keeps it.
    EXPECT_EQ(0, successorWhenStopped({peer(4, false, true)}, 3, true));
    // The lowest-numbered of the decks that qualify.
    EXPECT_EQ(2, successorWhenStopped({peer(4, true, true), peer(2, false, true)}, 3, false));
    EXPECT_EQ(4, successorWhenStopped({peer(4, true, true), peer(2, false, true)}, 3, true));
    // Nobody playing, or nobody still heard: the master keeps it.
    EXPECT_EQ(0, successorWhenStopped({peer(4, true, false)}, 3, false));
    SyncPeer gone = peer(4, true, true);
    gone.statusAgeMs = 5000.0;
    EXPECT_EQ(0, successorWhenStopped({gone}, 3, false));
}

// E12: an NXS playing a plain file sits at play state 3 with its playing flag
// clear, and never takes master. Offered it, it would leave the offer hanging.
TEST(AutoMaster, ADeckWhosePlayingFlagIsClearIsNeverOfferedMaster) {
    SyncPeer plainFile = peer(4, false, true);
    plainFile.playingFlag = false;
    EXPECT_EQ(0, successorWhenStopped({plainFile}, 3, false));
}

// The rule is held while we stay stopped, but an offer nobody takes up is not
// renewed to the same deck: it would cycle every two seconds, and each time
// every listener reading 0x9f would take that deck for the master.
TEST(AutoMaster, AnOfferIsNotRepeatedToTheSameDeckInOneStop) {
    StopOffers offers;
    const std::vector<SyncPeer> one = {peer(4, false, true)};
    EXPECT_EQ(4, offers.next(one, 3, false));
    EXPECT_EQ(0, offers.next(one, 3, false)) << "not taken up: not offered again";
    // A deck that starts later is a new candidate (E06: the rule is held).
    const std::vector<SyncPeer> two = {peer(4, false, true), peer(2, false, true)};
    EXPECT_EQ(2, offers.next(two, 3, false));
    EXPECT_EQ(0, offers.next(two, 3, false));
    offers.reset();
    EXPECT_EQ(2, offers.next(two, 3, false)) << "the next stop starts afresh";
}

// E06 E: a paused master in sync gives master to an unsynced deck that plays
// the moment its own SYNC goes off.
TEST(AutoMaster, OurSyncGoingOffWhileStoppedHandsMasterOn) {
    StopOffers offers;
    const std::vector<SyncPeer> peers = {peer(4, false, true)};
    EXPECT_EQ(0, offers.next(peers, 3, true));
    EXPECT_EQ(4, offers.next(peers, 3, false));
}
