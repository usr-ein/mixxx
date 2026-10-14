#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

namespace mixxx {
namespace deck {

/// "Record set"'s rules: what a recording of the DAC's input is called, how
/// long it may run and how much card it takes, when the input counts as
/// silent, what its track list says, and how its meter moves.
///
/// **Pure**: no files, no sound card, no clock of its own. DeckRecorder
/// records, the browser draws, and both are tested against this.
namespace setrecording {

/// The longest a recording runs: it stops by itself there (Sam: "Cap the max
/// recording length to 3 hours").
constexpr qint64 kMaxLengthMs = 3LL * 60 * 60 * 1000;

/// The MP3's bitrate, kbit/s, constant: what a recording takes on the card is
/// then known before it starts. 320 is MP3's best, what a set is shared at.
constexpr int kBitrateKbps = 320;

/// The bytes a recording of *lengthMs* takes, as MP3 at kBitrateKbps.
constexpr qint64 bytesFor(qint64 lengthMs) {
    return lengthMs * kBitrateKbps / 8;
}

/// The 8 digits a recording's two files share, from any number.
QString digitsOf(quint32 number);
/// `recorded-set-12345678.mp3` (Sam's name).
QString audioFileName(const QString& digits);
/// `recorded-set-track-list-12345678.txt` (Sam's name).
QString trackListFileName(const QString& digits);

/// A time into a recording, as its track list says it: `03:12`, or `1:02:13`
/// when the recording runs an hour or more (*hours*), so the times line up.
QString offsetText(qint64 ms, bool hours);
/// How long a recording has run, on the deck's screen: `0:42`, `12:34`,
/// `1:02:13`.
QString lengthText(qint64 ms);

/// A size as the deck says it, in a stick's own (decimal) units: `433 MB`,
/// `12.3 GB`.
QString sizeText(qint64 bytes);

/// One line of a track list.
struct TrackListLine {
    qint64 atMs = 0;
    QString artist;
    /// Empty when nobody could name it: "Unknown track", as the History says.
    QString title;
};

/// The track list's file: which recording it is and how long, then a line per
/// track, at the time into the recording it came in.
QString trackListText(const QString& digits, qint64 lengthMs, const QList<TrackListLine>& lines);

/// What dbfs() says of no signal at all: lower than any meter goes, and
/// finite, because Mixxx builds with fast math, which has no infinities.
constexpr double kSilenceDb = -200.0;

/// A peak of 0..1 (full scale) in dBFS; kSilenceDb for none.
double dbfs(float peak);

/// The meter's scale, in dBFS: its left end, and where it turns amber and red.
/// A mixer's send set for the codec peaks around -13 (its fixed-gain ADC has
/// no headroom of its own past 0).
constexpr double kMeterFloorDb = -60.0;
constexpr double kMeterAmberDb = -12.0;
constexpr double kMeterRedDb = -3.0;
/// Where *db* sits on the meter, 0..1.
double meterPosition(double db);

/// When to ask the DJ whether to end the recording: after five minutes in
/// which the input heard nothing (Sam: "a popup ... if the recording has been
/// silent for >5 minutes prompting the user to end the recording").
///
/// Level-based, not an event: it asks for as long as the silence lasts and
/// nobody said "keep recording". Sound on the input ends the silence, and the
/// question with it; "keep recording" holds it back until the input has had
/// sound and then another five silent minutes.
class SilenceWatch {
  public:
    /// The input hears nothing below this peak. The codec's own floor is about
    /// -78 dBFS with nothing plugged in, a mixer's send carrying music peaks
    /// around -13, and no set stays under -50 for long.
    static constexpr double kSilentBelowDb = -50.0;
    static constexpr qint64 kAskAfterMs = 5LL * 60 * 1000;

    explicit SilenceWatch(qint64 askAfterMs = kAskAfterMs)
            : m_askAfterMs(askAfterMs) {
    }

    /// A stretch of the input that ended at *nowMs*, and its loudest.
    void hear(qint64 nowMs, double peakDb);
    /// Whether to ask the DJ now.
    bool asking(qint64 nowMs) const;
    /// "Keep recording".
    void keep();
    /// Since when the input has heard nothing; -1 while it hears something.
    qint64 silentSinceMs() const {
        return m_silentSinceMs;
    }

  private:
    qint64 m_askAfterMs;
    qint64 m_silentSinceMs = -1;
    bool m_kept = false;
};

/// One bar of the meter: up at once, down at a steady rate, with a tick that
/// holds the loudest a moment before it follows.
class LevelMeter {
  public:
    static constexpr double kFallDbPerSecond = 24.0;
    static constexpr qint64 kHoldMs = 1500;

    /// The loudest the input was since the last update, at *nowMs*.
    void update(qint64 nowMs, double peakDb);
    double levelDb() const {
        return m_levelDb;
    }
    double holdDb() const {
        return m_holdDb;
    }

  private:
    qint64 m_lastMs = -1;
    double m_levelDb = kMeterFloorDb;
    double m_holdDb = kMeterFloorDb;
    qint64 m_heldSinceMs = 0;
};

} // namespace setrecording
} // namespace deck
} // namespace mixxx
