#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace mixxx {
namespace deck {

/// The History's rule: what played on the link, and how it is listed.
///
/// What is recorded is **plays**: one track on one player, from when it
/// started playing to when it stopped. What the DJ reads is **rows**: a stretch
/// of time during which the same tracks played, with its length, in Sam's
/// format:
///
///     Track 1 (10s)
///     Track 1 | Track 2 (20s)
///     Track 2 (40s)
///
/// The rows are worked out from the plays every time they are shown, and never
/// stored. So what reaches the card is what happened, and the rules for
/// listing it -- what is too short to show, what a pause does -- can change
/// without touching anything already written.
///
/// **Pure**: no network, no deck, no database. DeckHistory records through
/// Recorder and HistoryStore keeps the plays; this is what both are tested
/// against.
namespace history {

/// This deck's own player, in a Play. The link's players are 1 and up.
constexpr int kThisDeck = 0;

/// One track on one player, from when it started playing to when it stopped,
/// in milliseconds of the boot's clock.
///
/// The boot's clock and not the wall clock: a Pi has no clock that keeps time,
/// and NTP (when there is any) moves the wall clock by however long the deck
/// was off. Lengths measured across such a jump would be wrong by days.
struct Play {
    /// Who played it: kThisDeck, or a player number on the link.
    int player = kThisDeck;
    /// The track, as the session numbers its tracks. Two plays of one track
    /// carry the same number, on any player.
    int track = 0;
    qint64 startMs = 0;
    /// Still playing: the last time it was known to be.
    qint64 endMs = 0;
};

/// A stretch of time during which the same tracks played.
struct Segment {
    qint64 startMs = 0;
    qint64 endMs = 0;
    /// In the order they started playing, which is what puts "Track 1 |
    /// Track 2" that way round. A track playing on two players at once is in
    /// twice.
    QList<int> tracks;

    qint64 durationMs() const {
        return endMs - startMs;
    }
};

/// What played together, in order. Silence is no segment.
///
/// A segment ends wherever the tracks playing change: one starting, one
/// stopping, or one player going from a track to another. It does not end
/// where only the players change and the tracks do not -- a track that stops
/// on one player as it starts on another plays on.
///
/// In time growing as P log P with the plays: they are sorted once and
/// walked once.
QList<Segment> segmentsOf(QList<Play> plays);

/// One row of the History list: one or more segments of the same tracks.
struct Row {
    /// As the first of its segments had them.
    QList<int> tracks;
    /// How long they played, the silences between its segments left out.
    qint64 durationMs = 0;
    /// When the first of its segments started.
    qint64 startMs = 0;
};

/// How segments are listed.
struct RowRules {
    /// Rows shorter than this are left out: the half-second two tracks overlap
    /// in a cut, a CUE tapped on another deck.
    qint64 minimumMs = 2000;
};

/// The rows *segments* are listed as, in order:
///
///  - segments of the same tracks, one after the other with only silence
///    between them, are one row: a track paused and played on is one row,
///    with the pause left out of its length;
///  - a row shorter than `rules.minimumMs` is left out;
///  - and the rows that leaves side by side with the same tracks are one row.
QList<Row> rowsOf(const QList<Segment>& segments, const RowRules& rules = RowRules());

/// How many different tracks *rows* name.
int trackCount(const QList<Row>& rows);

/// A track of what played during a recording ("Record set").
struct ListedTrack {
    int track = 0;
    /// When it was first listed, from the recording's start.
    qint64 atMs = 0;
};

/// What played between *fromMs* and *toMs*, a recording's start and end, as
/// its track list: each track once, in the order the History's rows first
/// name it, at the start of that row. The plays are cut to the recording
/// first: what played only before or after it is not in, and what was already
/// playing when it started is there from 0. A row too short to list (a CUE
/// tapped on another deck) names nothing, as in the History.
QList<ListedTrack> tracksBetween(
        const QList<Play>& plays, qint64 fromMs, qint64 toMs, const RowRules& rules = RowRules());

/// A length as the History shows it: `10s`, `1m20s`, `2m`, `1h05m`.
QString durationText(qint64 ms);

/// What one player is doing, sampled.
struct Sample {
    int player = kThisDeck;
    /// What it is playing, as DeckHistory identifies tracks (see there).
    QString trackKey;
};

/// Turns samples of what plays into plays starting and stopping.
///
/// Sampled rather than told: the network says what each player is doing many
/// times a second, and nothing says "stopped" when a cable is pulled. A player
/// that is not in a sample is not playing.
class Recorder {
  public:
    struct Change {
        enum class Kind {
            Started,
            Stopped,
        };
        Kind kind = Kind::Started;
        int player = kThisDeck;
        QString trackKey;
        qint64 atMs = 0;
    };

    /// What plays at *nowMs*: every player that is playing, and what. A player
    /// is in it at most once. Returns what started and stopped since the last
    /// update, the stops first.
    QList<Change> update(qint64 nowMs, const QList<Sample>& playing);

    /// Everything playing stops at *nowMs*: Mixxx is going.
    QList<Change> stopAll(qint64 nowMs);

    /// What each player is playing now, by player.
    const QHash<int, QString>& playing() const {
        return m_playing;
    }

  private:
    QHash<int, QString> m_playing;
};

} // namespace history
} // namespace deck
} // namespace mixxx
