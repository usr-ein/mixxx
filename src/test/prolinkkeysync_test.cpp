#include "network/prolink/prolinkkeysync.h"

#include <gtest/gtest.h>

#include <QCoreApplication>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "mixer/playerinfo.h"
#include "network/prolink/prolinkcontrols.h"
#include "test/mockedenginebackendtest.h"
#include "track/keyutils.h"
#include "track/track.h"

namespace {

using mixxx::track::io::key::C_MINOR;
using mixxx::track::io::key::ChromaticKey;
using mixxx::track::io::key::D_MINOR;
using mixxx::track::io::key::F_SHARP_MINOR;

/// KEY SYNC against a real deck: its KeyControl, its keylock, and the order in
/// which a load sets the key and then resets the pitch.
class ProLinkKeySyncTest : public MockedEngineBackendTest {
  protected:
    void SetUp() override {
        MockedEngineBackendTest::SetUp();
        m_pControls = std::make_unique<mixxx::prolink::ProLinkControls>();
        m_pKeySync = std::make_unique<mixxx::prolink::ProLinkKeySync>(
                m_sGroup1, m_pControls.get());
        m_pEnabled = std::make_unique<ControlProxy>(
                QStringLiteral("[ProLink]"), QStringLiteral("key_sync_enabled"));
    }

    void TearDown() override {
        m_pEnabled.reset();
        m_pKeySync.reset();
        m_pControls.reset();
        MockedEngineBackendTest::TearDown();
    }

    double pitchAdjust() const {
        return ControlObject::get(ConfigKey(m_sGroup1, "pitch_adjust"));
    }

    void setFileKey(ChromaticKey key) {
        ControlObject::set(ConfigKey(m_sGroup1, "file_key"), static_cast<double>(key));
    }

    /// What BaseTrackPlayerImpl::slotTrackLoaded does, in its order: the key
    /// is set, the pitch is reset (SpeedAutoReset at Mixxx's default), and
    /// only then is the new track announced.
    void loadTrackInKey(ChromaticKey key) {
        setFileKey(key);
        ControlObject::set(ConfigKey(m_sGroup1, "pitch_adjust"), 0.0);
        PlayerInfo::instance().setTrackInfo(m_sGroup1, Track::newTemporary());
        QCoreApplication::processEvents();
        ProcessBuffer();
    }

    static int stepsFrom(ChromaticKey ours, ChromaticKey target) {
        return KeyUtils::shortestStepsToCompatibleKey(ours, target);
    }

    std::unique_ptr<mixxx::prolink::ProLinkControls> m_pControls;
    std::unique_ptr<mixxx::prolink::ProLinkKeySync> m_pKeySync;
    std::unique_ptr<ControlProxy> m_pEnabled;
};

} // namespace

TEST_F(ProLinkKeySyncTest, EngagingShiftsTheDeckAndTurnsKeylockOn) {
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    m_pEnabled->set(1.0);
    QCoreApplication::processEvents();
    ProcessBuffer();
    EXPECT_EQ(1.0, m_pEnabled->get());
    EXPECT_EQ(1.0, ControlObject::get(ConfigKey(m_sGroup1, "keylock")));
    EXPECT_NEAR(stepsFrom(D_MINOR, F_SHARP_MINOR), pitchAdjust(), 1e-9);
}

// A load sets the key and then resets the pitch: the shift has to be applied
// after that, for a different key and for the same one.
TEST_F(ProLinkKeySyncTest, TheShiftSurvivesEveryLoad) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    m_pEnabled->set(1.0);
    QCoreApplication::processEvents();

    loadTrackInKey(C_MINOR);
    EXPECT_NEAR(stepsFrom(C_MINOR, F_SHARP_MINOR), pitchAdjust(), 1e-9);

    loadTrackInKey(C_MINOR);
    EXPECT_NEAR(stepsFrom(C_MINOR, F_SHARP_MINOR), pitchAdjust(), 1e-9);
}

// Owner decision 8: master tempo turned off keeps the shift, and turning it
// back on keeps it too. KeyControl zeroes pitch_adjust both ways.
TEST_F(ProLinkKeySyncTest, TheShiftSurvivesKeylockBothWays) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    m_pEnabled->set(1.0);
    QCoreApplication::processEvents();
    ProcessBuffer();
    const int steps = stepsFrom(D_MINOR, F_SHARP_MINOR);

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ProcessBuffer();
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);
}

// Releasing leaves no offset behind, whatever the tempo fader is doing.
TEST_F(ProLinkKeySyncTest, ReleasingLeavesNoHiddenOffset) {
    ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.06);
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    m_pEnabled->set(1.0);
    QCoreApplication::processEvents();
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ProcessBuffer();

    m_pEnabled->set(0.0);
    QCoreApplication::processEvents();
    ProcessBuffer();
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}

TEST_F(ProLinkKeySyncTest, RefusedWithNothingToSyncTo) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(false, mixxx::track::io::key::INVALID);
    m_pEnabled->set(1.0);
    QCoreApplication::processEvents();
    EXPECT_EQ(0.0, m_pEnabled->get());
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}

// A shell made while the controls hold an engaged latch picks it up, so the
// press that lets go still lets go.
TEST_F(ProLinkKeySyncTest, ARebuiltShellResumesAndCanStillRelease) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    m_pEnabled->set(1.0);
    QCoreApplication::processEvents();
    ASSERT_NE(0.0, pitchAdjust());

    m_pKeySync = std::make_unique<mixxx::prolink::ProLinkKeySync>(
            m_sGroup1, m_pControls.get());
    // Still holding the key: a load is pitched into it.
    loadTrackInKey(C_MINOR);
    EXPECT_NEAR(stepsFrom(C_MINOR, F_SHARP_MINOR), pitchAdjust(), 1e-9);

    m_pEnabled->set(0.0);
    QCoreApplication::processEvents();
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}
