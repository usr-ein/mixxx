#include "library/deck/historystore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

namespace {

using mixxx::deck::HistoryPlay;
using mixxx::deck::HistorySession;
using mixxx::deck::HistoryStore;
using mixxx::deck::HistoryTrack;
using mixxx::deck::HistoryWrite;

constexpr qint64 kSecond = 1000;

/// The History's store in a directory of its own, as the card's would be.
class HistoryStoreTest : public testing::Test {
  protected:
    QString directory() const {
        return m_directory.path();
    }

    /// One power-up: open for *bootId*, and close again.
    HistoryStore::Opened boot(const QByteArray& bootId) {
        HistoryStore store;
        HistoryStore::Opened opened = store.open(directory(), bootId, 0);
        store.close();
        return opened;
    }

    /// One power-up in which *seconds* of track *title* played.
    void playedIn(const QByteArray& bootId, const QString& title, double seconds) {
        HistoryStore store;
        const HistoryStore::Opened opened = store.open(directory(), bootId, 0);
        ASSERT_TRUE(opened.ok);
        const int session = opened.sessions.first().id;
        const int next = opened.sessions.first().plays.size() + 1;
        HistoryWrite write;
        HistoryTrack track;
        track.id = next;
        track.key = title;
        track.title = title;
        write.newTracks.append(track);
        HistoryPlay play;
        play.id = next;
        play.play.player = 1;
        play.play.track = next;
        play.play.startMs = 0;
        play.play.endMs = static_cast<qint64>(seconds * kSecond);
        write.newPlays.append(play);
        store.write(session, write);
        store.close();
    }

    static QStringList bootsOf(const HistoryStore::Opened& opened) {
        QStringList boots;
        for (const HistorySession& session : opened.sessions) {
            boots.append(QString::fromLatin1(session.bootId));
        }
        return boots;
    }

