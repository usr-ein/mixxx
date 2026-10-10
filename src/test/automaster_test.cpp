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

namespace {

/// The session, as far as offers go: it refuses one while another is in
/// flight, until that one is taken up or withdrawn (VirtualCdj).
struct FakeSession {
    int inFlight = 0;
    bool offer(int deck) {
        if (inFlight != 0) {
            return false;
        }
        inFlight = deck;
        return true;
    }
    /// Nobody took it up: KeepIt withdraws it after two seconds.
    void withdraw() {
        inFlight = 0;
    }
};

} // namespace

// The rule is held while we stay stopped, but an offer nobody takes up is not
// renewed to the same deck: it would cycle every two seconds, and each time
// every listener reading 0x9f would take that deck for the master.
TEST(AutoMaster, AnOfferIsNotRepeatedToTheSameDeckInOneStop) {
    StopOffers offers;
    FakeSession session;
    const auto offer = [&session](int deck) { return session.offer(deck); };
    const std::vector<SyncPeer> one = {peer(4, false, true)};
    EXPECT_EQ(4, offers.offerNext(one, 3, false, offer));
    session.withdraw();
    EXPECT_EQ(0, offers.offerNext(one, 3, false, offer)) << "not taken up: not offered again";
    // A deck that starts later is a new candidate (E06: the rule is held).
    const std::vector<SyncPeer> two = {peer(4, false, true), peer(2, false, true)};
    EXPECT_EQ(2, offers.offerNext(two, 3, false, offer));
    session.withdraw();
    EXPECT_EQ(0, offers.offerNext(two, 3, false, offer));
    offers.reset();
    EXPECT_EQ(2, offers.offerNext(two, 3, false, offer)) << "the next stop starts afresh";
}

// Two decks that qualify from the stop: one is named, and nobody else while
// its offer is in flight. The poll runs every 33 ms; naming the second over
// the first could leave both claiming master (they claim 12-69 ms after being
// named, S28).
TEST(AutoMaster, TwoDecksQualifyingAtTheStopOnlyOneIsNamed) {
    StopOffers offers;
    FakeSession session;
    const auto offer = [&session](int deck) { return session.offer(deck); };
    const std::vector<SyncPeer> two = {peer(2, true, true), peer(4, true, true)};
    EXPECT_EQ(2, offers.offerNext(two, 3, false, offer)) << "the lowest-numbered";
    for (int poll = 0; poll < 60; ++poll) {
        EXPECT_EQ(0, offers.offerNext(two, 3, false, offer)) << "two seconds of polls";
    }
    EXPECT_EQ(2, session.inFlight) << "0x9f still names the first";
    // Withdrawn after two seconds, untaken: the next deck may have it.
    session.withdraw();
    EXPECT_EQ(4, offers.offerNext(two, 3, false, offer));
    EXPECT_EQ(4, session.inFlight);
}

// A deck counts as offered only once the session took the offer: one the
// session refused is still a candidate.
TEST(AutoMaster, AnOfferTheSessionRefusedIsNotCounted) {
    StopOffers offers;
    bool accepts = false;
    const auto offer = [&accepts](int) { return accepts; };
    const std::vector<SyncPeer> one = {peer(4, false, true)};
    EXPECT_EQ(0, offers.offerNext(one, 3, false, offer));
    EXPECT_EQ(4, offers.candidate(one, 3, false)) << "still a candidate";
    accepts = true;
    EXPECT_EQ(4, offers.offerNext(one, 3, false, offer));
    EXPECT_EQ(0, offers.candidate(one, 3, false));
}

// E06 E: a paused master in sync gives master to an unsynced deck that plays
// the moment its own SYNC goes off.
TEST(AutoMaster, OurSyncGoingOffWhileStoppedHandsMasterOn) {
    StopOffers offers;
    FakeSession session;
    const auto offer = [&session](int deck) { return session.offer(deck); };
    const std::vector<SyncPeer> peers = {peer(4, false, true)};
    EXPECT_EQ(0, offers.offerNext(peers, 3, true, offer));
    EXPECT_EQ(4, offers.offerNext(peers, 3, false, offer));
}

// "Nobody is master" is known only once every player has been heard: a peer
// sends status only after hearing our keep-alive, and a stale claim is not
// believed.
TEST(AutoMaster, NobodyIsMasterOnlyOnceEveryPlayerHasBeenHeard) {
    EXPECT_TRUE(everyPlayerHeard({peer(1, false, false), peer(2, false, true)}, {1, 2, 4}, 4));
    SyncPeer stale = peer(2, false, true);
    stale.statusAgeMs = 1500.0;
    EXPECT_FALSE(everyPlayerHeard({peer(1, false, false), stale}, {1, 2, 4}, 4));
    EXPECT_FALSE(everyPlayerHeard({peer(1, false, false)}, {1, 2}, 4))
            << "player 2 is in the device table and has sent no status yet";
    EXPECT_TRUE(everyPlayerHeard({}, {}, 4)) << "alone on the link";
}

// The first deck to play takes master as it starts (S28 22.058, E01 23.269).
// Decision 2's delay is for a master lost while we play, and for a collision.
TEST(AutoMaster, TheFirstDeckToPlayClaimsAtOnce) {
    EXPECT_TRUE(claimsAtOnce(true, true, 0));
    EXPECT_FALSE(claimsAtOnce(false, true, 0)) << "already playing: decision 2's delay";
    EXPECT_FALSE(claimsAtOnce(true, false, 0)) << "a player not heard yet may be master";
    EXPECT_FALSE(claimsAtOnce(true, true, 1)) << "after a collision the delay orders the decks";
}
