#include "library/deck/autoplay.h"

#include <gtest/gtest.h>

#include <QRandomGenerator>
#include <QSet>
#include <QStringList>
#include <utility>

namespace {

using mixxx::deck::MediumId;
using mixxx::deck::autoplay::Candidate;
using mixxx::deck::autoplay::nearestUnplayed;
using mixxx::deck::autoplay::next;
using mixxx::deck::autoplay::Pick;
using mixxx::deck::autoplay::PlayedMemory;
using mixxx::deck::autoplay::scopeKey;
using mixxx::deck::autoplay::trackKey;

/// A genre whose tracks are named after their tempo, which keeps the
/// expectations below readable: "126" is the track at 126 BPM. A track with no
/// tempo (0) is named after its place instead, so two of them are two tracks.
QList<Candidate> genreOf(const QList<double>& bpms) {
    QList<Candidate> candidates;
    for (int i = 0; i < bpms.size(); ++i) {
        Candidate candidate;
        candidate.rowId = i + 1;
        candidate.key = bpms.at(i) > 0
                ? QStringLiteral("/Music/%1.mp3").arg(bpms.at(i))
                : QStringLiteral("/Music/untagged-%1.mp3").arg(i);
        for (const Candidate& earlier : std::as_const(candidates)) {
            if (earlier.key == candidate.key) {
                // Two tempos that print alike: still two tracks.
                candidate.key = QStringLiteral("/Music/%1-%2.mp3").arg(bpms.at(i)).arg(i);
            }
        }
        candidate.bpm = bpms.at(i);
        candidates.append(candidate);
    }
    return candidates;
}

QString keyOf(double bpm) {
    return QStringLiteral("/Music/%1.mp3").arg(bpm);
}

double bpmAt(const QList<Candidate>& candidates, int index) {
    return index < 0 ? -1.0 : candidates.at(index).bpm;
}

const QString kScope = QStringLiteral("uuid:1234-ABCD\nHouse");

// ---------------------------------------------------------------------------
// The played memory: per drive and genre, until it is cleared.
// ---------------------------------------------------------------------------

TEST(AutoplayMemoryTest, RemembersPerScope) {
    PlayedMemory memory;
    const QString house = QStringLiteral("usb:/media/DJ_USB_1\nHouse");
    const QString techno = QStringLiteral("usb:/media/DJ_USB_1\nTechno");

    memory.insert(house, QStringLiteral("/a.mp3"));
    memory.insert(house, QStringLiteral("/b.mp3"));
    memory.insert(techno, QStringLiteral("/a.mp3"));

    EXPECT_TRUE(memory.contains(house, QStringLiteral("/a.mp3")));
    EXPECT_TRUE(memory.contains(house, QStringLiteral("/b.mp3")));
    EXPECT_FALSE(memory.contains(house, QStringLiteral("/c.mp3")));
    EXPECT_EQ(2, memory.played(house).size());
    // The same file under another genre is another genre's business.
    EXPECT_TRUE(memory.contains(techno, QStringLiteral("/a.mp3")));
    EXPECT_FALSE(memory.contains(techno, QStringLiteral("/b.mp3")));
}

TEST(AutoplayMemoryTest, ANewRoundForgetsOnlyItsOwnScope) {
    PlayedMemory memory;
    const QString house = QStringLiteral("usb:/media/DJ_USB_1\nHouse");
    const QString techno = QStringLiteral("usb:/media/DJ_USB_1\nTechno");
    memory.insert(house, QStringLiteral("/a.mp3"));
    memory.insert(techno, QStringLiteral("/b.mp3"));

    memory.clear(house);

    EXPECT_TRUE(memory.played(house).isEmpty());
    EXPECT_TRUE(memory.contains(techno, QStringLiteral("/b.mp3")));
}

TEST(AutoplayMemoryTest, AStickInTheOtherPortIsTheSameStick) {
    // A new mount point is a new medium id, but the UUID says it is the same
    // stick -- so what played off it is still remembered.
    const MediumId first = MediumId::local(QStringLiteral("/media/DJ_USB_1"),
            QStringLiteral("1234-ABCD"));
    const MediumId second = MediumId::local(QStringLiteral("/media/DJ_USB_2"),
            QStringLiteral("1234-ABCD"));
    const MediumId other = MediumId::local(QStringLiteral("/media/DJ_USB_1"),
            QStringLiteral("9999-0000"));

    EXPECT_EQ(scopeKey(first, QStringLiteral("House")), scopeKey(second, QStringLiteral("House")));
    EXPECT_NE(scopeKey(first, QStringLiteral("House")), scopeKey(other, QStringLiteral("House")));
    EXPECT_NE(scopeKey(first, QStringLiteral("House")), scopeKey(first, QStringLiteral("Techno")));
    // No genre is a genre of its own -- the "—" row -- and not every genre.
    EXPECT_NE(scopeKey(first, QString()), scopeKey(first, QStringLiteral("House")));
}

TEST(AutoplayMemoryTest, AStickWithoutAUuidOrAPlayersSlotIsItsMediumKey) {
    const MediumId bare = MediumId::local(QStringLiteral("/media/DJ_USB_1"));
    const MediumId remote = MediumId::proLink(QStringLiteral("c8:3d:fc:0a:11:22"), 3);

    EXPECT_EQ(bare.key() + QStringLiteral("\nHouse"), scopeKey(bare, QStringLiteral("House")));
    EXPECT_EQ(remote.key() + QStringLiteral("\nHouse"), scopeKey(remote, QStringLiteral("House")));
}

TEST(AutoplayMemoryTest, ATrackIsItsPathOnTheDrive) {
    EXPECT_EQ(QStringLiteral("/House/a.mp3"),
            trackKey(QStringLiteral("/media/DJ_USB_2/House/a.mp3"),
                    QStringLiteral("/media/DJ_USB_2"),
                    7));
    // The same file off the same stick in the other port.
    EXPECT_EQ(trackKey(QStringLiteral("/media/DJ_USB_1/House/a.mp3"),
                      QStringLiteral("/media/DJ_USB_1"),
                      7),
            trackKey(QStringLiteral("/media/DJ_USB_2/House/a.mp3"),
                    QStringLiteral("/media/DJ_USB_2"),
                    9));
    // Nothing to strip: the whole path. No path at all: the rekordbox id.
    EXPECT_EQ(QStringLiteral("/elsewhere/a.mp3"),
            trackKey(QStringLiteral("/elsewhere/a.mp3"), QStringLiteral("/media/DJ_USB_1"), 7));
    EXPECT_EQ(QStringLiteral("#7"), trackKey(QString(), QStringLiteral("/media/DJ_USB_1"), 7));
}

// ---------------------------------------------------------------------------
// The rule: nearest in BPM among the unplayed; ties at random; tracks with no
// BPM last.
// ---------------------------------------------------------------------------

TEST(AutoplayRuleTest, PicksTheNearestTempo) {
    QRandomGenerator random(1);
    const QList<Candidate> genre = genreOf({100, 118, 124, 131, 170});
    EXPECT_EQ(124, bpmAt(genre, nearestUnplayed(genre, {}, 125.0, &random)));
    EXPECT_EQ(118, bpmAt(genre, nearestUnplayed(genre, {}, 119.9, &random)));
    EXPECT_EQ(170, bpmAt(genre, nearestUnplayed(genre, {}, 200.0, &random)));
    EXPECT_EQ(100, bpmAt(genre, nearestUnplayed(genre, {}, 60.0, &random)));
}

TEST(AutoplayRuleTest, SkipsWhatHasPlayed) {
    QRandomGenerator random(1);
    const QList<Candidate> genre = genreOf({100, 118, 124, 131, 170});
    EXPECT_EQ(131, bpmAt(genre, nearestUnplayed(genre, {keyOf(124), keyOf(118)}, 125.0, &random)));
}

TEST(AutoplayRuleTest, BreaksTiesAtRandom) {
    const QList<Candidate> genre = genreOf({120, 124, 126, 140});
    QSet<double> seen;
    for (quint32 seed = 0; seed < 64; ++seed) {
        QRandomGenerator random(seed);
        const double picked = bpmAt(genre, nearestUnplayed(genre, {}, 125.0, &random));
        // One of the two a BPM away, and never anything farther.
        ASSERT_TRUE(picked == 124 || picked == 126) << picked;
        seen.insert(picked);
    }
    EXPECT_EQ(2, seen.size()) << "both of a tie get their turn";
}

TEST(AutoplayRuleTest, TheSameTempoIsATie) {
    // Two tracks at what is, to anyone listening, the same tempo: a tie, not
    // whichever one a rounding error favours.
    const QList<Candidate> genre = genreOf({126.0, 126.0004, 128.0});
    QSet<int> seen;
    for (quint32 seed = 0; seed < 64; ++seed) {
        QRandomGenerator random(seed);
        const int index = nearestUnplayed(genre, {}, 126.2, &random);
        ASSERT_TRUE(index == 0 || index == 1) << index;
        seen.insert(index);
    }
    EXPECT_EQ(2, seen.size());
}

TEST(AutoplayRuleTest, TracksWithNoTempoComeLast) {
    QRandomGenerator random(1);
    // Two untagged tracks and one far away in tempo: the far one first, since
    // the untagged ones are not near anything at all.
    const QList<Candidate> genre = genreOf({0, 172, 0});
    EXPECT_EQ(172, bpmAt(genre, nearestUnplayed(genre, {}, 124.0, &random)));

    // Then the untagged ones, at random, once nothing with a tempo is left.
    QSet<int> seen;
    for (quint32 seed = 0; seed < 64; ++seed) {
        QRandomGenerator seeded(seed);
        const int index = nearestUnplayed(genre, {keyOf(172)}, 124.0, &seeded);
        ASSERT_TRUE(index == 0 || index == 2) << index;
        seen.insert(index);
    }
    EXPECT_EQ(2, seen.size());
}

TEST(AutoplayRuleTest, NoReferenceMeansAnyTrackWithATempo) {
    // The starting track had no tempo. Anything with one may follow it, and
    // still nothing without one while something with one is left.
    const QList<Candidate> genre = genreOf({118, 0, 131});
    QSet<int> seen;
    for (quint32 seed = 0; seed < 64; ++seed) {
        QRandomGenerator random(seed);
        const int index = nearestUnplayed(genre, {}, 0.0, &random);
        ASSERT_TRUE(index == 0 || index == 2) << index;
        seen.insert(index);
    }
    EXPECT_EQ(2, seen.size());
}

TEST(AutoplayRuleTest, NothingLeftIsMinusOne) {
    QRandomGenerator random(1);
    const QList<Candidate> genre = genreOf({118, 124});
    EXPECT_EQ(-1, nearestUnplayed(genre, {keyOf(118), keyOf(124)}, 120.0, &random));
    EXPECT_EQ(-1, nearestUnplayed({}, {}, 120.0, &random));
}

// ---------------------------------------------------------------------------
// One step at a time, rounds included.
// ---------------------------------------------------------------------------

TEST(AutoplayRoundTest, AWalkFollowsTheNearestTempoAndRecordsIt) {
    QRandomGenerator random(1);
    const QList<Candidate> genre = genreOf({100, 112, 120, 126, 141});
    PlayedMemory memory;
    // The DJ started on 120: it counts as played.
    memory.insert(kScope, keyOf(120));

    QList<double> walk;
    QString current = keyOf(120);
    double reference = 120;
    for (int step = 0; step < 4; ++step) {
        const Pick pick = next(genre, &memory, kScope, current, reference, &random);
        ASSERT_GE(pick.index, 0);
        EXPECT_FALSE(pick.newRound);
        EXPECT_EQ(4 - step, pick.unplayed);
        walk.append(genre.at(pick.index).bpm);
        current = genre.at(pick.index).key;
        reference = genre.at(pick.index).bpm;
        EXPECT_TRUE(memory.contains(kScope, current));
    }
    // 126 is 6 away from 120 and 112 is 8; then 112 (14) beats 141 (15); then
    // 100 (12); then the only one left. Drifting, as Sam said it may.
    EXPECT_EQ((QList<double>{126, 112, 100, 141}), walk);
}

TEST(AutoplayRoundTest, NoTrackRepeatsWithinARoundAndThenARoundStarts) {
    const QList<Candidate> genre = genreOf({96, 102, 118, 121, 124, 124.5, 127, 0, 0});
    for (quint32 seed = 0; seed < 16; ++seed) {
        QRandomGenerator random(seed);
        PlayedMemory memory;
        QString current = keyOf(121);
        double reference = 121;
        memory.insert(kScope, current);

        // Two rounds' worth. The first is the 8 after the starting track.
        QStringList round;
        round.append(current);
        int rounds = 1;
        for (int step = 0; step < 2 * genre.size(); ++step) {
            const Pick pick = next(genre, &memory, kScope, current, reference, &random);
            ASSERT_GE(pick.index, 0);
            const QString key = genre.at(pick.index).key;
            if (pick.newRound) {
                // Only once every track of the genre has had its turn.
                ASSERT_EQ(genre.size(), round.size()) << "seed " << seed;
                EXPECT_EQ(genre.size(), pick.unplayed);
                // And not opened on the track that has just played.
                EXPECT_NE(current, key);
                round.clear();
                ++rounds;
            }
            ASSERT_FALSE(round.contains(key)) << "seed " << seed << ": " << qPrintable(key) << " twice";
            round.append(key);
            current = key;
            reference = genre.at(pick.index).bpm;
        }
        EXPECT_EQ(3, rounds) << "seed " << seed;
    }
}

TEST(AutoplayRoundTest, ANewRoundOpensNearTheLastTrack) {
    QRandomGenerator random(1);
    const QList<Candidate> genre = genreOf({100, 120, 126});
    PlayedMemory memory;
    for (const Candidate& candidate : genre) {
        memory.insert(kScope, candidate.key);
    }
    // 126 has just played and was the last: the new round opens on 120, the
    // nearest to it, rather than on 126 again.
    const Pick pick = next(genre, &memory, kScope, keyOf(126), 126, &random);
    EXPECT_TRUE(pick.newRound);
    EXPECT_EQ(120, bpmAt(genre, pick.index));
    EXPECT_EQ((QSet<QString>{keyOf(120)}), memory.played(kScope));
}

TEST(AutoplayRoundTest, AGenreOfOneFollowsItself) {
    QRandomGenerator random(1);
    const QList<Candidate> genre = genreOf({124});
    PlayedMemory memory;
    memory.insert(kScope, keyOf(124));
    const Pick pick = next(genre, &memory, kScope, keyOf(124), 124, &random);
    EXPECT_TRUE(pick.newRound);
    EXPECT_EQ(0, pick.index);
}

TEST(AutoplayRoundTest, AnEmptyGenreHasNothingNext) {
    QRandomGenerator random(1);
    PlayedMemory memory;
    const Pick pick = next({}, &memory, kScope, keyOf(124), 124, &random);
    EXPECT_EQ(-1, pick.index);
    EXPECT_FALSE(pick.newRound);
}

} // namespace
