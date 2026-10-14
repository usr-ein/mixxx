#include "library/deck/playhistory.h"

#include <gtest/gtest.h>

#include <QStringList>

namespace {

using mixxx::deck::history::durationText;
using mixxx::deck::history::kThisDeck;
using mixxx::deck::history::Play;
using mixxx::deck::history::Recorder;
using mixxx::deck::history::Row;
using mixxx::deck::history::RowRules;
using mixxx::deck::history::rowsOf;
using mixxx::deck::history::Sample;
using mixxx::deck::history::Segment;
using mixxx::deck::history::segmentsOf;

constexpr qint64 kSecond = 1000;

Play play(int player, int track, double fromSeconds, double toSeconds) {
    Play p;
    p.player = player;
    p.track = track;
    p.startMs = static_cast<qint64>(fromSeconds * kSecond);
    p.endMs = static_cast<qint64>(toSeconds * kSecond);
    return p;
}

/// The rows as the list reads them, "1 | 2 (20s)", so an expectation is the
/// list itself.
QStringList listed(const QList<Row>& rows) {
    QStringList lines;
    for (const Row& row : rows) {
        QStringList tracks;
        for (const int track : row.tracks) {
            tracks.append(QString::number(track));
        }
        lines.append(QStringLiteral("%1 (%2)").arg(
                tracks.join(QStringLiteral(" | ")), durationText(row.durationMs)));
    }
    return lines;
}

QStringList listed(const QList<Play>& plays) {
    return listed(rowsOf(segmentsOf(plays)));
}

// ---------------------------------------------------------------------------
// Sam's own example, which is the specification.
// ---------------------------------------------------------------------------

/// Decks A, B and C. A plays track 1; B starts track 2 over it; A stops; C
/// starts track 3 over track 2 and stops it again; B plays on, then stops as
/// C plays track 3 on. Each row of his list, in his words.
TEST(PlayHistoryTest, SamsExampleRowForRow) {
    constexpr int kA = 1;
    constexpr int kB = 2;
    constexpr int kC = 3;
    const QList<Play> plays = {
            play(kA, 1, 0, 30),
            play(kB, 2, 10, 160),
            play(kC, 3, 70, 80),
            play(kC, 3, 160, 280),
    };
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (10s)"),
                      QStringLiteral("1 | 2 (20s)"),
                      QStringLiteral("2 (40s)"),
                      QStringLiteral("2 | 3 (10s)"),
                      QStringLiteral("2 (1m20s)"),
                      QStringLiteral("3 (2m)"),
              }),
            listed(plays));
}

/// The order the plays happen to be stored in says nothing about the order
/// they happened in.
TEST(PlayHistoryTest, TheOrderPlaysAreStoredInDoesNotMatter) {
    const QList<Play> plays = {
            play(3, 3, 160, 280),
            play(2, 2, 10, 160),
            play(3, 3, 70, 80),
            play(1, 1, 0, 30),
    };
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (10s)"),
                      QStringLiteral("1 | 2 (20s)"),
                      QStringLiteral("2 (40s)"),
                      QStringLiteral("2 | 3 (10s)"),
                      QStringLiteral("2 (1m20s)"),
                      QStringLiteral("3 (2m)"),
              }),
            listed(plays));
}

// ---------------------------------------------------------------------------
// Overlaps.
// ---------------------------------------------------------------------------

/// "Track 1 | Track 2": the one that started first comes first, whichever
/// player it is on.
TEST(PlayHistoryTest, AnOverlapListsTheTracksInTheOrderTheyStarted) {
    const QList<Segment> segments = segmentsOf({
            play(3, 7, 0, 60),
            play(1, 5, 30, 90),
    });
    ASSERT_EQ(3, segments.size());
    EXPECT_EQ(QList<int>({7, 5}), segments.at(1).tracks);
}

/// A track joining two already playing goes last, and the two keep their
/// order.
TEST(PlayHistoryTest, ATrackJoiningGoesLast) {
    const QList<Segment> segments = segmentsOf({
            play(1, 1, 0, 100),
            play(2, 2, 10, 100),
            play(kThisDeck, 3, 20, 100),
    });
    ASSERT_EQ(3, segments.size());
    EXPECT_EQ(QList<int>({1, 2, 3}), segments.at(2).tracks);
}

/// A cut: the half second both played is no row, and either side of it is.
TEST(PlayHistoryTest, AHalfSecondOverlapInACutIsNoRow) {
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (3m)"),
                      QStringLiteral("2 (2m)"),
              }),
            listed({
                    play(1, 1, 0, 180.4),
                    play(2, 2, 180, 300),
            }));
}

