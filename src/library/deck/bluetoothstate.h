#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

namespace mixxx {
namespace deck {

/// The Bluetooth receiver's rules, apart from Bluetooth: what it says it is
/// doing, how its times and its pairing code read, and when its screen goes
/// off. Pure, so that they are tested without a radio, a phone or a bus
/// (BluetoothReceiver gathers the facts; WDeckBluetooth shows them).
namespace bluetooth {

/// What the phone says it is playing: AVRCP, as BlueZ's MediaPlayer1 has it.
struct Track {
    QString title;
    QString artist;
    QString album;
    /// 0 when the phone gives none.
    qint64 durationMs = 0;
    /// The cover's handle for AVRCP's imaging (BIP), when the phone offers
    /// one: what obexd fetches the picture by.
    QString imageHandle;
};

/// A paired device, as the page lists it.
struct Device {
    /// BlueZ's object path: what the device is picked and forgotten by.
    QString path;
    QString name;
    bool connected = false;
    /// The one the DJ picked: the only one let in.
    bool picked = false;
};

/// Everything the receiver knows, as plain facts.
struct Facts {
    enum class Radio {
        Off,
        /// Mixxx has let go of the sound card and the radio is coming up.
        Starting,
        Up,
        /// It would not come up: `failure` says why.
        Failed,
    };
    Radio radio = Radio::Off;
    /// A controller is there (BlueZ has an adapter).
    bool adapter = false;
    /// The Pair window is open: the deck is visible to phones looking.
    bool discoverable = false;
    /// A phone asks to pair and its code waits for Confirm on the deck.
    bool hasPasskey = false;
    quint32 passkey = 0;
    /// The phone asking.
    QString pairingName;
    /// The paired devices, by name: the DJ picks the one that plays (Sam: "I
    /// don't want it jumping clients without being told").
    QList<Device> devices;
    /// The device picked, while the deck asks it to connect.
    QString connectingName;
    /// The phone connected, if any: the picked one, never another.
    QString phoneName;
    /// Its audio is streaming (the A2DP transport is active).
    bool streaming = false;
    /// MediaPlayer1.Status: "playing", "paused", "stopped", "forward-seek"...
    QString playerStatus;
    Track track;
    /// MediaPlayer1.Position when the phone last said it, and when that was
    /// on a monotonic clock: in between, a playing track moves on by itself
    /// (positionNow()), as BlueZ only signals a position that jumps.
    qint64 positionMs = 0;
    qint64 positionStampMs = 0;
    QString failure;
};

enum class Status {
    /// The menu is closed.
    Off,
    Starting,
    /// Up, but there is no controller to be a receiver with.
    NoAdapter,
    Failed,
    /// Up, nothing picked: the DJ picks a paired device, or pairs one.
    Waiting,
    /// A device picked, asked to connect.
    Connecting,
    /// Visible for pairing.
    Pairing,
    /// A code to confirm.
    Confirming,
    Connected,
    Playing,
};

Status status(const Facts& facts);

/// The line under the track: what the receiver is doing, in words. *deckName*
/// is what phones list the deck as.
QString statusText(const Facts& facts, const QString& deckName);

/// m:ss below an hour, h:mm:ss above, and "-:--" for a time not known.
QString formatTime(qint64 ms);

/// The six digits a phone shows to confirm, as "123 456".
QString formatPasskey(quint32 passkey);

/// Where the track is now: the position the phone last gave, moved on by the
/// time since if it is playing, never past the end.
qint64 positionNow(const Facts& facts, qint64 nowMs);

/// Whether BlueZ may let *device* in: the device picked, and a phone new to
/// the deck in the Pair window, which becomes the one picked once paired.
/// Any other paired device trying to connect or take over is refused until
/// the DJ picks it (Sam).
bool mayConnect(const QString& device, const QString& picked, bool paired, bool discoverable);

/// The paired devices in the order listed: by name, case aside.
QList<Device> sortedDevices(QList<Device> devices);

/// The screen goes off three minutes after the last touch or control (Sam).
constexpr qint64 kScreenOffAfterMs = 3 * 60 * 1000;
/// Whether it is time, with the last activity at *lastActivityMs* on the same
/// clock as *nowMs*.
bool screenOffDue(qint64 lastActivityMs, qint64 nowMs);

} // namespace bluetooth
} // namespace deck
} // namespace mixxx
