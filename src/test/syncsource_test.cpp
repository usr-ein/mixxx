#include "network/prolink/syncsource.h"

#include <gtest/gtest.h>

namespace {

using mixxx::prolink::chooseMeterDeck;
using mixxx::prolink::chooseSyncSource;
using mixxx::prolink::MeterDeck;
using mixxx::prolink::SyncPeer;
using mixxx::prolink::SyncSource;

constexpr int kUs = 3;

/// A CDJ that is playing, heard from just now, beating at *bpm*.
SyncPeer playing(int number, double bpm = 128.0) {
    SyncPeer peer;
    peer.number = number;
    peer.hasStatus = true;
    peer.statusAgeMs = 50.0;
    peer.playing = true;
    peer.beatBpm = bpm;
    peer.beatAgeMs = 100.0;
    peer.statusBpm = bpm;
    peer.barPhase = 0.3;
    peer.barPosition = 0.25;
    return peer;
}

SyncPeer master(SyncPeer peer) {
    peer.isMaster = true;
    return peer;
}

SyncPeer paused(SyncPeer peer) {
    peer.playing = false;
    peer.beatAgeMs = 5000.0;
    return peer;
}

} // namespace

TEST(SyncSource, FollowsAPlayingMasterTempoAndPhase) {
    const SyncSource source = chooseSyncSource({master(playing(2, 126.0))}, kUs);
    EXPECT_EQ(SyncSource::Kind::Master, source.kind);
    EXPECT_EQ(2, source.device);
    EXPECT_DOUBLE_EQ(126.0, source.bpm);
    EXPECT_TRUE(source.phaseLive);
    EXPECT_FALSE(source.masterStopped);
}

// Owner decision 1: a real master always wins, even over a lower-numbered
// deck that is playing.
TEST(SyncSource, TheMasterWinsOverAPlayingDeck) {
    const SyncSource source = chooseSyncSource({playing(1, 120.0), master(playing(4, 130.0))}, kUs);
    EXPECT_EQ(SyncSource::Kind::Master, source.kind);
    EXPECT_EQ(4, source.device);
}

// Owner decision 15: a paused master is not followed, and does not hand SYNC
// to another playing deck either.
TEST(SyncSource, APausedMasterIsReportedStoppedAndNotReplaced) {
    const SyncSource source = chooseSyncSource({playing(1), master(paused(playing(2)))}, kUs);
    EXPECT_EQ(SyncSource::Kind::Master, source.kind);
    EXPECT_EQ(2, source.device);
    EXPECT_TRUE(source.masterStopped);
    EXPECT_LE(source.bpm, 0.0);
}

// Its last status is 30 s old: it has gone, whatever it last said.
TEST(SyncSource, AMasterThatHasGoneSilentIsIgnored) {
    SyncPeer gone = master(playing(2));
    gone.statusAgeMs = 4000.0;
    const SyncSource source = chooseSyncSource({gone, playing(1)}, kUs);
    EXPECT_EQ(SyncSource::Kind::Fallback, source.kind);
    EXPECT_EQ(1, source.device);
}

// Owner decision 1: with no master, the playing deck the meter shows.
TEST(SyncSource, WithNoMasterTheLowestPlayingDeck) {
    const SyncSource source = chooseSyncSource({playing(4), paused(playing(1)), playing(2)}, kUs);
    EXPECT_EQ(SyncSource::Kind::Fallback, source.kind);
    EXPECT_EQ(2, source.device);
}

// Two synced TriMixxx decks and no master must not follow each other: the
// higher-numbered synced one follows the lower, never the other way round.
TEST(SyncSource, NeverFollowsASyncedDeckNumberedAboveUs) {
    SyncPeer other = playing(4);
    other.isSynced = true;
    EXPECT_EQ(SyncSource::Kind::None, chooseSyncSource({other}, kUs).kind);
    SyncPeer lower = playing(2);
    lower.isSynced = true;
    EXPECT_EQ(2, chooseSyncSource({lower}, kUs).device);
}

TEST(SyncSource, NeverOurselvesNorANonPlayer) {
    SyncPeer mixer = playing(33);
    EXPECT_EQ(SyncSource::Kind::None, chooseSyncSource({playing(kUs), mixer}, kUs).kind);
}

// A master handing over is about to stop being master; following it, or
// anyone else, in that moment would be following the wrong deck.
TEST(SyncSource, NothingIsFollowedWhileMasterIsHandedOver) {
    SyncPeer outgoing = master(playing(2));
    outgoing.yieldingTo = 5;
    EXPECT_EQ(SyncSource::Kind::None, chooseSyncSource({outgoing, playing(1)}, kUs).kind);
    SyncPeer successor = master(playing(5, 131.0));
    EXPECT_EQ(5, chooseSyncSource({outgoing, successor}, kUs).device);
}

