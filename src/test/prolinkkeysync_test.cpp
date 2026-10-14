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

using mixxx::prolink::KeySync;
using mixxx::track::io::key::A_MINOR;
using mixxx::track::io::key::C_MINOR;
using mixxx::track::io::key::ChromaticKey;
using mixxx::track::io::key::D_MINOR;
using mixxx::track::io::key::F_SHARP_MINOR;
using mixxx::track::io::key::INVALID;

/// KEY SYNC against a real deck: its KeyControl, its keylock, and the order in
/// which a load sets the key and then announces the track.
class ProLinkKeySyncTest : public MockedEngineBackendTest {
  protected:
    void SetUp() override {
        MockedEngineBackendTest::SetUp();
        m_pControls = std::make_unique<mixxx::prolink::ProLinkControls>();
        m_pKeySync = std::make_unique<mixxx::prolink::ProLinkKeySync>(
                m_sGroup1, m_pControls.get());
        // As the mapping and the key list reach them: through proxies, so
        // their writes are heard as someone else's.
        m_pPress = std::make_unique<ControlProxy>(
                QStringLiteral("[ProLink]"), QStringLiteral("key_sync"));
        m_pShift = std::make_unique<ControlProxy>(
                QStringLiteral("[ProLink]"), QStringLiteral("key_shift"));
        m_pAvailable = std::make_unique<ControlProxy>(
                QStringLiteral("[ProLink]"), QStringLiteral("key_sync_available"));
    }

    void TearDown() override {
        m_pAvailable.reset();
        m_pShift.reset();
        m_pPress.reset();
        m_pKeySync.reset();
        m_pControls.reset();
        MockedEngineBackendTest::TearDown();
    }

    double pitchAdjust() const {
        return ControlObject::get(ConfigKey(m_sGroup1, "pitch_adjust"));
    }

    double keylock() const {
        return ControlObject::get(ConfigKey(m_sGroup1, "keylock"));
    }

    void setFileKey(ChromaticKey key) {
        ControlObject::set(ConfigKey(m_sGroup1, "file_key"), static_cast<double>(key));
        QCoreApplication::processEvents();
    }

    /// What BaseTrackPlayerImpl::slotTrackLoaded does, in its order: the key
    /// is set, and only then is the new track announced. mixxx.cfg's
    /// SpeedAutoReset 0 leaves pitch_adjust alone in between.
    void loadTrackInKey(ChromaticKey key) {
        setFileKey(key);
        PlayerInfo::instance().setTrackInfo(m_sGroup1, Track::newTemporary());
        QCoreApplication::processEvents();
        ProcessBuffer();
    }

    void press() {
        m_pPress->set(1.0);
        QCoreApplication::processEvents();
        ProcessBuffer();
    }

    /// A key picked off the list.
    void pick(double shift) {
        m_pShift->set(shift);
        QCoreApplication::processEvents();
        ProcessBuffer();
    }

    static int shiftOnto(ChromaticKey ours, ChromaticKey master) {
        return KeySync::shiftOntoCamelotNumber(ours, master).value();
    }

    std::unique_ptr<mixxx::prolink::ProLinkControls> m_pControls;
    std::unique_ptr<mixxx::prolink::ProLinkKeySync> m_pKeySync;
    std::unique_ptr<ControlProxy> m_pPress;
    std::unique_ptr<ControlProxy> m_pShift;
    std::unique_ptr<ControlProxy> m_pAvailable;
};

} // namespace

TEST_F(ProLinkKeySyncTest, APressShiftsOntoTheMastersNumberAndTurnsKeylockOn) {
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    press();
    const int steps = shiftOnto(D_MINOR, F_SHARP_MINOR);
    ASSERT_EQ(4, steps);
    EXPECT_EQ(steps, m_pShift->get());
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);
    EXPECT_EQ(1.0, keylock());
    // The key coming out of the deck is the master's.
    EXPECT_EQ(F_SHARP_MINOR,
            KeyUtils::keyFromNumericValue(ControlObject::get(ConfigKey(m_sGroup1, "key"))));
}

TEST_F(ProLinkKeySyncTest, ASecondPressGoesBackToTheOriginal) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    press();
    ASSERT_NE(0.0, pitchAdjust());
    press();
    EXPECT_EQ(0.0, m_pShift->get());
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}

// The network is read at the press and only then.
TEST_F(ProLinkKeySyncTest, TheMasterMovingChangesNothingUntilTheNextPress) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    press();
    const double steps = pitchAdjust();

    m_pKeySync->setLink(true, A_MINOR);
    ProcessBuffer();
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);
    m_pKeySync->setLink(false, INVALID);
    ProcessBuffer();
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);

    // The next press takes what is there now: nothing, so back.
    press();
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}

