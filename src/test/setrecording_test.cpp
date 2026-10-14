#include "library/deck/setrecording.h"

#include <gtest/gtest.h>

namespace {

using namespace mixxx::deck::setrecording;

constexpr qint64 kSecond = 1000;
constexpr qint64 kMinute = 60 * kSecond;

TEST(SetRecordingTest, SamsFileNamesShareTheirDigits) {
    const QString digits = digitsOf(4207);
    EXPECT_EQ(QStringLiteral("00004207"), digits);
    EXPECT_EQ(QStringLiteral("recorded-set-00004207.mp3"), audioFileName(digits));
    EXPECT_EQ(QStringLiteral("recorded-set-track-list-00004207.txt"), trackListFileName(digits));
    // Any number gives eight digits, never nine.
    EXPECT_EQ(QStringLiteral("94967295"), digitsOf(4294967295u));
}

/// Three hours at 320 kbit/s: the room a recording may need on the card.
TEST(SetRecordingTest, ThreeHoursIsFourHundredAndThirtyTwoMegabytes) {
    EXPECT_EQ(432000000, bytesFor(kMaxLengthMs));
    EXPECT_EQ(40000, bytesFor(kSecond));
}

TEST(SetRecordingTest, TimesIntoARecording) {
    EXPECT_EQ(QStringLiteral("00:00"), offsetText(0, false));
    EXPECT_EQ(QStringLiteral("03:12"), offsetText(3 * kMinute + 12 * kSecond + 999, false));
    EXPECT_EQ(QStringLiteral("0:03:12"), offsetText(3 * kMinute + 12 * kSecond, true));
    EXPECT_EQ(QStringLiteral("2:59:59"), offsetText(kMaxLengthMs - 1, true));
    EXPECT_EQ(QStringLiteral("0:42"), lengthText(42 * kSecond));
    EXPECT_EQ(QStringLiteral("12:34"), lengthText(12 * kMinute + 34 * kSecond));
    EXPECT_EQ(QStringLiteral("1:02:13"), lengthText(62 * kMinute + 13 * kSecond));
    EXPECT_EQ(QStringLiteral("3:00:00"), lengthText(kMaxLengthMs));
}

TEST(SetRecordingTest, Sizes) {
    EXPECT_EQ(QStringLiteral("433 MB"), sizeText(bytesFor(kMaxLengthMs) + 1024 * 1024 + 64 * 1024));
    EXPECT_EQ(QStringLiteral("0 MB"), sizeText(0));
    EXPECT_EQ(QStringLiteral("12.3 GB"), sizeText(12300000000LL));
    EXPECT_EQ(QStringLiteral("1.0 GB"), sizeText(1000000000LL));
}

TEST(SetRecordingTest, TheTrackList) {
    const QList<TrackListLine> lines = {
            {0, QStringLiteral("Dead Grid"), QStringLiteral("My So-Called Life")},
            {3 * kMinute + 12 * kSecond, QString(), QStringLiteral("Untitled 4")},
            {5 * kMinute + 40 * kSecond, QStringLiteral("Someone"), QString()},
    };
    EXPECT_EQ(QStringLiteral("Recorded set 12345678 · 9:05\n"
                             "\n"
                             "00:00  Dead Grid - My So-Called Life\n"
                             "03:12  Untitled 4\n"
                             "05:40  Unknown track\n"),
            trackListText(QStringLiteral("12345678"), 9 * kMinute + 5 * kSecond, lines));
}

/// An hour or more: every time with its hours, so they line up.
TEST(SetRecordingTest, ATrackListOfHours) {
    const QList<TrackListLine> lines = {
            {0, QStringLiteral("A"), QStringLiteral("One")},
            {75 * kMinute, QStringLiteral("B"), QStringLiteral("Two")},
    };
    EXPECT_EQ(QStringLiteral("Recorded set 00000001 · 1:30:00\n"
                             "\n"
                             "0:00:00  A - One\n"
                             "1:15:00  B - Two\n"),
            trackListText(QStringLiteral("00000001"), 90 * kMinute, lines));
}

TEST(SetRecordingTest, AnEmptyTrackListSaysSo) {
    EXPECT_EQ(QStringLiteral("Recorded set 00000001 · 0:30\n"
                             "\n"
                             "Nothing played on the link while it recorded.\n"),
            trackListText(QStringLiteral("00000001"), 30 * kSecond, {}));
}

TEST(SetRecordingTest, Levels) {
    EXPECT_DOUBLE_EQ(0.0, dbfs(1.0f));
    EXPECT_NEAR(-6.02, dbfs(0.5f), 0.01);
    EXPECT_DOUBLE_EQ(kSilenceDb, dbfs(0.0f));
    EXPECT_DOUBLE_EQ(0.0, meterPosition(dbfs(0.0f)));
    EXPECT_DOUBLE_EQ(0.0, meterPosition(-80.0));
    EXPECT_DOUBLE_EQ(0.5, meterPosition(-30.0));
    EXPECT_DOUBLE_EQ(1.0, meterPosition(0.0));
    EXPECT_DOUBLE_EQ(1.0, meterPosition(3.0));
}

/// Five minutes of nothing, then the question; sound takes it back.
TEST(SetRecordingSilenceTest, AsksAfterFiveSilentMinutes) {
    SilenceWatch watch;
    watch.hear(0, -20.0);
    EXPECT_FALSE(watch.asking(0));
    watch.hear(10 * kSecond, -70.0);
    EXPECT_FALSE(watch.asking(5 * kMinute));
    watch.hear(5 * kMinute + 10 * kSecond, -78.0);
    EXPECT_TRUE(watch.asking(5 * kMinute + 10 * kSecond));
    // The music comes back: no question any more.
    watch.hear(6 * kMinute, -13.0);
    EXPECT_FALSE(watch.asking(6 * kMinute));
}

/// A click in the silence starts it again: five minutes from there.
TEST(SetRecordingSilenceTest, SoundStartsTheSilenceAgain) {
    SilenceWatch watch;
    watch.hear(0, -90.0);
    watch.hear(4 * kMinute, -10.0);
    watch.hear(4 * kMinute + 1, -90.0);
    EXPECT_FALSE(watch.asking(5 * kMinute + 1));
    EXPECT_TRUE(watch.asking(9 * kMinute + 1));
}

/// "Keep recording": not asked again during this silence, and asked again
/// after the next one.
TEST(SetRecordingSilenceTest, KeepHoldsItUntilTheNextSilence) {
    SilenceWatch watch;
    watch.hear(0, -90.0);
    ASSERT_TRUE(watch.asking(5 * kMinute));
    watch.keep();
    watch.hear(20 * kMinute, -90.0);
    EXPECT_FALSE(watch.asking(20 * kMinute));
    watch.hear(21 * kMinute, -15.0);
    watch.hear(22 * kMinute, -90.0);
    EXPECT_FALSE(watch.asking(26 * kMinute));
    EXPECT_TRUE(watch.asking(27 * kMinute));
}

/// Digital silence (an input nobody feeds) is silence too.
TEST(SetRecordingSilenceTest, NothingAtAllIsSilent) {
    SilenceWatch watch;
    watch.hear(0, dbfs(0.0f));
    EXPECT_TRUE(watch.asking(5 * kMinute));
}

TEST(SetRecordingMeterTest, UpAtOnceDownAtItsRate) {
    LevelMeter meter;
    meter.update(0, -6.0);
    EXPECT_DOUBLE_EQ(-6.0, meter.levelDb());
    // Half a second of silence: 12 dB down.
    meter.update(500, -90.0);
    EXPECT_DOUBLE_EQ(-18.0, meter.levelDb());
    // Never under the floor.
    meter.update(10000, -90.0);
    EXPECT_DOUBLE_EQ(kMeterFloorDb, meter.levelDb());
}

TEST(SetRecordingMeterTest, TheTickHoldsThenFollows) {
    LevelMeter meter;
    meter.update(0, -3.0);
    meter.update(1000, -40.0);
    EXPECT_DOUBLE_EQ(-3.0, meter.holdDb());
    // Past the hold, it falls at the meter's rate, from where it was.
    meter.update(2000, -40.0);
    EXPECT_DOUBLE_EQ(-27.0, meter.holdDb());
    // And never below the bar.
    meter.update(4000, -40.0);
    EXPECT_DOUBLE_EQ(meter.levelDb(), meter.holdDb());
}

} // namespace