/// A CUE tapped on another deck in the middle of a track: the track is one row
/// of its whole length, not two with a blip between them.
TEST(PlayHistoryTest, ABlipOnAnotherDeckDoesNotSplitATrack) {
    EXPECT_EQ(QStringList({QStringLiteral("1 (1m)")}),
            listed({
                    play(1, 1, 0, 60),
                    play(2, 2, 20, 20.5),
            }));
}

/// The minimum is a rule of the list, not of the record: at zero, everything
/// that played is a row.
TEST(PlayHistoryTest, WithNoMinimumEveryOverlapIsARow) {
    RowRules rules;
    rules.minimumMs = 0;
    const QList<Row> rows = rowsOf(segmentsOf({
                                           play(1, 1, 0, 180.4),
                                           play(2, 2, 180, 300),
                                   }),
            rules);
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (3m)"),
                      QStringLiteral("1 | 2 (0s)"),
                      QStringLiteral("2 (2m)"),
              }),
            listed(rows));
}

/// The same track on two players at once -- a double drop -- is in the row
/// twice, because twice is what played.
TEST(PlayHistoryTest, OneTrackOnTwoPlayersAtOnceIsInTheRowTwice) {
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (10s)"),
                      QStringLiteral("1 | 1 (20s)"),
                      QStringLiteral("1 (30s)"),
              }),
            listed({
                    play(1, 1, 0, 30),
                    play(2, 1, 10, 60),
            }));
}

/// A track that stops on one player at the moment it starts on another plays
/// on: the tracks did not change, only the players.
TEST(PlayHistoryTest, ATrackHandedFromOnePlayerToAnotherPlaysOn) {
    const QList<Segment> segments = segmentsOf({
            play(1, 1, 0, 30),
            play(2, 1, 30, 70),
    });
    ASSERT_EQ(1, segments.size());
    EXPECT_EQ(0, segments.first().startMs);
    EXPECT_EQ(70 * kSecond, segments.first().endMs);
}

// ---------------------------------------------------------------------------
// Pauses, replays, silence.
// ---------------------------------------------------------------------------

/// Paused and played on: one row, the pause left out of its length.
TEST(PlayHistoryTest, APausedTrackPlayedOnIsOneRow) {
    const QList<Play> plays = {
            play(1, 1, 0, 30),
            play(1, 1, 33, 60),
    };
    // Two segments: the silence between them is a segment of nothing.
    EXPECT_EQ(2, segmentsOf(plays).size());
    EXPECT_EQ(QStringList({QStringLiteral("1 (57s)")}), listed(plays));
}

/// A pause in the middle of an overlap: the overlap goes on, as one row.
TEST(PlayHistoryTest, APauseOfBothTracksInAnOverlapIsOneRow) {
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (10s)"),
                      QStringLiteral("1 | 2 (30s)"),
                      QStringLiteral("2 (10s)"),
              }),
            listed({
                    play(1, 1, 0, 20),
                    play(2, 2, 10, 20),
                    play(1, 1, 25, 45),
                    play(2, 2, 25, 55),
            }));
}

/// A track played again after others is a row of its own, where it played.
TEST(PlayHistoryTest, ATrackReplayedAfterOthersIsARowAgain) {
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (1m)"),
                      QStringLiteral("2 (1m)"),
                      QStringLiteral("1 (1m)"),
              }),
            listed({
                    play(1, 1, 0, 60),
                    play(2, 2, 60, 120),
                    play(1, 1, 120, 180),
            }));
}

/// One player going straight from one track to the next: two rows.
TEST(PlayHistoryTest, APlayerGoingFromOneTrackToTheNextIsTwoRows) {
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (1m)"),
                      QStringLiteral("2 (1m)"),
              }),
            listed({
                    play(1, 1, 0, 60),
                    play(1, 2, 60, 120),
            }));
}

/// Silence is no row, at the start, between tracks or at the end.
TEST(PlayHistoryTest, SilenceIsNoRow) {
    const QList<Segment> segments = segmentsOf({
            play(1, 1, 100, 160),
            play(2, 2, 400, 460),
    });
    ASSERT_EQ(2, segments.size());
    EXPECT_EQ(100 * kSecond, segments.at(0).startMs);
    EXPECT_EQ(400 * kSecond, segments.at(1).startMs);
}

