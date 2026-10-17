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
    // A device or a code means nothing while the radio is not up.
    facts.radio = Radio::Starting;
    facts.connectedName = QStringLiteral("Sam's phone");
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
    // A second device pairing while one plays: the question comes first.
    facts.connectedName = QStringLiteral("Sam's phone");
    facts.streaming = true;
    EXPECT_EQ(Status::Confirming, status(facts));
}

TEST(BluetoothStateTest, PlayingIsWhatIsHeard) {
    Facts facts = up();
    facts.connectedName = QStringLiteral("Sam's phone");
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
    EXPECT_EQ(QStringLiteral("Waiting for a device · + to pair one"),
            statusText(facts, QStringLiteral("trimixxx2")));
    Device pixel;
    pixel.name = QStringLiteral("Pixel 9");
    facts.devices = {pixel};
    EXPECT_EQ(QStringLiteral("Pick a device to play from"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.connectingName = QStringLiteral("Pixel 9");
    EXPECT_EQ(QStringLiteral("Connecting to Pixel 9…"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.connectingName.clear();
    facts.discoverable = true;
    EXPECT_EQ(QStringLiteral("Visible as trimixxx2: pair from the device"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.hasPasskey = true;
    facts.pairingName = QStringLiteral("Pixel 9");
    EXPECT_EQ(QStringLiteral("Pair with Pixel 9? Same code on both: Confirm"),
            statusText(facts, QStringLiteral("trimixxx2")));
    facts.hasPasskey = false;
    facts.connectedName = QStringLiteral("Pixel 9");
    facts.streaming = true;
    EXPECT_EQ(QStringLiteral("Playing from Pixel 9"), statusText(facts, QString()));
    // A device with no name is still something to read: "device", never
    // "phone", as it can be a Mac (Sam).
    facts.connectedName.clear();
    facts.hasPasskey = true;
    facts.pairingName.clear();
    EXPECT_EQ(QStringLiteral("Pair with the device? Same code on both: Confirm"),
            statusText(facts, QString()));
    facts.hasPasskey = false;
    facts.connectedName = QStringLiteral("MBP NJ");
    facts.streaming = false;
    EXPECT_EQ(QStringLiteral("Connected to MBP NJ"), statusText(facts, QString()));
}

TEST(BluetoothStateTest, NoPhoneInWhatTheDjReads) {
    // Every status line, with and without names: none says "phone".
    Facts facts = up();
    QStringList lines;
    const auto read = [&]() { lines << statusText(facts, QStringLiteral("trimixxx2")); };
    read();
    Device mac;
    mac.name = QStringLiteral("MBP NJ");
    facts.devices = {mac};
    read();
    facts.connectingName = QStringLiteral("MBP NJ");
    read();
    facts.connectingName.clear();
    facts.discoverable = true;
    read();
    facts.hasPasskey = true;
    read();
    facts.pairingName = QStringLiteral("MBP NJ");
    read();
    facts.hasPasskey = false;
    facts.connectedName = QStringLiteral("MBP NJ");
    read();
    facts.streaming = true;
    read();
    for (const QString& line : std::as_const(lines)) {
        EXPECT_FALSE(line.contains(QStringLiteral("phone"), Qt::CaseInsensitive))
                << line.toStdString();
    }
}

TEST(BluetoothStateTest, ConnectingUntilThePickedOneIsIn) {
    Facts facts = up();
    facts.connectingName = QStringLiteral("Pixel 9");
    EXPECT_EQ(Status::Connecting, status(facts));
    // Picked and asked outranks the Pair window: the DJ chose.
    facts.discoverable = true;
    EXPECT_EQ(Status::Connecting, status(facts));
    facts.connectedName = QStringLiteral("Pixel 9");
    EXPECT_EQ(Status::Connected, status(facts));
}

// ---------------------------------------------------------------------------
// Who may connect: the device picked, nothing else (Sam)
// ---------------------------------------------------------------------------

TEST(BluetoothStateTest, OnlyThePickedDeviceIsLetIn) {
    const QString pixel = QStringLiteral("/org/bluez/hci0/dev_AA");
    const QString iphone = QStringLiteral("/org/bluez/hci0/dev_BB");
    // Nothing picked, on every opening: no paired device gets in.
    EXPECT_FALSE(mayConnect(pixel, QString(), true, false));
    EXPECT_TRUE(mayConnect(pixel, pixel, true, false));
    // Another paired device trying to take over is refused, Pair window or
    // not: pairing is for devices new to the deck.
    EXPECT_FALSE(mayConnect(iphone, pixel, true, false));
    EXPECT_FALSE(mayConnect(iphone, pixel, true, true));
    // A device new to the deck, in the Pair window only.
    EXPECT_TRUE(mayConnect(iphone, pixel, false, true));
    EXPECT_FALSE(mayConnect(iphone, pixel, false, false));
    // No path is no device.
    EXPECT_FALSE(mayConnect(QString(), QString(), true, false));
}

TEST(BluetoothStateTest, TheStarredDeviceIsPickedWhenNothingIs) {
    Device pixel;
    pixel.path = QStringLiteral("/org/bluez/hci0/dev_AA");
    Device iphone;
    iphone.path = QStringLiteral("/org/bluez/hci0/dev_BB");
    // Nothing starred: nothing is picked by the deck, on opening or after.
    EXPECT_EQ(QString(), autoPick({pixel, iphone}, QString()));
    // Starred, and nothing picked (the menu just opened): it is.
    iphone.starred = true;
    EXPECT_EQ(iphone.path, autoPick({pixel, iphone}, QString()));
    // The DJ's own pick stands, the starred one's included.
    EXPECT_EQ(QString(), autoPick({pixel, iphone}, pixel.path));
    EXPECT_EQ(QString(), autoPick({pixel, iphone}, iphone.path));
    // No devices, no pick.
    EXPECT_EQ(QString(), autoPick({}, QString()));
}

TEST(BluetoothStateTest, DevicesByName) {
    Device a;
    a.name = QStringLiteral("pixel 9");
    Device b;
    b.name = QStringLiteral("iPhone 15");
    Device c;
    c.name = QStringLiteral("MBP NJ");
    const QList<Device> sorted = sortedDevices({a, b, c});
    ASSERT_EQ(3, sorted.size());
    EXPECT_EQ(QStringLiteral("iPhone 15"), sorted.at(0).name);
    EXPECT_EQ(QStringLiteral("MBP NJ"), sorted.at(1).name);
    EXPECT_EQ(QStringLiteral("pixel 9"), sorted.at(2).name);
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
    // Leading zeros are digits the device shows too.
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

TEST(BluetoothStateTest, ABackwardJumpIsFollowed) {
    // The device seeks back to the start: its position replaces the deck's,
    // however far the deck had counted.
    Facts facts;
    facts.track.durationMs = 200000;
    facts.playerStatus = QStringLiteral("playing");
    setPosition(&facts, 90000, 1000);
    EXPECT_EQ(100000, positionNow(facts, 11000));
    setPosition(&facts, 0, 11000);
    EXPECT_EQ(0, positionNow(facts, 11000));
    EXPECT_EQ(2500, positionNow(facts, 13500));
    // And forwards, past where it had counted to.
    setPosition(&facts, 150000, 13500);
    EXPECT_EQ(150000, positionNow(facts, 13500));
}

TEST(BluetoothStateTest, AStatusChangeKeepsThePositionReached) {
    Facts facts;
    facts.track.durationMs = 200000;
    facts.playerStatus = QStringLiteral("playing");
    setPosition(&facts, 10000, 0);
    // Paused 5 s later: it stays where it got to.
    setStatus(&facts, QStringLiteral("paused"), 5000);
    EXPECT_EQ(QStringLiteral("paused"), facts.playerStatus);
    EXPECT_EQ(15000, positionNow(facts, 5000));
    EXPECT_EQ(15000, positionNow(facts, 60000));
    // Playing again a minute on: from there, not from a minute later.
    setStatus(&facts, QStringLiteral("playing"), 60000);
    EXPECT_EQ(15000, positionNow(facts, 60000));
    EXPECT_EQ(17000, positionNow(facts, 62000));
}

// ---------------------------------------------------------------------------
// The deck's controls, for the device (Sam)
// ---------------------------------------------------------------------------

TEST(BluetoothStateTest, PlayPausesWhatPlaysAndPlaysTheRest) {
    EXPECT_EQ(QStringLiteral("Pause"), playPauseMethod(QStringLiteral("playing")));
    // Seeking as it plays is playing.
    EXPECT_EQ(QStringLiteral("Pause"), playPauseMethod(QStringLiteral("forward-seek")));
    EXPECT_EQ(QStringLiteral("Pause"), playPauseMethod(QStringLiteral("reverse-seek")));
    EXPECT_EQ(QStringLiteral("Play"), playPauseMethod(QStringLiteral("paused")));
    EXPECT_EQ(QStringLiteral("Play"), playPauseMethod(QStringLiteral("stopped")));
    EXPECT_EQ(QStringLiteral("Play"), playPauseMethod(QStringLiteral("error")));
    // A device that has said nothing yet: play is what is asked of it.
    EXPECT_EQ(QStringLiteral("Play"), playPauseMethod(QString()));
    EXPECT_TRUE(isPlaying(QStringLiteral("playing")));
    EXPECT_FALSE(isPlaying(QStringLiteral("paused")));
    EXPECT_FALSE(isPlaying(QString()));
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
