#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

#include "library/deck/ramstore.h"
#include "library/deck/streamingfile.h"
#include "library/deck/trackcache.h"

using mixxx::deck::MediumId;
using mixxx::deck::RamStore;
using mixxx::deck::StreamingFile;
using mixxx::deck::StreamingFileRegistry;
using mixxx::deck::TrackCache;

namespace {

// A stick's track, copied for the deck. What these pin down is the bugs they
// came from: a load copied the whole file on the GUI thread before the deck
// could play a note of it (eighteen seconds of a frozen deck for an AIFF off a
// USB 2 stick); copies started for rows the selection merely rested on took a
// slow stick's bandwidth from the track on the deck; and a copy that kept the
// stick to itself left none for a CDJ playing off it.
class TrackCacheTest : public testing::Test {
  protected:
    void SetUp() override {
        clearTier1();
        // 256 KiB a chunk, so a few megabytes take long enough to catch the
        // copy running.
        TrackCache::setChunkDelayForTest(15);
        m_pCache = std::make_unique<TrackCache>();
        QObject::connect(m_pCache.get(),
                &TrackCache::cached,
                [this](const QString& path, bool ok) {
                    m_reports.append({path, ok});
                });
    }

    void TearDown() override {
        m_pCache.reset();
        TrackCache::setChunkDelayForTest(0);
        TrackCache::setLeadForTest(0);
        clearTier1();
    }

    static void clearTier1() {
        QDir dir(RamStore::path(QStringLiteral("cache")));
        for (const QString& name : dir.entryList(QDir::Files)) {
            dir.remove(name);
        }
    }

    static QByteArray pattern(int size, int seed) {
        QByteArray bytes(size, Qt::Uninitialized);
        for (int i = 0; i < size; ++i) {
            bytes[i] = static_cast<char>((i * 31 + i / 4096 + seed) & 0xFF);
        }
        return bytes;
    }

    QString makeSource(const QString& name, const QByteArray& content) {
        const QString path = m_dir.filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(content);
        return path;
    }

    static QByteArray readFile(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    /// Runs the event loop until *done*, which is where a copy reports back.
    static bool waitFor(const std::function<bool()>& done, int timeoutMs = 20000) {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > timeoutMs) {
                return false;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
        }
        return true;
    }

    int reportsFor(const QString& path, bool ok) const {
        int count = 0;
        for (const auto& report : m_reports) {
            count += report.first == path && report.second == ok;
        }
        return count;
    }

    /// A read through the track's stream, off the GUI thread as a decoder's is.
    static QByteArray readThroughStream(const std::shared_ptr<StreamingFile>& pStream,
            qint64 offset,
            qint64 length,
            qint64* pElapsedMs = nullptr) {
        QByteArray out(static_cast<int>(length), 0);
        qint64 got = -1;
        QElapsedTimer timer;
        timer.start();
        QThread* pReader = QThread::create([&]() {
            got = pStream->read(offset, out.data(), length);
        });
        pReader->start();
        while (!pReader->wait(5)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        delete pReader;
        if (pElapsedMs) {
            *pElapsedMs = timer.elapsed();
        }
        return got == length ? out : QByteArray();
    }

    const MediumId m_medium = MediumId::local(QStringLiteral("/media/DJ_USB_1"),
            QStringLiteral("uuid:CAFE-0001"));
    QTemporaryDir m_dir;
    std::unique_ptr<TrackCache> m_pCache;
    QList<QPair<QString, bool>> m_reports;
};

TEST_F(TrackCacheTest, ALoadReturnsAtOnceAndPlaysWhileTheCopyArrives) {
    const QByteArray content = pattern(3 * 1024 * 1024 + 123, 1);
    const QString source = makeSource(QStringLiteral("track.aiff"), content);

    QElapsedTimer timer;
    timer.start();
    const QString local = m_pCache->startLocal(m_medium, source);
    // Twelve chunks at 15 ms each are coming; none of them is waited for here.
    EXPECT_LT(timer.elapsed(), 100);
    ASSERT_FALSE(local.isEmpty());
    EXPECT_TRUE(local.endsWith(QStringLiteral(".aiff")));
    const auto pStream = StreamingFileRegistry::lookup(local);
    ASSERT_TRUE(pStream);
    EXPECT_FALSE(m_pCache->isCached(m_medium, source));

    // The head, as a decoder opening the file reads it, long before the end.
    EXPECT_EQ(content.left(64 * 1024), readThroughStream(pStream, 0, 64 * 1024));

    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, true) == 1; }));
    EXPECT_EQ(content, readFile(local));
    EXPECT_TRUE(m_pCache->isCached(m_medium, source));
    // Whole: anything opening it now reads an ordinary file.
    EXPECT_FALSE(StreamingFileRegistry::lookup(local));
    EXPECT_FALSE(QFile::exists(local + QStringLiteral(".copying")));
    // And a second load is the copy that is here.
    EXPECT_EQ(local, m_pCache->startLocal(m_medium, source));
    EXPECT_EQ(1, m_reports.size());
}

