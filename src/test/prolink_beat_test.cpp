#include <gtest/gtest.h>

#include <QByteArray>

#include "network/prolink/prolinkbeatlistener.h"

namespace {

using mixxx::prolink::BeatPhase;
using mixxx::prolink::parseBeatPacket;

QByteArray hex(const char* text) {
    return QByteArray::fromHex(QByteArray(text));
}

/// A real 96-byte beat packet from a CDJ-2000NXS, out of
/// captures/S06-load-and-play. Player 1, on the downbeat, playing a 132.01 BPM
/// track with the pitch fader at about -3%.
const char* kRealBeatPacket =
        "5173707431576d4a4f4c2843444a2d3230303"
        "06e6578757300000000000000010001003c000001c60000038d0000071a"
        "0000071a00000e3400000e34ffffffffffffffffffffffffffffffffffff"
        "ffffffffffff000f83120000339101000001";

TEST(ProLinkBeatTest, ParsesARealCdjBeatPacket) {
    BeatPhase phase;
    ASSERT_TRUE(parseBeatPacket(hex(kRealBeatPacket), &phase));
    EXPECT_EQ(1, phase.deviceNumber);
    EXPECT_EQ(1, phase.beatInBar);

    // **The tempo has to have the pitch applied.** The packet carries the
    // track's 132.01 and a separate pitch multiplier, and its timing fields are
    // stated as if the fader were at zero. Using the raw BPM would put the phase
    // estimate 3% out, which at 128 BPM is a whole beat adrift inside forty
    // beats -- a phase meter that slowly lies.
    EXPECT_NEAR(127.98, phase.bpm, 0.01);
}

TEST(ProLinkBeatTest, PhaseAdvancesAcrossTheBeatAndClampsAtItsEnd) {
    BeatPhase phase;
    ASSERT_TRUE(parseBeatPacket(hex(kRealBeatPacket), &phase));
    // One beat at 127.98 BPM is 468.8 ms.
    EXPECT_NEAR(0.0, phase.beatPhase(), 0.001);
    phase.sinceBeatMs = 234;
    EXPECT_NEAR(0.5, phase.beatPhase(), 0.01);

    // Past the beat the phase stops rather than wrapping: beat packets stop
    // arriving the moment a platter does, so a marker that kept spinning would
    // report a paused deck as playing.
    phase.sinceBeatMs = 5000;
    EXPECT_DOUBLE_EQ(1.0, phase.beatPhase());
}

/// Beat 1 of the bar starts at 0.0 of the bar, beat 3 halfway through it.
TEST(ProLinkBeatTest, BarPhaseCountsFromTheDownbeat) {
    BeatPhase phase;
    ASSERT_TRUE(parseBeatPacket(hex(kRealBeatPacket), &phase));
    EXPECT_NEAR(0.0, phase.barPhase(), 0.001);

    phase.beatInBar = 3;
    EXPECT_NEAR(0.5, phase.barPhase(), 0.001);
    phase.sinceBeatMs = 234;
    EXPECT_NEAR(0.625, phase.barPhase(), 0.01);
}

TEST(ProLinkBeatTest, RejectsThingsThatAreNotBeatPackets) {
    BeatPhase phase;
    EXPECT_FALSE(parseBeatPacket(QByteArray(), &phase));
    EXPECT_FALSE(parseBeatPacket(QByteArray(96, '\0'), &phase));
    // A keep-alive: right magic, wrong type byte.
    EXPECT_FALSE(parseBeatPacket(
            hex("5173707431576d4a4f4c060043444a2d323030306e657875730000"
                "0000000000010200360502a0cec8e226dea9fe6364010000000100"),
            &phase));
    // Right shape but truncated: a beat packet is 0x60 bytes and the fields we
    // need are at the far end of it.
    EXPECT_FALSE(parseBeatPacket(hex(kRealBeatPacket).left(0x40), &phase));
}

} // namespace
