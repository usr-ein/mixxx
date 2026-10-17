#include "library/deck/bluetoothstate.h"

#include <gtest/gtest.h>

using namespace mixxx::deck::bluetooth;
using Radio = Facts::Radio;

namespace {

Facts up() {
    Facts facts;
    facts.radio = Radio::Up;
    facts.adapter = true;
    return facts;
}

// ---------------------------------------------------------------------------
// What the receiver says it is doing
// ---------------------------------------------------------------------------

TEST(BluetoothStateTest, TheRadioFirst) {
    Facts facts;
    EXPECT_EQ(Status::Off, status(facts));
    facts.radio = Radio::Starting;
    EXPECT_EQ(Status::Starting, status(facts));
    facts.radio = Radio::Failed;
    EXPECT_EQ(Status::Failed, status(facts));
    // Up with no controller: a deck whose radio is not there (an emulated
    // one, or one wired to nothing).
    facts.radio = Radio::Up;
    EXPECT_EQ(Status::NoAdapter, status(facts));
    // A phone or a code means nothing while the radio is not up.
    facts.radio = Radio::Starting;
    facts.phoneName = QStringLiteral("Sam's phone");
    facts.hasPasskey = true;
    EXPECT_EQ(Status::Starting, status(facts));
}

TEST(BluetoothStateTest, WaitingThenPairing) {
    Facts facts = up();
    EXPECT_EQ(Status::Waiting, status(facts));
    facts.discoverable = true;
    EXPECT_EQ(Status::Pairing, status(facts));
}

TEST(BluetoothStateTest, ACodeOutranksEverything) {
    Facts facts = up();
    facts.discoverable = true;
    facts.hasPasskey = true;
    EXPECT_EQ(Status::Confirming, status(facts));
    // A second phone pairing while one plays: the question comes first.
    facts.phoneName = QStringLiteral("Sam's phone");
    facts.streaming = true;
    EXPECT_EQ(Status::Confirming, status(facts));
}

TEST(BluetoothStateTest, PlayingIsWhatIsHeard) {
    Facts facts = up();
    facts.phoneName = QStringLiteral("Sam's phone");
    EXPECT_EQ(Status::Connected, status(facts));
    // A player saying it plays, with no audio yet, is still only connected.
    facts.playerStatus = QStringLiteral("playing");
    EXPECT_EQ(Status::Connected, status(facts));
    facts.streaming = true;
    EXPECT_EQ(Status::Playing, status(facts));
    // Connected wins over the Pair window still open.
    facts.discoverable = true;
    EXPECT_EQ(Status::Playing, status(facts));
}

TEST(BluetoothStateTest, StatusTextNamesThePhoneAndTheDeck) {
    Facts facts = up();
    EXPECT_EQ(QStringLiteral("Waiting for a phone · Pair to add one"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.discoverable = true;
    EXPECT_EQ(QStringLiteral("Visible as trimixxx2: pair from the phone"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.hasPasskey = true;
    facts.pairingName = QStringLiteral("Pixel 9");
    EXPECT_EQ(QStringLiteral("Pair with Pixel 9? Same code on both: Confirm"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.hasPasskey = false;
    facts.phoneName = QStringLiteral("Pixel 9");
    facts.streaming = true;
    EXPECT_EQ(QStringLiteral("Playing from Pixel 9"), statusText(facts, QString()));
    // A phone with no name is still something to read.
    facts.phoneName.clear();
    facts.phoneName = QString();
    facts.hasPasskey = true;
    facts.pairingName.clear();
    EXPECT_EQ(QStringLiteral("Pair with the phone? Same code on both: Confirm"),
            statusText(facts, QString()));
}

TEST(BluetoothStateTest, FailedSaysWhy) {
    Facts facts;
    facts.radio = Radio::Failed;
    EXPECT_EQ(QStringLiteral("Bluetooth would not start"), statusText(facts, QString()));
    facts.failure = QStringLiteral("sudo: a password is required");
    EXPECT_EQ(QStringLiteral("Bluetooth would not start: sudo: a password is required"),
            statusText(facts, QString()));
}

// ---------------------------------------------------------------------------
// How it reads
// ---------------------------------------------------------------------------

TEST(BluetoothStateTest, Times) {
    EXPECT_EQ(QStringLiteral("0:00"), formatTime(0));
    EXPECT_EQ(QStringLiteral("0:59"), formatTime(59999));
    EXPECT_EQ(QStringLiteral("3:07"), formatTime(187000));
    EXPECT_EQ(QStringLiteral("59:59"), formatTime(3599000));
    EXPECT_EQ(QStringLiteral("1:00:00"), formatTime(3600000));
    EXPECT_EQ(QStringLiteral("1:02:03"), formatTime(3723000));
    EXPECT_EQ(QStringLiteral("-:--"), formatTime(-1));
}

TEST(BluetoothStateTest, PasskeysInTwoGroupsOfThree) {
    EXPECT_EQ(QStringLiteral("123 456"), formatPasskey(123456));
    // Leading zeros are digits the phone shows too.
    EXPECT_EQ(QStringLiteral("000 042"), formatPasskey(42));
    EXPECT_EQ(QStringLiteral("999 999"), formatPasskey(999999));
}

// ---------------------------------------------------------------------------
// Where the track is
// ---------------------------------------------------------------------------

TEST(BluetoothStateTest, APlayingTrackMovesOnByItself) {
    Facts facts;
    facts.track.durationMs = 200000;
    facts.positionMs = 10000;
    facts.positionStampMs = 5000;
    facts.playerStatus = QStringLiteral("playing");
    EXPECT_EQ(10000, positionNow(facts, 5000));
    EXPECT_EQ(13500, positionNow(facts, 8500));
    // Never past the end, however late the next position is.
    EXPECT_EQ(200000, positionNow(facts, 10000000));
}

TEST(BluetoothStateTest, APausedTrackStaysPut) {
    Facts facts;
    facts.track.durationMs = 200000;
    facts.positionMs = 10000;
    facts.positionStampMs = 5000;
    facts.playerStatus = QStringLiteral("paused");
    EXPECT_EQ(10000, positionNow(facts, 60000));
    // A clock that has not reached the stamp moves nothing back.
    facts.playerStatus = QStringLiteral("playing");
    EXPECT_EQ(10000, positionNow(facts, 1000));
}

TEST(BluetoothStateTest, NoDurationNoCeiling) {
    Facts facts;
    facts.positionMs = 10000;
    facts.positionStampMs = 0;
    facts.playerStatus = QStringLiteral("playing");
    EXPECT_EQ(70000, positionNow(facts, 60000));
}

// ---------------------------------------------------------------------------
// The screen goes off after three minutes
// ---------------------------------------------------------------------------

TEST(BluetoothStateTest, ScreenOffAfterThreeMinutes) {
    EXPECT_FALSE(screenOffDue(0, 0));
    EXPECT_FALSE(screenOffDue(0, 179999));
    EXPECT_TRUE(screenOffDue(0, 180000));
    EXPECT_TRUE(screenOffDue(1000, 181000));
    EXPECT_FALSE(screenOffDue(1000, 180999));
}

} // namespace
