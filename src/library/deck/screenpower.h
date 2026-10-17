#pragma once

#include <QString>

namespace mixxx {
namespace deck {

/// The panel's light, off and back on: the Bluetooth receiver's screen off
/// (Sam: "turn the screen/black out the buttons while in this mode").
///
/// A panel with a backlight under /sys/class/backlight -- the decks' Waveshare
/// DSI panel, `10-0045` -- has it set to 0 and back: its `brightness` is the
/// video group's, which the deck's user is in. A panel without one, on HDMI,
/// is put into DPMS off through X (`xset`), and X wakes it at the next input.
///
/// What it was before goes into a file in RAM ($XDG_RUNTIME_DIR), so that a
/// Mixxx that crashed with the screen off leaves it lit at its next start
/// (recover()), never dark or with DPMS left armed to blank the screen ten
/// minutes into a set.
namespace screenpower {

void off();
void on();
/// Undo an off() that no on() followed: a crash. Cheap when there is none.
void recover();

/// The first backlight under *root* that says how bright it goes, or empty.
QString backlightIn(const QString& root = QStringLiteral("/sys/class/backlight"));

} // namespace screenpower
} // namespace deck
} // namespace mixxx