    QTemporaryDir m_directory;
};

// ---------------------------------------------------------------------------
// Rotation: a session per power-up, three kept.
// ---------------------------------------------------------------------------

TEST_F(HistoryStoreTest, AFirstBootStartsTheFirstSession) {
    const HistoryStore::Opened opened = boot("boot-1");
    ASSERT_TRUE(opened.ok) << opened.error.toStdString();
    EXPECT_EQ(QStringList({QStringLiteral("boot-1")}), bootsOf(opened));
    EXPECT_TRUE(opened.sessions.first().plays.isEmpty());
}

/// Four power-ups that played: the fourth drops the first, with what played
/// in it.
TEST_F(HistoryStoreTest, AFourthBootDropsTheOldest) {
    playedIn("boot-1", QStringLiteral("one"), 60);
    playedIn("boot-2", QStringLiteral("two"), 60);
    playedIn("boot-3", QStringLiteral("three"), 60);
    const HistoryStore::Opened opened = boot("boot-4");
    ASSERT_TRUE(opened.ok);
    EXPECT_EQ(QStringList({
                      QStringLiteral("boot-4"),
                      QStringLiteral("boot-3"),
                      QStringLiteral("boot-2"),
              }),
            bootsOf(opened));
    // This one is new and empty; the last two kept what played in them.
    EXPECT_TRUE(opened.sessions.at(0).plays.isEmpty());
    ASSERT_EQ(1, opened.sessions.at(1).tracks.size());
    EXPECT_EQ(QStringLiteral("three"), opened.sessions.at(1).tracks.first().title);
    EXPECT_EQ(QStringLiteral("two"), opened.sessions.at(2).tracks.first().title);
}

/// Mixxx started again within one power-up -- a crash, a deploy -- carries on
/// that power-up's session rather than starting one.
TEST_F(HistoryStoreTest, TheSameBootCarriesOnItsSession) {
    playedIn("boot-1", QStringLiteral("one"), 60);
    playedIn("boot-1", QStringLiteral("two"), 30);
    const HistoryStore::Opened opened = boot("boot-1");
    ASSERT_EQ(1, opened.sessions.size());
    EXPECT_EQ(2, opened.sessions.first().plays.size());
    EXPECT_EQ(2, opened.sessions.first().tracks.size());
}

/// A power-up in which nothing played makes way for the next one, not the
/// oldest: switching the deck on and off twice after a gig keeps the gig.
TEST_F(HistoryStoreTest, APowerUpThatPlayedNothingMakesWayForTheNext) {
    playedIn("boot-1", QStringLiteral("the gig"), 3600);
    playedIn("boot-2", QStringLiteral("the soundcheck"), 600);
    boot("boot-3");
    boot("boot-4");
    const HistoryStore::Opened opened = boot("boot-5");
    EXPECT_EQ(QStringList({
                      QStringLiteral("boot-5"),
                      QStringLiteral("boot-2"),
                      QStringLiteral("boot-1"),
              }),
            bootsOf(opened));
}

/// Nothing listed is nothing played, whatever the plays add up to: two
/// tracks of a second and a half each are two rows too short to show, and
/// the power-up they were in makes way like an empty one.
TEST_F(HistoryStoreTest, APowerUpListedAsNothingMakesWayToo) {
    playedIn("boot-1", QStringLiteral("the gig"), 3600);
    {
        HistoryStore store;
        const HistoryStore::Opened opened = store.open(directory(), "boot-2", 0);
        ASSERT_TRUE(opened.ok);
        HistoryWrite write;
        for (int i = 1; i <= 2; ++i) {
            HistoryTrack track;
            track.id = i;
            track.key = QString::number(i);
            write.newTracks.append(track);
            HistoryPlay play;
            play.id = i;
            play.play.player = i;
            play.play.track = i;
            play.play.startMs = i * 10 * kSecond;
            play.play.endMs = i * 10 * kSecond + 1500;
            write.newPlays.append(play);
        }
        store.write(opened.sessions.first().id, write);
        store.close();
    }
    const HistoryStore::Opened opened = boot("boot-3");
    EXPECT_EQ(QStringList({QStringLiteral("boot-3"), QStringLiteral("boot-1")}),
            bootsOf(opened));
}

/// A CUE tapped is not a power-up that played.
TEST_F(HistoryStoreTest, ASecondOfPlayIsNothingPlayed) {
    playedIn("boot-1", QStringLiteral("the gig"), 3600);
    playedIn("boot-2", QStringLiteral("a tap"), 1);
    const HistoryStore::Opened opened = boot("boot-3");
    EXPECT_EQ(QStringList({QStringLiteral("boot-3"), QStringLiteral("boot-1")}),
            bootsOf(opened));
}

// ---------------------------------------------------------------------------
// What a write leaves, and what a cut leaves.
// ---------------------------------------------------------------------------

TEST_F(HistoryStoreTest, AWriteIsReadBackWhole) {
    HistoryStore store;
    HistoryStore::Opened opened = store.open(directory(), "boot-1", 1234);
    ASSERT_TRUE(opened.ok);
    const int session = opened.sessions.first().id;

    HistoryWrite started;
    HistoryTrack track;
    track.id = 1;
    track.key = QStringLiteral("key-1");
    started.newTracks.append(track);
    HistoryPlay play;
    play.id = 1;
    play.play.player = 3;
    play.play.track = 1;
    play.play.startMs = 5 * kSecond;
    play.play.endMs = 5 * kSecond;
    started.newPlays.append(play);
    store.write(session, started);

    // The name arrived later; the play went on.
    HistoryWrite later;
    track.title = QStringLiteral("Title");
    track.artist = QStringLiteral("Artist");
    track.sourceName = QStringLiteral("SAM1");
    track.sourcePlayer = 2;
    track.sourceSlot = QStringLiteral("usb");
    track.artwork = QStringLiteral("abc.jpg");
    later.changedTracks.append(track);
    later.ends.append({1, 65 * kSecond});
    store.write(session, later);
    store.close();

    opened = boot("boot-1");
    ASSERT_EQ(1, opened.sessions.size());
    const HistorySession& read = opened.sessions.first();
    EXPECT_EQ(1234, read.openedWallMs);
    ASSERT_EQ(1, read.tracks.size());
    EXPECT_EQ(QStringLiteral("key-1"), read.tracks.first().key);
    EXPECT_EQ(QStringLiteral("Title"), read.tracks.first().title);
    EXPECT_EQ(QStringLiteral("Artist"), read.tracks.first().artist);
    EXPECT_EQ(QStringLiteral("SAM1"), read.tracks.first().sourceName);
    EXPECT_EQ(2, read.tracks.first().sourcePlayer);
    EXPECT_EQ(QStringLiteral("usb"), read.tracks.first().sourceSlot);
    EXPECT_EQ(QStringLiteral("abc.jpg"), read.tracks.first().artwork);
    ASSERT_EQ(1, read.plays.size());
    EXPECT_EQ(3, read.plays.first().play.player);
    EXPECT_EQ(5 * kSecond, read.plays.first().play.startMs);
    EXPECT_EQ(65 * kSecond, read.plays.first().play.endMs);
}

/// The card as a cut leaves it: the files exactly as they are on disk while
/// Mixxx has them open, never closed. Everything written is there; the next
/// boot finds it as the last session.
TEST_F(HistoryStoreTest, ACutKeepsEverythingWritten) {
    HistoryStore store;
    const HistoryStore::Opened opened = store.open(directory(), "boot-1", 0);
    ASSERT_TRUE(opened.ok);
    const int session = opened.sessions.first().id;
    HistoryWrite write;
    HistoryTrack track;
    track.id = 1;
    track.key = QStringLiteral("k");
    track.title = QStringLiteral("Playing when the plug was pulled");
    write.newTracks.append(track);
    HistoryPlay play;
    play.id = 1;
    play.play.track = 1;
    play.play.startMs = 0;
    play.play.endMs = 0;
    write.newPlays.append(play);
    store.write(session, write);
    // The heartbeats, the last one five seconds before the cut.
    for (int beat = 1; beat <= 10; ++beat) {
        HistoryWrite heartbeat;
        heartbeat.ends.append({1, beat * 5 * kSecond});
        store.write(session, heartbeat);
    }

    QTemporaryDir afterTheCut;
    for (const QString& name : QDir(directory()).entryList(QDir::Files)) {
        // Not the shared-memory index: that is RAM's, and the next open
        // rebuilds it from the log.
        if (!name.endsWith(QStringLiteral("-shm"))) {
            ASSERT_TRUE(QFile::copy(QDir(directory()).filePath(name),
                    QDir(afterTheCut.path()).filePath(name)));
        }
    }
    store.close();

    HistoryStore next;
    const HistoryStore::Opened reopened = next.open(afterTheCut.path(), "boot-2", 0);
    next.close();
    ASSERT_TRUE(reopened.ok);
    ASSERT_EQ(2, reopened.sessions.size());
    const HistorySession& last = reopened.sessions.at(1);
    EXPECT_EQ(QByteArray("boot-1"), last.bootId);
    ASSERT_EQ(1, last.plays.size());
    EXPECT_EQ(50 * kSecond, last.plays.first().play.endMs);
    EXPECT_EQ(QStringLiteral("Playing when the plug was pulled"), last.tracks.first().title);
}

/// A cut in the middle of a commit leaves the log with a torn end. The commit
/// is lost, and nothing before it.
TEST_F(HistoryStoreTest, ATornWriteLosesOnlyItself) {
    HistoryStore store;
    const HistoryStore::Opened opened = store.open(directory(), "boot-1", 0);
    ASSERT_TRUE(opened.ok);
    const int session = opened.sessions.first().id;
    HistoryWrite write;
    HistoryTrack track;
    track.id = 1;
    track.key = QStringLiteral("k");
    write.newTracks.append(track);
    HistoryPlay play;
    play.id = 1;
    play.play.track = 1;
    play.play.endMs = 30 * kSecond;
    write.newPlays.append(play);
    store.write(session, write);

    QTemporaryDir afterTheCut;
    for (const QString& name : QDir(directory()).entryList(QDir::Files)) {
        if (!name.endsWith(QStringLiteral("-shm"))) {
            ASSERT_TRUE(QFile::copy(QDir(directory()).filePath(name),
                    QDir(afterTheCut.path()).filePath(name)));
        }
    }
    store.close();
    // Half a frame of a commit that never finished.
    QFile log(QDir(afterTheCut.path()).filePath(QStringLiteral("history.sqlite-wal")));
    ASSERT_TRUE(log.exists());
    ASSERT_TRUE(log.open(QIODevice::Append));
    log.write(QByteArray(2000, '\x5a'));
    log.close();

    HistoryStore next;
    const HistoryStore::Opened reopened = next.open(afterTheCut.path(), "boot-2", 0);
    next.close();
    ASSERT_TRUE(reopened.ok) << reopened.error.toStdString();
    ASSERT_EQ(2, reopened.sessions.size());
    ASSERT_EQ(1, reopened.sessions.at(1).plays.size());
    EXPECT_EQ(30 * kSecond, reopened.sessions.at(1).plays.first().play.endMs);
}

/// A database that is not one any more is put aside, and the History starts
/// again: a broken file costs the History, never the deck.
TEST_F(HistoryStoreTest, ABrokenDatabaseIsPutAsideAndStartedAgain) {
    playedIn("boot-1", QStringLiteral("one"), 60);
    const QString path = QDir(directory()).filePath(QStringLiteral("history.sqlite"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArray(8192, '\x42'));
    }
    const HistoryStore::Opened opened = boot("boot-2");
    ASSERT_TRUE(opened.ok) << opened.error.toStdString();
    EXPECT_EQ(QStringList({QStringLiteral("boot-2")}), bootsOf(opened));
    EXPECT_TRUE(QFile::exists(path + QStringLiteral(".broken")));
    // And it takes writes again.
    playedIn("boot-2", QStringLiteral("two"), 60);
    EXPECT_EQ(1, boot("boot-2").sessions.first().plays.size());
}

/// Nowhere to write -- no card directory -- is no store, and writes are
/// nothing rather than a crash.
TEST_F(HistoryStoreTest, NoDirectoryIsNoStore) {
    HistoryStore store;
    const HistoryStore::Opened opened = store.open(
            QDir(directory()).filePath(QStringLiteral("not-there")), "boot-1", 0);
    EXPECT_FALSE(opened.ok);
    EXPECT_FALSE(opened.error.isEmpty());
    HistoryWrite write;
    write.ends.append({1, kSecond});
    store.write(1, write);
    store.close();
}

// ---------------------------------------------------------------------------
// Thumbnails.
// ---------------------------------------------------------------------------

/// A cover is kept as a square thumbnail no bigger than kArtworkSize, and
/// goes when no session names it any more.
TEST_F(HistoryStoreTest, ArtworkIsAThumbnailThatGoesWithItsSessions) {
    QTemporaryDir stick;
    const QString cover = QDir(stick.path()).filePath(QStringLiteral("cover.png"));
    QImage image(500, 300, QImage::Format_RGB32);
    image.fill(Qt::red);
    ASSERT_TRUE(image.save(cover));

    HistoryStore store;
    const HistoryStore::Opened opened = store.open(directory(), "boot-1", 0);
    ASSERT_TRUE(opened.ok);
    int saved = 0;
    QObject::connect(&store, &HistoryStore::artworkSaved, [&saved]() { ++saved; });
    store.saveArtwork(cover, QStringLiteral("t1.jpg"));
    ASSERT_EQ(1, saved);
    const QImage thumbnail(store.artworkPath(QStringLiteral("t1.jpg")));
    ASSERT_FALSE(thumbnail.isNull());
    EXPECT_EQ(HistoryStore::kArtworkSize, thumbnail.width());
    EXPECT_EQ(HistoryStore::kArtworkSize, thumbnail.height());
    EXPECT_LT(QFileInfo(store.artworkPath(QStringLiteral("t1.jpg"))).size(), 20 * 1024);

    HistoryWrite write;
    HistoryTrack track;
    track.id = 1;
    track.key = QStringLiteral("k");
    track.artwork = QStringLiteral("t1.jpg");
    write.newTracks.append(track);
    HistoryPlay play;
    play.id = 1;
    play.play.track = 1;
    play.play.endMs = 60 * kSecond;
    write.newPlays.append(play);
    store.write(opened.sessions.first().id, write);
    // One nobody names: a cut between the thumbnail and its row.
    store.saveArtwork(cover, QStringLiteral("orphan.jpg"));
    const QString kept = store.artworkPath(QStringLiteral("t1.jpg"));
    const QString orphan = store.artworkPath(QStringLiteral("orphan.jpg"));
    store.close();

    playedIn("boot-2", QStringLiteral("two"), 60);
    EXPECT_TRUE(QFile::exists(kept));
    EXPECT_FALSE(QFile::exists(orphan));
    playedIn("boot-3", QStringLiteral("three"), 60);
    boot("boot-4");
    // boot-1 has gone, and its cover with it.
    EXPECT_FALSE(QFile::exists(kept));
}

/// A small cover is not blown up.
TEST_F(HistoryStoreTest, ASmallCoverStaysItsSize) {
    QTemporaryDir stick;
    const QString cover = QDir(stick.path()).filePath(QStringLiteral("cover.png"));
    QImage image(80, 80, QImage::Format_RGB32);
    image.fill(Qt::blue);
    ASSERT_TRUE(image.save(cover));
    HistoryStore store;
    ASSERT_TRUE(store.open(directory(), "boot-1", 0).ok);
    store.saveArtwork(cover, QStringLiteral("small.jpg"));
    const QImage thumbnail(store.artworkPath(QStringLiteral("small.jpg")));
    store.close();
    EXPECT_EQ(80, thumbnail.width());
    EXPECT_EQ(80, thumbnail.height());
}

/// What will not load as an image is no thumbnail, and says nothing.
TEST_F(HistoryStoreTest, NoImageIsNoThumbnail) {
    HistoryStore store;
    ASSERT_TRUE(store.open(directory(), "boot-1", 0).ok);
    int saved = 0;
    QObject::connect(&store, &HistoryStore::artworkSaved, [&saved]() { ++saved; });
    store.saveArtwork(QDir(directory()).filePath(QStringLiteral("nothing.jpg")),
            QStringLiteral("x.jpg"));
    EXPECT_EQ(0, saved);
    EXPECT_FALSE(QFile::exists(store.artworkPath(QStringLiteral("x.jpg"))));
    store.close();
}

// ---------------------------------------------------------------------------
// A session's plays, as the list takes them.
// ---------------------------------------------------------------------------

/// What is still playing ends now, for the list; what has stopped, where it
/// stopped.
TEST(HistorySessionTest, OpenPlaysEndNow) {
    HistorySession session;
    HistoryPlay stopped;
    stopped.id = 1;
    stopped.play.startMs = 0;
    stopped.play.endMs = 10 * kSecond;
    HistoryPlay playing;
    playing.id = 2;
    playing.play.startMs = 5 * kSecond;
    playing.play.endMs = 15 * kSecond; // the last heartbeat
    session.plays = {stopped, playing};
    const QList<mixxx::deck::history::Play> plays = session.playsUntil({2}, 18 * kSecond);
    ASSERT_EQ(2, plays.size());
    EXPECT_EQ(10 * kSecond, plays.at(0).endMs);
    EXPECT_EQ(18 * kSecond, plays.at(1).endMs);
    // A session that has ended is as it was written.
    EXPECT_EQ(15 * kSecond, session.playsUntil({}, -1).at(1).endMs);
}

} // namespace
