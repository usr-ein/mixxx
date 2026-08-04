#include "network/prolink/synctempo.h"

#include <gtest/gtest.h>

#include <QtDebug>

namespace {

using mixxx::prolink::SyncTempo;

/// The loaded track's own tempo. Every fader position below is a percentage of
/// this, exactly as the wire carries it: byte 0x92 of a status packet is the
/// track's tempo and 0x8c is the fader, and a follower multiplies the two.
constexpr double kFileBpm = 130.0;

/// The fader position, in percent, that would play *bpm* on this track.
double faderFor(double bpm) {
    return (bpm / kFileBpm - 1.0) * 100.0;
}

/// What the status packet would carry: the track's tempo and the fader, which
/// is what every other player multiplies together to get our tempo.
///
/// Asserted rather than the effective tempo alone, because the pair is what
/// goes out and a deck whose two halves disagree is the failure this whole area
/// keeps producing.
struct OnTheWire {
    double bpm;
    double pitchPercent;
};

OnTheWire wire(const SyncTempo::Output& out) {
    if (out.effectiveBpm <= 0.0) {
        return {0.0, 0.0};
    }
    return {kFileBpm, (out.effectiveBpm / kFileBpm - 1.0) * 100.0};
}

/// A deck with a track loaded, its fader at the position that plays *bpm*.
SyncTempo::Inputs deck(double faderBpm) {
    SyncTempo::Inputs in;
    in.fileBpm = kFileBpm;
    in.faderPercent = faderFor(faderBpm);
    return in;
}

MATCHER_P2(PlaysAt, expected, tolerance, "") {
    return std::abs(arg - expected) <= tolerance;
}

} // namespace

/// The whole of the tempo hand-over, as one continuous session.
///
/// Written as a single test on purpose: every step depends on the state the
/// previous one left behind, and splitting it into independent cases would test
/// the transitions without testing that they compose -- which is exactly where
/// this is hard to get right.
TEST(SyncTempoTest, TheFaderHasToCatchUpBeforeItTakesOver) {
    SyncTempo tempo;

    // --- The CDJ is master and we are following it. -----------------------
    //
    // Our fader sits at 130 and stays there for the whole of this section: the
    // DJ is not touching it, and it is not connected to anything.
    SyncTempo::Inputs in = deck(130.0);
    in.syncEnabled = true;
    in.isMaster = false;
    in.masterBpm = 130.0;
    SyncTempo::Output out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(130.0, 0.001));
    EXPECT_FALSE(out.faderInControl) << "a follower's fader is decoupled";
    EXPECT_THAT(wire(out).pitchPercent, PlaysAt(0.0, 0.001));

    // The CDJ runs its tempo up to 140. We follow it there, and the wire says
    // so: the track is still a 130 BPM track, played 7.7% fast.
    in.masterBpm = 140.0;
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(140.0, 0.001));
    EXPECT_THAT(wire(out).bpm, PlaysAt(130.0, 0.001));
    EXPECT_THAT(wire(out).pitchPercent, PlaysAt(7.6923, 0.001));

    // --- We take master. --------------------------------------------------
    //
    // The tempo stays at 140. The fader is still physically at 130, which is
    // *below* what is playing, so it will have to come up to meet it.
    in.isMaster = true;
    in.masterBpm = 0.0; // nobody else holds it now
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(140.0, 0.001))
            << "taking master must not drop the tempo to wherever the fader is";
    EXPECT_FALSE(out.faderInControl);

    // Moving the fader up to 133 does nothing at all: still short of 140.
    in.faderPercent = faderFor(133.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(140.0, 0.001));
    EXPECT_FALSE(out.faderInControl);
    EXPECT_THAT(wire(out).pitchPercent, PlaysAt(7.6923, 0.001))
            << "nothing changed on the wire, so no follower moved";

    // ...and at 139.9 it still does nothing. The catch is a crossing, not a
    // proximity.
    in.faderPercent = faderFor(139.9);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(140.0, 0.001));
    EXPECT_FALSE(out.faderInControl);

    // Reaching 140 catches it. From here the fader *is* the tempo.
    in.faderPercent = faderFor(140.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(140.0, 0.001));
    EXPECT_TRUE(out.faderInControl) << "the fader has met the tempo";

    // So 143 now goes out, and every synced deck follows it.
    in.faderPercent = faderFor(143.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(143.0, 0.001));
    EXPECT_THAT(wire(out).pitchPercent, PlaysAt(10.0, 0.001));

    // And so does 120, in the other direction: once caught, the fader stays
    // caught however far it travels.
    in.faderPercent = faderFor(120.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(120.0, 0.001));
    EXPECT_TRUE(out.faderInControl);
    EXPECT_THAT(wire(out).pitchPercent, PlaysAt(-7.6923, 0.001));

    // --- The CDJ takes master back, at 120. -------------------------------
    //
    // It had been left at 140, so from its point of view the fader is now
    // *above* the tempo and has to come down. This deck follows.
    in.isMaster = false;
    in.masterBpm = 120.0;
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(120.0, 0.001));
    EXPECT_FALSE(out.faderInControl);
}