TEST_F(TrackCacheTest, ALoadStopsTheCopyOfTheTrackItReplaces) {
    // The deck holds one track: the one it held before is not worth the stick.
    const QString first = makeSource(QStringLiteral("first.aiff"), pattern(8 * 1024 * 1024, 3));
    const QByteArray content = pattern(1024 * 1024, 4);
    const QString second = makeSource(QStringLiteral("second.aiff"), content);

    const QString firstLocal = m_pCache->startLocal(m_medium, first);
    const QString local = m_pCache->startLocal(m_medium, second);
    ASSERT_FALSE(local.isEmpty());

    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, true) == 1; }));
    ASSERT_TRUE(waitFor([&]() { return reportsFor(firstLocal, false) == 1; }));
    EXPECT_EQ(0, reportsFor(firstLocal, true));
    EXPECT_FALSE(QFile::exists(firstLocal));
    EXPECT_FALSE(StreamingFileRegistry::lookup(firstLocal));
    EXPECT_EQ(content, readFile(local));
}

TEST_F(TrackCacheTest, TheCopyEasesOffOnceItIsAheadOfTheDeck) {
    // Past the lead, each chunk is followed by three times its read in rest,
    // which leaves the stick to a CDJ reading it. 16 chunks of 10 ms: 160 ms
    // flat out; 4 at full speed (the 1 MiB lead) and 12 at 40 ms paced.
    TrackCache::setChunkDelayForTest(10);
    TrackCache::setLeadForTest(1024 * 1024);
    const QByteArray content = pattern(4 * 1024 * 1024, 11);
    const QString source = makeSource(QStringLiteral("paced.aiff"), content);
    QElapsedTimer timer;
    timer.start();
    const QString local = m_pCache->startLocal(m_medium, source);
    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, true) == 1; }));
    const qint64 elapsed = timer.elapsed();
    EXPECT_GE(elapsed, 350);
    EXPECT_LT(elapsed, 3000);
    EXPECT_EQ(content, readFile(local));
}

TEST_F(TrackCacheTest, ADeckThatCatchesUpIsServedAtFullSpeed) {
    // Resting is for when the deck is far ahead: a read that has to wait --
    // a seek past the copy -- ends the rest at once.
    TrackCache::setChunkDelayForTest(10);
    TrackCache::setLeadForTest(1024 * 1024);
    const int size = 8 * 1024 * 1024;
    const QByteArray content = pattern(size, 12);
    const QString source = makeSource(QStringLiteral("seek.aiff"), content);
    const QString local = m_pCache->startLocal(m_medium, source);
    const auto pStream = StreamingFileRegistry::lookup(local);
    ASSERT_TRUE(pStream);
    QThread::msleep(200); // well into the paced part
    const qint64 offset = 6 * 1024 * 1024 + 512;
    qint64 elapsedMs = 0;
    EXPECT_EQ(content.mid(static_cast<int>(offset), 4096),
            readThroughStream(pStream, offset, 4096, &elapsedMs));
    EXPECT_LT(elapsedMs, 120);
    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, true) == 1; }));
    EXPECT_EQ(content, readFile(local));
}