/// Nothing played: nothing to list, and not a row of nothing.
TEST(PlayHistoryTest, NothingPlayedIsNoRows) {
    EXPECT_TRUE(segmentsOf({}).isEmpty());
    EXPECT_TRUE(rowsOf({}).isEmpty());
}

/// A play of no length, or one that ends before it starts (a row damaged on
/// the card), is no time at all and changes nothing around it.
TEST(PlayHistoryTest, APlayOfNoLengthIsIgnored) {
    EXPECT_EQ(QStringList({QStringLiteral("1 (1m)")}),
            listed({
                    play(1, 1, 0, 60),
                    play(2, 2, 30, 30),
                    play(3, 3, 50, 40),
            }));
}

/// Only plays that were too short: nothing to list.
TEST(PlayHistoryTest, OnlyCueTapsListNothing) {
    EXPECT_TRUE(rowsOf(segmentsOf({
                               play(1, 1, 0, 0.3),
                               play(1, 1, 10, 10.4),
                       }))
                        .isEmpty());
}

// ---------------------------------------------------------------------------
// Lengths, as the list prints them.
// ---------------------------------------------------------------------------

TEST(PlayHistoryTest, DurationsReadAsSamWritesThem) {
    EXPECT_EQ(QStringLiteral("10s"), durationText(10 * kSecond));
    EXPECT_EQ(QStringLiteral("10s"), durationText(9600));
    EXPECT_EQ(QStringLiteral("59s"), durationText(59 * kSecond));
    EXPECT_EQ(QStringLiteral("1m"), durationText(60 * kSecond));
    EXPECT_EQ(QStringLiteral("1m20s"), durationText(80 * kSecond));
    EXPECT_EQ(QStringLiteral("1m05s"), durationText(65 * kSecond));
    EXPECT_EQ(QStringLiteral("2m"), durationText(120 * kSecond));
    EXPECT_EQ(QStringLiteral("59m59s"), durationText(3599 * kSecond));
    EXPECT_EQ(QStringLiteral("1h00m"), durationText(3600 * kSecond));
    EXPECT_EQ(QStringLiteral("1h05m"), durationText((3600 + 5 * 60 + 10) * kSecond));
    EXPECT_EQ(QStringLiteral("0s"), durationText(-5));
}

// ---------------------------------------------------------------------------
// The recorder: samples into plays starting and stopping.
// ---------------------------------------------------------------------------

using Kind = Recorder::Change::Kind;

TEST(PlayHistoryRecorderTest, AStartAndAStop) {
    Recorder recorder;
    QList<Recorder::Change> changes = recorder.update(0, {{1, QStringLiteral("a")}});
    ASSERT_EQ(1, changes.size());
    EXPECT_EQ(Kind::Started, changes.first().kind);
    EXPECT_EQ(1, changes.first().player);
    EXPECT_EQ(QStringLiteral("a"), changes.first().trackKey);
    EXPECT_EQ(0, changes.first().atMs);

    // Sampled again, still playing: nothing new.
    EXPECT_TRUE(recorder.update(250, {{1, QStringLiteral("a")}}).isEmpty());

    changes = recorder.update(500, {});
    ASSERT_EQ(1, changes.size());
    EXPECT_EQ(Kind::Stopped, changes.first().kind);
    EXPECT_EQ(500, changes.first().atMs);
    EXPECT_TRUE(recorder.playing().isEmpty());
}

/// A player gone from the link mid-play is a player not in the sample: its
/// track stops there, and not when the network forgets it half a minute on.
TEST(PlayHistoryRecorderTest, APlayerLeavingMidPlayStopsItsTrack) {
    Recorder recorder;
    recorder.update(0, {{1, QStringLiteral("a")}, {2, QStringLiteral("b")}});
    const QList<Recorder::Change> changes = recorder.update(1000, {{1, QStringLiteral("a")}});
    ASSERT_EQ(1, changes.size());
    EXPECT_EQ(Kind::Stopped, changes.first().kind);
    EXPECT_EQ(2, changes.first().player);
    EXPECT_EQ(QStringLiteral("b"), changes.first().trackKey);
}