/// The mirror image: a fader left above the tempo catches coming down.
///
/// The same crossing rule, and worth its own test because an implementation
/// that only ever checks "fader >= tempo" passes the scenario above and leaves
/// this deck's fader permanently dead.
TEST(SyncTempoTest, AFaderLeftAboveTheTempoCatchesOnTheWayDown) {
    SyncTempo tempo;
    SyncTempo::Inputs in = deck(140.0);
    in.syncEnabled = true;
    in.masterBpm = 120.0;

    // Following at 120 with the fader up at 140.
    SyncTempo::Output out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(120.0, 0.001));

    // Take master: still 120, fader above.
    in.isMaster = true;
    in.masterBpm = 0.0;
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(120.0, 0.001));
    EXPECT_FALSE(out.faderInControl);

    // Coming down through 130 changes nothing.
    in.faderPercent = faderFor(130.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(120.0, 0.001));
    EXPECT_FALSE(out.faderInControl);

    // Reaching 120 catches, and below it the fader leads.
    in.faderPercent = faderFor(120.0);
    out = tempo.update(in);
    EXPECT_TRUE(out.faderInControl);
    in.faderPercent = faderFor(112.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(112.0, 0.001));
}

TEST(SyncTempoTest, WithoutSyncTheFaderIsAlwaysInCharge) {
    // Nothing to catch up to: no master has ever driven this deck, so the fader
    // is simply the tempo from the first update.
    SyncTempo tempo;
    SyncTempo::Inputs in = deck(124.0);
    const SyncTempo::Output out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(124.0, 0.001));
    EXPECT_TRUE(out.faderInControl);
}

TEST(SyncTempoTest, SyncedWithNobodyMasterLeavesTheFaderInCharge) {
    // SYNC is on but there is no master to follow -- every deck stopped, say.
    // There is nothing to decouple the fader from, so it keeps working; a deck
    // whose fader does nothing because of a master that does not exist is the
    // worst possible reading of this rule.
    SyncTempo tempo;
    SyncTempo::Inputs in = deck(128.0);
    in.syncEnabled = true;
    in.masterBpm = 0.0;
    const SyncTempo::Output out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(128.0, 0.001));
    EXPECT_TRUE(out.faderInControl);
}

TEST(SyncTempoTest, AnEmptyDeckHasNoTempoAndNothingToHandOver) {
    SyncTempo tempo;
    SyncTempo::Inputs in = deck(130.0);
    in.syncEnabled = true;
    in.masterBpm = 140.0;
    tempo.update(in);

    // The track is ejected. Nothing is published, and the next track starts
    // with the fader live rather than needing to catch a tempo from a track
    // that is no longer loaded.
    in.fileBpm = 0.0;
    SyncTempo::Output out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(0.0, 0.001));
    EXPECT_THAT(wire(out).bpm, PlaysAt(0.0, 0.001));

    in = deck(126.0);
    out = tempo.update(in);
    EXPECT_THAT(out.effectiveBpm, PlaysAt(126.0, 0.001));
    EXPECT_TRUE(out.faderInControl);
}