TEST_F(ProLinkKeySyncTest, APressWithNothingToMatchGoesBackToTheOriginal) {
    setFileKey(D_MINOR);
    pick(3);
    ASSERT_NEAR(3.0, pitchAdjust(), 1e-9);
    m_pKeySync->setLink(false, INVALID);
    press();
    EXPECT_EQ(0.0, m_pShift->get());
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}

// Nothing to match and no shift: the press changes nothing, keylock included.
TEST_F(ProLinkKeySyncTest, APressThatChangesNothingLeavesKeylockAlone) {
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    setFileKey(D_MINOR);
    m_pKeySync->setLink(false, INVALID);
    press();
    EXPECT_EQ(0.0, m_pShift->get());
    EXPECT_EQ(0.0, keylock());
}

TEST_F(ProLinkKeySyncTest, PickingAKeyShiftsTheDeck) {
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    setFileKey(D_MINOR);
    pick(-2);
    EXPECT_NEAR(-2.0, pitchAdjust(), 1e-9);
    EXPECT_EQ(1.0, keylock());
    // Back to the original off the list: no shift, keylock as it is now.
    pick(0);
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
    EXPECT_EQ(1.0, keylock());
}

// Whatever lands is folded into -5..+6 and rounded to a semitone, and the
// control says what was applied.
TEST_F(ProLinkKeySyncTest, APickOutOfRangeIsFolded) {
    setFileKey(D_MINOR);
    pick(9);
    EXPECT_EQ(-3.0, m_pShift->get());
    EXPECT_NEAR(-3.0, pitchAdjust(), 1e-9);
    pick(1.6);
    EXPECT_EQ(2.0, m_pShift->get());
    EXPECT_NEAR(2.0, pitchAdjust(), 1e-9);
}

// Sam: a new track plays in its own key.
TEST_F(ProLinkKeySyncTest, ALoadPlaysTheNewTrackInItsOwnKey) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    press();
    ASSERT_NE(0.0, pitchAdjust());

    loadTrackInKey(C_MINOR);
    EXPECT_EQ(0.0, m_pShift->get());
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
    // And one press matches the master again, from the new track's key.
    press();
    EXPECT_NEAR(shiftOnto(C_MINOR, F_SHARP_MINOR), pitchAdjust(), 1e-9);
}

// Owner decision 8: master tempo turned off keeps the shift, and turning it
// back on keeps it too. KeyControl zeroes pitch_adjust both ways.
TEST_F(ProLinkKeySyncTest, TheShiftSurvivesKeylockBothWays) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    press();
    const int steps = shiftOnto(D_MINOR, F_SHARP_MINOR);

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ProcessBuffer();
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);

    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 1.0);
    ProcessBuffer();
    EXPECT_NEAR(steps, pitchAdjust(), 1e-9);
}

// Going back leaves no offset behind, whatever the tempo fader is doing.
TEST_F(ProLinkKeySyncTest, GoingBackLeavesNoHiddenOffset) {
    ControlObject::set(ConfigKey(m_sGroup1, "rate_ratio"), 1.06);
    setFileKey(D_MINOR);
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    press();
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ProcessBuffer();

    press();
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}

// The pad's dim state: whether a press would change the deck's key.
TEST_F(ProLinkKeySyncTest, AvailableSaysWhetherAPressWouldMoveTheDeck) {
    setFileKey(D_MINOR);
    m_pKeySync->setLink(false, INVALID);
    EXPECT_EQ(0.0, m_pAvailable->get());

    m_pKeySync->setLink(true, F_SHARP_MINOR);
    EXPECT_EQ(1.0, m_pAvailable->get());

    // A master on our number already (F major, 7B, is D minor's relative).
    m_pKeySync->setLink(true, mixxx::track::io::key::F_MAJOR);
    EXPECT_EQ(0.0, m_pAvailable->get());

    // Our own key unknown: no shift goes anywhere in particular.
    m_pKeySync->setLink(true, F_SHARP_MINOR);
    setFileKey(INVALID);
    EXPECT_EQ(0.0, m_pAvailable->get());

    // Shifted with nothing to match: a press goes back.
    setFileKey(D_MINOR);
    m_pKeySync->setLink(false, INVALID);
    pick(3);
    EXPECT_EQ(1.0, m_pAvailable->get());
}

// A new instance made while the controls hold a shift holds it too, so a
// keylock toggle still puts it back and the pad still goes back.
TEST_F(ProLinkKeySyncTest, ARebuiltShellKeepsTheShift) {
    setFileKey(D_MINOR);
    pick(4);
    ASSERT_NEAR(4.0, pitchAdjust(), 1e-9);

    m_pKeySync = std::make_unique<mixxx::prolink::ProLinkKeySync>(
            m_sGroup1, m_pControls.get());
    ControlObject::set(ConfigKey(m_sGroup1, "keylock"), 0.0);
    ProcessBuffer();
    EXPECT_NEAR(4.0, pitchAdjust(), 1e-9);

    press();
    EXPECT_NEAR(0.0, pitchAdjust(), 1e-9);
}