/// Straight from one track to the next: the first stops, then the next starts,
/// at the same moment.
TEST(PlayHistoryRecorderTest, ANewTrackWithoutStoppingStopsTheOldOneFirst) {
    Recorder recorder;
    recorder.update(0, {{1, QStringLiteral("a")}});
    const QList<Recorder::Change> changes = recorder.update(1000, {{1, QStringLiteral("b")}});
    ASSERT_EQ(2, changes.size());
    EXPECT_EQ(Kind::Stopped, changes.at(0).kind);
    EXPECT_EQ(QStringLiteral("a"), changes.at(0).trackKey);
    EXPECT_EQ(Kind::Started, changes.at(1).kind);
    EXPECT_EQ(QStringLiteral("b"), changes.at(1).trackKey);
    EXPECT_EQ(1000, changes.at(1).atMs);
}

/// A sample with no track for a player is no play: a deck with nothing loaded
/// says it is stopped, not that it plays nothing.
TEST(PlayHistoryRecorderTest, ASampleWithoutATrackIsNotPlaying) {
    Recorder recorder;
    EXPECT_TRUE(recorder.update(0, {{1, QString()}}).isEmpty());
    EXPECT_TRUE(recorder.playing().isEmpty());
}

/// Several at once come out stops first, then starts, each by player.
TEST(PlayHistoryRecorderTest, ChangesComeOutStopsFirstByPlayer) {
    Recorder recorder;
    recorder.update(0, {{3, QStringLiteral("c")}, {1, QStringLiteral("a")}});
    const QList<Recorder::Change> changes = recorder.update(
            100, {{2, QStringLiteral("b")}, {4, QStringLiteral("d")}});
    ASSERT_EQ(4, changes.size());
    EXPECT_EQ(Kind::Stopped, changes.at(0).kind);
    EXPECT_EQ(1, changes.at(0).player);
    EXPECT_EQ(Kind::Stopped, changes.at(1).kind);
    EXPECT_EQ(3, changes.at(1).player);
    EXPECT_EQ(Kind::Started, changes.at(2).kind);
    EXPECT_EQ(2, changes.at(2).player);
    EXPECT_EQ(Kind::Started, changes.at(3).kind);
    EXPECT_EQ(4, changes.at(3).player);
}

/// Mixxx going: everything that plays stops there.
TEST(PlayHistoryRecorderTest, StopAllStopsEverything) {
    Recorder recorder;
    recorder.update(0, {{kThisDeck, QStringLiteral("a")}, {2, QStringLiteral("b")}});
    const QList<Recorder::Change> changes = recorder.stopAll(9000);
    ASSERT_EQ(2, changes.size());
    for (const Recorder::Change& change : changes) {
        EXPECT_EQ(Kind::Stopped, change.kind);
        EXPECT_EQ(9000, change.atMs);
    }
    EXPECT_TRUE(recorder.playing().isEmpty());
}

/// The recorder's plays, swept: Sam's example, sampled as DeckHistory samples
/// it, gives his list.
TEST(PlayHistoryRecorderTest, SamsExampleSampledGivesHisList) {
    struct Step {
        double atSeconds;
        QList<Sample> playing;
    };
    const QString t1 = QStringLiteral("t1");
    const QString t2 = QStringLiteral("t2");
    const QString t3 = QStringLiteral("t3");
    const QList<Step> steps = {
            {0, {{1, t1}}},
            {10, {{1, t1}, {2, t2}}},
            {30, {{2, t2}}},
            {70, {{2, t2}, {3, t3}}},
            {80, {{2, t2}}},
            {160, {{3, t3}}},
            {280, {}},
    };
    Recorder recorder;
    QHash<QString, int> trackNumbers{{t1, 1}, {t2, 2}, {t3, 3}};
    QHash<int, Play> open;
    QList<Play> plays;
    for (const Step& step : steps) {
        const qint64 now = static_cast<qint64>(step.atSeconds * kSecond);
        for (const Recorder::Change& change : recorder.update(now, step.playing)) {
            if (change.kind == Kind::Started) {
                Play p;
                p.player = change.player;
                p.track = trackNumbers.value(change.trackKey);
                p.startMs = change.atMs;
                open.insert(change.player, p);
            } else {
                Play p = open.take(change.player);
                p.endMs = change.atMs;
                plays.append(p);
            }
        }
    }
    EXPECT_TRUE(open.isEmpty());
    EXPECT_EQ(QStringList({
                      QStringLiteral("1 (10s)"),
                      QStringLiteral("1 | 2 (20s)"),
                      QStringLiteral("2 (40s)"),
                      QStringLiteral("2 | 3 (10s)"),
                      QStringLiteral("2 (1m20s)"),
                      QStringLiteral("3 (2m)"),
              }),
            listed(plays));
}

} // namespace