TEST_F(TrackCacheTest, AReadAheadOfTheCopyIsFetchedNext) {
    // A hot cue two thirds of the way in, pressed the moment the track loads:
    // its bytes come next, not after everything before them.
    const int size = 12 * 1024 * 1024; // 48 chunks, ~0.7 s of copying
    const QByteArray content = pattern(size, 5);
    const QString source = makeSource(QStringLiteral("long.aiff"), content);
    const QString local = m_pCache->startLocal(m_medium, source);
    const auto pStream = StreamingFileRegistry::lookup(local);
    ASSERT_TRUE(pStream);

    const qint64 offset = 8 * 1024 * 1024 + 4096;
    qint64 elapsedMs = 0;
    EXPECT_EQ(content.mid(static_cast<int>(offset), 32 * 1024),
            readThroughStream(pStream, offset, 32 * 1024, &elapsedMs));
    // In order it would have waited for 32 chunks; out of order, a few.
    EXPECT_LT(elapsedMs, 250);

    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, true) == 1; }));
    EXPECT_EQ(content, readFile(local));
}

TEST_F(TrackCacheTest, ReleasingTheDecksTrackStopsItsCopy) {
    const QString source = makeSource(QStringLiteral("gone.aiff"), pattern(8 * 1024 * 1024, 8));
    const QString local = m_pCache->startLocal(m_medium, source);
    const auto pStream = StreamingFileRegistry::lookup(local);
    ASSERT_TRUE(pStream);

    m_pCache->release(local);
    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, false) == 1; }));
    EXPECT_FALSE(QFile::exists(local));
    EXPECT_FALSE(StreamingFileRegistry::lookup(local));
    // A reader still holding it is told, rather than left waiting.
    EXPECT_FALSE(pStream->error().isEmpty());
}

TEST_F(TrackCacheTest, AStickThatCannotBeReadIsRefused) {
    EXPECT_TRUE(m_pCache->startLocal(m_medium, m_dir.filePath(QStringLiteral("absent.mp3"))).isEmpty());
    EXPECT_TRUE(m_pCache->startLocal(m_medium, QString()).isEmpty());
}

TEST_F(TrackCacheTest, ACopyCutShortIsNotTakenForAWholeOneNextTime) {
    // Mixxx stopped mid-copy: the file is there at its full size, with holes
    // that would play as silence.
    const QString source = makeSource(QStringLiteral("cut.aiff"), pattern(8 * 1024 * 1024, 9));
    const QString local = m_pCache->startLocal(m_medium, source);
    ASSERT_FALSE(local.isEmpty());
    m_pCache.reset();
    ASSERT_TRUE(QFile::exists(local));
    ASSERT_TRUE(QFile::exists(local + QStringLiteral(".copying")));

    m_pCache = std::make_unique<TrackCache>();
    EXPECT_FALSE(QFile::exists(local));
    EXPECT_FALSE(QFile::exists(local + QStringLiteral(".copying")));
    EXPECT_FALSE(m_pCache->isCached(m_medium, source));
}

TEST_F(TrackCacheTest, AWholeCopyFromBeforeARestartIsKept) {
    const QByteArray content = pattern(1024 * 1024, 10);
    const QString source = makeSource(QStringLiteral("kept.aiff"), content);
    const QString local = m_pCache->startLocal(m_medium, source);
    ASSERT_TRUE(waitFor([&]() { return reportsFor(local, true) == 1; }));

    m_pCache = std::make_unique<TrackCache>();
    // Not copied again: it is here, whole, and counted.
    EXPECT_TRUE(m_pCache->isCached(m_medium, source));
    EXPECT_EQ(local, m_pCache->startLocal(m_medium, source));
    EXPECT_EQ(content.size(), m_pCache->bytesInRam());
}

} // namespace
