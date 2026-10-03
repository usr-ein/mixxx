#include "widget/deck/decklevels.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <memory>

#include "control/controlaudiotaperpot.h"
#include "test/mixxxtest.h"
#include "util/fpclassify.h"
#include "util/math.h"

using mixxx::deck::DeckLevels;

namespace {

// ---------------------------------------------------------------------------
// The grids. Both levels are only ever set to a step, so whatever the encoder,
// the file or something else hands in has to land on one, inside the range.
// ---------------------------------------------------------------------------

TEST(DeckLevelsSnapTest, OutputLandsOnHalfDecibels) {
    EXPECT_DOUBLE_EQ(0.0, DeckLevels::snapOutputDb(0.2));
    EXPECT_DOUBLE_EQ(0.5, DeckLevels::snapOutputDb(0.3));
    EXPECT_DOUBLE_EQ(-1.5, DeckLevels::snapOutputDb(-1.4));
    EXPECT_DOUBLE_EQ(2.0, DeckLevels::snapOutputDb(2.0));
}

TEST(DeckLevelsSnapTest, OutputStaysInsideTheTrim) {
    EXPECT_DOUBLE_EQ(DeckLevels::kOutputMaxDb, DeckLevels::snapOutputDb(14.0));
    EXPECT_DOUBLE_EQ(DeckLevels::kOutputMinDb, DeckLevels::snapOutputDb(-40.0));
    EXPECT_DOUBLE_EQ(DeckLevels::kOutputMaxDb, DeckLevels::snapOutputDb(1e9));
    EXPECT_DOUBLE_EQ(DeckLevels::kOutputMinDb, DeckLevels::snapOutputDb(-1e9));
}

TEST(DeckLevelsSnapTest, ANanIsUnity) {
    // Nothing that is not a number gets to the gain stage; unity is the safe
    // reading of it. This is the case -ffast-math broke: std::isnan folded to
    // false and the NaN clamped to the top of the range, +6 dB.
    EXPECT_DOUBLE_EQ(0.0, DeckLevels::snapOutputDb(util_double_nan()));
}

TEST(DeckLevelsSnapTest, BrightnessLandsOnFivesAndNeverGoesDark) {
    EXPECT_EQ(80, DeckLevels::snapBrightness(81));
    EXPECT_EQ(85, DeckLevels::snapBrightness(83));
    EXPECT_EQ(DeckLevels::kBrightnessMax, DeckLevels::snapBrightness(140));
    // The floor is what keeps the panel readable enough to turn back up.
    EXPECT_EQ(DeckLevels::kBrightnessMin, DeckLevels::snapBrightness(0));
    EXPECT_EQ(DeckLevels::kBrightnessMin, DeckLevels::snapBrightness(-20));
}

// ---------------------------------------------------------------------------
// Against a real gain control and a backlight made of files, laid out the way
// /sys/class/backlight/<panel>/ is: brightness and max_brightness, numbers on
// a line of their own.
// ---------------------------------------------------------------------------

class DeckLevelsTest : public MixxxTest {
  protected:
    void SetUp() override {
        // The engine's own definition of the main gain, so the trim is checked
        // against the range and taper the deck actually has.
        m_pGain = std::make_unique<ControlAudioTaperPot>(
                ConfigKey(QStringLiteral("[Master]"), QStringLiteral("gain")), -14, 14, 0.5);
        ASSERT_TRUE(QDir(m_backlights.path()).mkpath(QStringLiteral("panel")));
        writePanelFile(QStringLiteral("max_brightness"), 255);
        writePanelFile(QStringLiteral("brightness"), 255);
    }

    std::unique_ptr<DeckLevels> makeLevels() const {
        return std::make_unique<DeckLevels>(m_settings.path(), m_backlights.path());
    }