// A paused deck that played minutes ago still has a beat packet on file; its
// tempo now is what its status says, not what that packet said.
TEST(SyncSource, TempoComesFromStatusWhenTheBeatsAreStale) {
    SyncPeer cdj = master(playing(2, 128.0));
    cdj.beatAgeMs = 60000.0;
    cdj.statusBpm = 133.12;
    const SyncSource source = chooseSyncSource({cdj}, kUs);
    EXPECT_DOUBLE_EQ(133.12, source.bpm);
    EXPECT_FALSE(source.phaseLive);
}

// The next beat is due and has not come: the phase is frozen at the end of the
// beat and must not be measured against.
TEST(SyncSource, AnOverdueBeatIsFlagged) {
    SyncPeer cdj = master(playing(2, 120.0)); // 500 ms a beat
    cdj.beatAgeMs = 498.0;
    EXPECT_TRUE(chooseSyncSource({cdj}, kUs).beatOverdue);
    cdj.beatAgeMs = 200.0;
    EXPECT_FALSE(chooseSyncSource({cdj}, kUs).beatOverdue);
}

// The master is drawn first, paused or not: it is the deck a DJ cueing up
// wants to see. Paused, it is held where its status puts it.
TEST(MeterDeck, TheMasterFirstHeldWhereItStandsWhilePaused) {
    const MeterDeck deck = chooseMeterDeck({playing(1), master(paused(playing(4)))}, kUs);
    EXPECT_EQ(4, deck.device);
    EXPECT_TRUE(deck.isMaster);
    EXPECT_FALSE(deck.live);
    EXPECT_DOUBLE_EQ(0.25, deck.barPhase);
}

TEST(MeterDeck, WithNoMasterAPlayingDeckThenTheLowest) {
    EXPECT_EQ(4, chooseMeterDeck({paused(playing(1)), playing(4)}, kUs).device);
    EXPECT_EQ(2, chooseMeterDeck({playing(4), playing(2)}, kUs).device);
    const MeterDeck cued = chooseMeterDeck({paused(playing(2))}, kUs);
    EXPECT_EQ(2, cued.device);
    EXPECT_FALSE(cued.live);
}

// A deck that has gone keeps its last status, mastership included, until it
// is forgotten some 30 s later.
TEST(MeterDeck, ADeckThatHasGoneSilentIsNotDrawn) {
    SyncPeer gone = master(playing(2));
    gone.statusAgeMs = 4000.0;
    gone.beatAgeMs = 4000.0;
    const MeterDeck deck = chooseMeterDeck({gone, paused(playing(4))}, kUs);
    EXPECT_EQ(4, deck.device);
    EXPECT_FALSE(deck.isMaster);
    EXPECT_EQ(0, chooseMeterDeck({gone}, kUs).device);
}

// Status still says playing, but no beat has come for two beats: the phase
// from beats stands at the end of the last one. Status places the deck.
TEST(MeterDeck, HeldOnceItsBeatsStop) {
    SyncPeer cdj = playing(2, 120.0); // 500 ms a beat
    cdj.beatAgeMs = 1000.0;
    cdj.barPhase = 0.5;
    const MeterDeck deck = chooseMeterDeck({cdj}, kUs);
    EXPECT_FALSE(deck.live);
    EXPECT_DOUBLE_EQ(0.25, deck.barPhase);
}

// Just paused: its last beat is fresh, but status says it has stopped.
TEST(MeterDeck, HeldWhileStatusSaysStopped) {
    SyncPeer cdj = playing(2);
    cdj.playing = false;
    const MeterDeck deck = chooseMeterDeck({cdj}, kUs);
    EXPECT_FALSE(deck.live);
    EXPECT_DOUBLE_EQ(0.25, deck.barPhase);
}

// CUE held moves the playhead, and the meter shows it moving.
TEST(MeterDeck, AuditioningTheCueIsDrawnLive) {
    SyncPeer cdj = playing(2);
    cdj.playing = false;
    cdj.auditioning = true;
    const MeterDeck deck = chooseMeterDeck({cdj}, kUs);
    EXPECT_TRUE(deck.live);
    EXPECT_DOUBLE_EQ(0.3, deck.barPhase);
}

// We are not announcing, so no status reaches us: beats alone.
TEST(MeterDeck, WithoutStatusDrawnWhileItsBeatsArrive) {
    SyncPeer cdj;
    cdj.number = 2;
    cdj.beatBpm = 128.0;
    cdj.beatAgeMs = 100.0;
    cdj.barPhase = 0.3;
    const MeterDeck deck = chooseMeterDeck({cdj}, kUs);
    EXPECT_EQ(2, deck.device);
    EXPECT_TRUE(deck.live);
    cdj.beatAgeMs = 2000.0;
    EXPECT_EQ(0, chooseMeterDeck({cdj}, kUs).device);
}

// Our own beats come back to us, and a mixer beats as a metronome.
TEST(MeterDeck, NeverOurselvesNorANonPlayer) {
    const MeterDeck deck = chooseMeterDeck({master(playing(kUs)), playing(33)}, kUs);
    EXPECT_EQ(0, deck.device);
    EXPECT_LT(deck.barPhase, 0.0);
}
