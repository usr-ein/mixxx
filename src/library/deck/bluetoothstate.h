#pragma once

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
    /// The phone connected, if any.
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
    /// Up, no phone: known phones may connect.
    Waiting,
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

/// The screen goes off three minutes after the last touch or control (Sam).
constexpr qint64 kScreenOffAfterMs = 3 * 60 * 1000;
/// Whether it is time, with the last activity at *lastActivityMs* on the same
/// clock as *nowMs*.
bool screenOffDue(qint64 lastActivityMs, qint64 nowMs);

} // namespace bluetooth
} // namespace deck
} // namespace mixxx