    void writePanelFile(const QString& name, int value) const {
        QFile file(panelFile(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::number(value) + '\n');
    }

    int rawBrightness() const {
        QFile file(panelFile(QStringLiteral("brightness")));
        if (!file.open(QIODevice::ReadOnly)) {
            return -1;
        }
        return file.readAll().trimmed().toInt();
    }

    QString savedFile() const {
        QFile file(QDir(m_settings.path()).filePath(QStringLiteral("trimixxx-levels")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }

    void writeSavedFile(const QByteArray& contents) const {
        QFile file(QDir(m_settings.path()).filePath(QStringLiteral("trimixxx-levels")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }

    double gainDb() const {
        return ratio2db(m_pGain->get());
    }

    QString panelFile(const QString& name) const {
        return QDir(m_backlights.path()).filePath(QStringLiteral("panel/") + name);
    }

    QTemporaryDir m_settings;
    QTemporaryDir m_backlights;
    std::unique_ptr<ControlAudioTaperPot> m_pGain;
};

TEST_F(DeckLevelsTest, NothingSavedTouchesNothing) {
    // A deck nobody has adjusted keeps the engine's unity and whatever
    // brightness the system restored.
    writePanelFile(QStringLiteral("brightness"), 200);
    auto pLevels = makeLevels();
    EXPECT_DOUBLE_EQ(1.0, m_pGain->get());
    EXPECT_EQ(200, rawBrightness());
    EXPECT_TRUE(pLevels->hasOutput());
    EXPECT_TRUE(pLevels->hasBacklight());
}

TEST_F(DeckLevelsTest, OutputMovesInHalfDecibelsAndStopsAtTheEnds) {
    auto pLevels = makeLevels();
    pLevels->stepOutput(1);
    EXPECT_NEAR(0.5, gainDb(), 1e-9);
    pLevels->stepOutput(-3);
    EXPECT_NEAR(-1.0, gainDb(), 1e-9);
    pLevels->stepOutput(100);
    EXPECT_NEAR(DeckLevels::kOutputMaxDb, gainDb(), 1e-9);
    pLevels->stepOutput(-100);
    EXPECT_NEAR(DeckLevels::kOutputMinDb, gainDb(), 1e-9);
}

TEST_F(DeckLevelsTest, OutputNeverMovesTheWrongWay) {
    // +10 dB is outside the trim, set by something else. Up must not drop it
    // to the top of the range -- that is a turn up making the deck quieter --
    // but down does come back into it.
    m_pGain->set(db2ratio(10.0));
    auto pLevels = makeLevels();
    pLevels->stepOutput(1);
    EXPECT_NEAR(10.0, gainDb(), 1e-9);
    pLevels->stepOutput(-1);
    EXPECT_NEAR(DeckLevels::kOutputMaxDb, gainDb(), 1e-9);
}

TEST_F(DeckLevelsTest, BrightnessIsScaledToThePanel) {
    auto pLevels = makeLevels();
    EXPECT_EQ(100, pLevels->brightness());
    pLevels->stepBrightness(-1);
    EXPECT_EQ(95, pLevels->brightness());
    EXPECT_EQ(242, rawBrightness()); // 95 % of 255
    pLevels->stepBrightness(-100);
    EXPECT_EQ(DeckLevels::kBrightnessMin, pLevels->brightness());
    EXPECT_EQ(26, rawBrightness()); // 10 % of 255, rounded
}

TEST_F(DeckLevelsTest, BrightnessBelowTheFloorIsNotBrightenedByTurningDown) {
    writePanelFile(QStringLiteral("brightness"), 5); // 2 %, from elsewhere
    auto pLevels = makeLevels();
    pLevels->stepBrightness(-1);
    EXPECT_EQ(5, rawBrightness());
    pLevels->stepBrightness(1);
    EXPECT_EQ(DeckLevels::kBrightnessMin, pLevels->brightness());
}

TEST_F(DeckLevelsTest, LevelsSurviveARestart) {
    {
        auto pLevels = makeLevels();
        pLevels->stepOutput(3);
        pLevels->stepBrightness(-4);
        // Destroyed before the save timer fires: the destructor writes it.
    }
    EXPECT_FALSE(savedFile().isEmpty());

    // A fresh start: the engine back at unity, the panel at full.
    m_pGain->set(1.0);
    writePanelFile(QStringLiteral("brightness"), 255);
    auto pLevels = makeLevels();
    EXPECT_NEAR(1.5, gainDb(), 1e-9);
    EXPECT_EQ(80, pLevels->brightness());
    EXPECT_EQ(204, rawBrightness());
}

TEST_F(DeckLevelsTest, OnlyWhatWasSetHereIsKept) {
    {
        auto pLevels = makeLevels();
        pLevels->stepOutput(-2);
    }
    EXPECT_TRUE(savedFile().contains(QStringLiteral("output_db=-1.0")));
    EXPECT_FALSE(savedFile().contains(QStringLiteral("brightness")));

    // So the brightness the system restores next time is left alone, while the
    // output still comes back.
    m_pGain->set(1.0);
    writePanelFile(QStringLiteral("brightness"), 128);
    auto pLevels = makeLevels();
    EXPECT_EQ(128, rawBrightness());
    EXPECT_NEAR(-1.0, gainDb(), 1e-9);
}

TEST_F(DeckLevelsTest, AHandEditedFileIsBroughtIntoRange) {
    writeSavedFile("output_db=40\nbrightness=3\n");
    auto pLevels = makeLevels();
    EXPECT_NEAR(DeckLevels::kOutputMaxDb, gainDb(), 1e-9);
    EXPECT_EQ(DeckLevels::kBrightnessMin, pLevels->brightness());
}

TEST_F(DeckLevelsTest, ValuesThatAreNotNumbersAreIgnored) {
    writeSavedFile("output_db=loud\nbrightness=nan\nnot a line\n=4\n");
    auto pLevels = makeLevels();
    EXPECT_DOUBLE_EQ(1.0, m_pGain->get());
    EXPECT_EQ(255, rawBrightness());
}

TEST_F(DeckLevelsTest, NanAndInfinityInTheFileAreIgnored) {
    // QString reads all three as numbers. "nan" is the one that bit: through
    // a -ffast-math std::isnan it came out of the snap as +6 dB.
    for (const QByteArray& value : {QByteArray("nan"), QByteArray("inf"), QByteArray("-inf")}) {
        writeSavedFile("output_db=" + value + '\n');
        m_pGain->set(1.0);
        auto pLevels = makeLevels();
        EXPECT_DOUBLE_EQ(1.0, m_pGain->get()) << value.constData();
    }
}

TEST_F(DeckLevelsTest, UnityIsSavedWithoutASign) {
    // Up a step from -0.6 dB snaps onto zero from below, which in floating
    // point can be -0.0. The file has to say 0.0 either way.
    m_pGain->set(db2ratio(-0.6));
    {
        auto pLevels = makeLevels();
        pLevels->stepOutput(1);
    }
    EXPECT_NEAR(0.0, gainDb(), 1e-9);
    EXPECT_TRUE(savedFile().contains(QStringLiteral("output_db=0.0\n")));
    EXPECT_FALSE(savedFile().contains(QStringLiteral("-0.0")));
}

TEST_F(DeckLevelsTest, APanelWithoutABacklightHasNoBrightness) {
    QTemporaryDir empty;
    DeckLevels levels(m_settings.path(), empty.path());
    EXPECT_FALSE(levels.hasBacklight());
    EXPECT_EQ(-1, levels.brightness());
    levels.stepBrightness(1); // must not create or write anything
    EXPECT_TRUE(QDir(empty.path()).isEmpty());
}

} // namespace
