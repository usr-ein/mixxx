#include "network/prolink/audiblebeatclock.h"

#include <gtest/gtest.h>

#include "control/controlobject.h"
#include "mixer/playerinfo.h"
#include "test/mockedenginebackendtest.h"
#include "track/track.h"

namespace {

/// 120 BPM at 44.1 kHz from the first frame: a beat every 22050 frames.
constexpr double kBeatFrames = 22050.0;
/// loadFakeTrack() builds a ten-second track.
constexpr double kTrackFrames = 441000.0;

class AudibleBeatClockTest : public MockedEngineBackendTest {
  protected:
    void SetUp() override {
        MockedEngineBackendTest::SetUp();
        m_pTrack1->trySetBeats(mixxx::Beats::fromConstTempo(
                m_pTrack1->getSampleRate(), mixxx::audio::kStartFramePos, mixxx::Bpm(120)));
        PlayerInfo::instance().setTrackInfo(m_sGroup1, m_pTrack1);
        ControlObject::set(ConfigKey(m_sGroup1, "duration"), 10.0);
        ProcessBuffer();
    }

    /// Paused at *frame*, so the audible position is the engine's.
    void standAt(double frame) {
        ControlObject::set(ConfigKey(m_sGroup1, "playposition"), frame / kTrackFrames);
        ProcessBuffer();
    }

    void introAtBeat(int beat) {
        // Engine sample positions are frames times two.
        ControlObject::set(ConfigKey(m_sGroup1, "intro_start_position"),
                beat * kBeatFrames * 2.0);
    }
};

} // namespace

TEST_F(AudibleBeatClockTest, CountsBeatsOnTheGridFromTheFirstBeat) {
    mixxx::prolink::AudibleBeatClock clock(m_sGroup1, nullptr);
    standAt(5.5 * kBeatFrames);
    const auto position = clock.now();
    EXPECT_EQ(6u, position.number);
    EXPECT_NEAR(0.5, position.fraction, 1e-3);
}

// Owner decision 14: the bar starts on rekordbox's downbeat, the intro cue.
// Beat 5 is three beats after a downbeat on beat 2.
TEST_F(AudibleBeatClockTest, TheBarStartsOnTheIntroCue) {
    mixxx::prolink::AudibleBeatClock clock(m_sGroup1, nullptr);
    introAtBeat(2);
    standAt(5.5 * kBeatFrames);
    EXPECT_NEAR((3 + 0.5) / 4.0, mixxx::prolink::barPhaseOf(clock.now()), 1e-3);
    standAt(2.25 * kBeatFrames);
    EXPECT_NEAR(0.25 / 4.0, mixxx::prolink::barPhaseOf(clock.now()), 1e-3);
}
