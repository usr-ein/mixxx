#include "network/prolink/keysync.h"

#include <gtest/gtest.h>

namespace {

using mixxx::prolink::KeySync;
namespace key = mixxx::track::io::key;

KeySync::Link offering(key::ChromaticKey masterKey) {
    KeySync::Link link;
    link.otherIsMaster = true;
    link.masterKey = masterKey;
    return link;
}

/// A master with a key we cannot resolve: it is playing an unanalysed track,
/// or one off a stick this deck has never read.
KeySync::Link masterWithNoKey() {
    return offering(key::INVALID);
}

/// Nobody else holds tempo master -- we do, or nobody does.
KeySync::Link noMaster() {
    KeySync::Link link;
    link.otherIsMaster = false;
    link.masterKey = key::A_MINOR;
    return link;
}

} // namespace

// --- Rule 1: it can only be engaged against a master. ----------------------

TEST(KeySync, RefusesWithoutAMaster) {
    KeySync sync;
    EXPECT_FALSE(KeySync::canEngage(noMaster()));
    EXPECT_FALSE(sync.engage(noMaster()));
    EXPECT_FALSE(sync.engaged());
    EXPECT_EQ(key::INVALID, sync.target());
}

// A master we can see but cannot read a key off is no more use than none: the
// deck would light the button and pitch nothing.
TEST(KeySync, RefusesWhenTheMastersKeyIsUnknown) {
    KeySync sync;
    EXPECT_FALSE(KeySync::canEngage(masterWithNoKey()));
    EXPECT_FALSE(sync.engage(masterWithNoKey()));
    EXPECT_FALSE(sync.engaged());
}

TEST(KeySync, EngagesOnAMasterWithAKey) {
    KeySync sync;
    EXPECT_TRUE(KeySync::canEngage(offering(key::A_MINOR)));
    EXPECT_TRUE(sync.engage(offering(key::A_MINOR)));
    EXPECT_TRUE(sync.engaged());
    EXPECT_EQ(key::A_MINOR, sync.target());
}

// --- Rule 2: engaging latches the key. ------------------------------------

TEST(KeySync, KeepsItsKeyWhenTheMasterLoadsSomethingElse) {
    KeySync sync;
    ASSERT_TRUE(sync.engage(offering(key::A_MINOR)));
    sync.engage(offering(key::F_MAJOR));
    EXPECT_EQ(key::A_MINOR, sync.target());
}

// The one the deck's UI is about: master moves from one CDJ to another, both
// with tracks, and this deck does not move with it.
TEST(KeySync, KeepsItsKeyWhenMasterMovesToAnotherDeck) {
    KeySync sync;
    ASSERT_TRUE(sync.engage(offering(key::A_MINOR)));
    KeySync::Link other;
    other.otherIsMaster = true;
    other.masterKey = key::D_MINOR;
    sync.engage(other);
    EXPECT_TRUE(sync.engaged());
    EXPECT_EQ(key::A_MINOR, sync.target());
}

// --- Rule 3: it never releases itself. ------------------------------------

TEST(KeySync, StaysEngagedWhenTheNetworkGoesAway) {
    KeySync sync;
    ASSERT_TRUE(sync.engage(offering(key::A_MINOR)));
    // Every way the offer can lapse: the master hands over to us, its track
    // comes off, the CDJ is unplugged. None of them is this deck's business
    // once it is holding a key.
    EXPECT_FALSE(KeySync::canEngage(noMaster()));
    EXPECT_TRUE(sync.engaged());
    EXPECT_EQ(key::A_MINOR, sync.target());
}

// Pressing the button while the offer has lapsed still turns it OFF. This is
// the asymmetry the deck's pad depends on: dark and dead when it is off, but
// always releasable when it is on.
TEST(KeySync, ReleasesEvenWithNothingOnTheNetwork) {
    KeySync sync;
    ASSERT_TRUE(sync.engage(offering(key::A_MINOR)));
    sync.release();
    EXPECT_FALSE(sync.engaged());
    EXPECT_EQ(key::INVALID, sync.target());
}

// --- Rule 4: re-latching takes a release first. ---------------------------

TEST(KeySync, TakesANewKeyOnlyAfterARelease) {
    KeySync sync;
    ASSERT_TRUE(sync.engage(offering(key::A_MINOR)));
    sync.release();
    ASSERT_TRUE(sync.engage(offering(key::D_MINOR)));
    EXPECT_EQ(key::D_MINOR, sync.target());
}

// Released with nothing on the network, it cannot be brought back -- which is
// what makes the pad go dark rather than merely unlit.
TEST(KeySync, CannotBeBroughtBackOnceReleasedWithNoMaster) {
    KeySync sync;
    ASSERT_TRUE(sync.engage(offering(key::A_MINOR)));
    sync.release();
    EXPECT_FALSE(sync.engage(noMaster()));
    EXPECT_FALSE(sync.engaged());
}
