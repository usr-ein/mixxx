#include "network/prolink/synctempo.h"

#include <gtest/gtest.h>

namespace {

using mixxx::prolink::SyncTempo;

/// One row of the table in `docs/tempo-sync.md`.
///
/// Named by the four switches rather than by a number, so a failure says which
/// state broke rather than which line of a table it was on.
SyncTempo::Source inState(bool weAreMaster, bool ourSync, double masterBpm) {
    SyncTempo::State state;
    state.isMaster = weAreMaster;
    state.syncEnabled = ourSync;
    // Zero when we are master: by the invariant nobody else holds it.
    state.masterBpm = weAreMaster ? 0.0 : masterBpm;
    return SyncTempo::decide(state);
}

constexpr double kCdjTempo = 140.0;

} // namespace

// The eight states. The CDJ's own SYNC never appears below, and that is the
// point: it decides whether the CDJ follows *us*, which is its business. It
// changes nothing about which tempo this deck plays, so rows that differ only
// in that column are the same test written twice — and are written twice, so
// that the table in the document maps one-to-one onto this file.

// --- Row 1: CDJ master, no SYNC anywhere. ----------------------------------
TEST(SyncTempoTable, CdjMasterAndNobodySynced) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(false, false, kCdjTempo));
}

// --- Row 2: CDJ master and synced; we are not. -----------------------------
// The CDJ's SYNC is inert -- it is the master, so there is nobody for it to
// follow -- and ours is off. We are free.
TEST(SyncTempoTable, CdjMasterAndSyncedButWeAreNot) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(false, false, kCdjTempo));
}

// --- Row 3: CDJ master and synced, and we are synced. ----------------------
TEST(SyncTempoTable, CdjMasterAndWeFollowIt) {
    EXPECT_EQ(SyncTempo::Source::Master, inState(false, true, kCdjTempo));
}

// --- Row 4: CDJ master and NOT synced, and we are synced. ------------------
//
// Absent from the original table, and the most common live case: the other deck
// is simply playing and we beat-match to it. Being master is what matters, not
// whether the master's own SYNC is lit, so this is row 3 exactly.
TEST(SyncTempoTable, CdjMasterUnsyncedAndWeFollowIt) {
    EXPECT_EQ(SyncTempo::Source::Master, inState(false, true, kCdjTempo));
}

// --- Row 5: we are master and synced; CDJ not synced. ----------------------
// Our SYNC is inert. The fader leads.
TEST(SyncTempoTable, WeAreMasterAndSyncedWithNobodyFollowing) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(true, true, 0.0));
}

// --- Row 6: we are master, no SYNC anywhere. -------------------------------
TEST(SyncTempoTable, WeAreMasterAndNobodySynced) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(true, false, 0.0));
}

// --- Row 7: we are master; the CDJ is synced and follows us. ---------------
TEST(SyncTempoTable, WeAreMasterAndTheCdjFollowsUs) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(true, false, 0.0));
}

// --- Row 8: we are master and synced; the CDJ follows us. ------------------
// Row 7 with our SYNC flag also lit. Inert, and published all the same, because
// a CDJ lights its own SYNC button from that bit.
TEST(SyncTempoTable, WeAreMasterAndSyncedAndTheCdjFollowsUs) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(true, true, 0.0));
}

// --- The two states the table does not have a row for. ---------------------

/// Nobody is master: the seconds after power-on, before anyone has claimed.
///
/// SYNC is lit and there is nothing to follow. The fader leads, because a deck
/// whose fader does nothing on account of a master that does not exist is the
/// worst possible reading of the rule.
TEST(SyncTempoTable, SyncedWithNoMasterAtAllLeavesTheFaderInCharge) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(false, true, 0.0));
}

/// The master is stopped, or has no track, and publishes the no-tempo
/// sentinel. Following a zero would drag this deck to a standstill.
TEST(SyncTempoTable, AMasterWithNoTempoIsNotFollowed) {
    EXPECT_EQ(SyncTempo::Source::Fader, inState(false, true, 0.0));
}
