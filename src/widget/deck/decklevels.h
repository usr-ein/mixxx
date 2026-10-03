#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>
#include <optional>

class ControlProxy;

namespace mixxx {
namespace deck {

/// The two levels the deck can be set to from its own screen: how loud it plays
/// and how bright its panel is. Diagnostics adjusts them (browser-prd.md 14).
///
/// **Output** is `[Master],gain`, the last gain stage before the DAC. The engine
/// ramps it across a buffer, so a detent does not click, and applies it ahead
/// of the VU meter, so `[Main],peak_indicator` reports what it did to the
/// output. A trim rather than a fader: a few dB either side of unity, to sit
/// the deck level with the players beside it. Above unity it clips loud masters
/// -- the UCA222 has no headroom to give (mixxx_config/README.md) -- which is
/// why Diagnostics shows clipping beside it.
///
/// **Brightness** is the panel's backlight, through the first device under
/// /sys/class/backlight -- `10-0045`, the Waveshare DSI panel's, on the decks.
/// Raspberry Pi OS gives its `brightness` file to the `video` group, which the
/// deck's user is in, so this needs no helper and no sudo. A panel with no
/// backlight there, or a machine that is not a Pi, has no brightness to set.
///
/// **Neither is kept in mixxx.cfg.** Mixxx persists neither, writes that file
/// only on a clean exit, and mixxx_config/upload.sh replaces it on every
/// deploy, which would put both back to default for a mapping tweak. So they
/// have a file of their own beside it, like sessionpurge's boot id, written a
/// moment after the last change and applied when the skin loads. It holds only
/// what was set here: a level nobody has touched stays wherever the system put
/// it, which for the backlight is systemd-backlight's restore.
class DeckLevels : public QObject {
    Q_OBJECT

  public:
    static constexpr double kOutputMinDb = -6.0;
    static constexpr double kOutputMaxDb = 6.0;
    static constexpr double kOutputStepDb = 0.5;
    /// What a gain of nothing at all reads as. Silence is -inf dB, and Mixxx
    /// builds with -ffast-math, which lets the compiler assume no value is ever
    /// infinite; a floor keeps every level a finite number.
    static constexpr double kSilenceDb = -120.0;
    /// Not zero: a panel turned all the way down cannot be read to turn it
    /// back up.
    static constexpr int kBrightnessMin = 10;
    static constexpr int kBrightnessMax = 100;
    static constexpr int kBrightnessStep = 5;

    /// *settingsPath* is the directory holding mixxx.cfg. *backlightRoot* is
    /// where backlights are looked for, and is only anything else in a test.
    explicit DeckLevels(const QString& settingsPath,
            const QString& backlightRoot = QStringLiteral("/sys/class/backlight"),
            QObject* pParent = nullptr);
    ~DeckLevels() override;

    bool hasOutput() const;
    /// The output gain in dB as it is now, whoever set it. kSilenceDb when
    /// silent.
    double outputDb() const;
    /// Move the output by *steps* of kOutputStepDb, within the trim's range.
    void stepOutput(int steps);

    bool hasBacklight() const {
        return !m_backlight.isEmpty();
    }
    /// The backlight in percent of its maximum, or -1 if it cannot be read.
    int brightness() const;
    /// Move the backlight by *steps* of kBrightnessStep, within its range.
    void stepBrightness(int steps);

    /// Onto the step grid and into range. Pure.
    static double snapOutputDb(double db);
    static int snapBrightness(int percent);

  private:
    void applySaved();
    void scheduleSave();
    void save();
    int readRawBrightness() const;
    bool writeBrightness(int percent);

    const QString m_path;
    std::unique_ptr<ControlProxy> m_pGain;
    /// The backlight's sysfs directory, or empty for a panel without one.
    QString m_backlight;
    int m_maxBrightness = 0;
    /// What the file holds, or will once the save timer fires.
    std::optional<double> m_savedOutputDb;
    std::optional<int> m_savedBrightness;
    /// Coalesces a turn of the encoder into one write, at its end.
    QTimer m_saveTimer;
};

} // namespace deck
} // namespace mixxx
