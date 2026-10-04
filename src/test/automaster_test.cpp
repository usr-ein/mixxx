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

// Decision 13: a stopped master hands over to a synced deck that plays on.
TEST(AutoMaster, HandsOverToTheLowestSyncedPlayingDeck) {
    EXPECT_EQ(2, successorWhenStopped({peer(4, true, true), peer(2, true, true)}, 3));
    EXPECT_EQ(0, successorWhenStopped({peer(4, false, true)}, 3));
    EXPECT_EQ(0, successorWhenStopped({peer(4, true, false)}, 3));
    SyncPeer gone = peer(4, true, true);
    gone.statusAgeMs = 5000.0;
    EXPECT_EQ(0, successorWhenStopped({gone}, 3));
}
