#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <QThread>

#include "library/deck/streamingfile.h"
#include "test/mixxxtest.h"

using mixxx::deck::PresentRanges;
using mixxx::deck::StreamingFile;

namespace {

// ---------------------------------------------------------------------------
// PresentRanges
//
// This is the piece that decides whether a decoder ever reads a hole. Every
// test below is about one direction of error being far worse than the other:
// saying a range is missing when it is present costs a needless wait, while
// saying it is present when it is missing plays silence and reports nothing.
// ---------------------------------------------------------------------------

TEST(PresentRangesTest, EmptyContainsNothing) {
    PresentRanges ranges;
    EXPECT_FALSE(ranges.contains(0, 1));
    EXPECT_EQ(0, ranges.availableFrom(0));
}

TEST(PresentRangesTest, AZeroLengthRangeIsAlwaysSatisfied) {
    // A decoder asking for nothing must not block on a file that has nothing.
    PresentRanges ranges;
    EXPECT_TRUE(ranges.contains(0, 0));
}

TEST(PresentRangesTest, ContainsIsExactAtBothEdges) {
    PresentRanges ranges;
    ranges.add(100, 50); // [100, 150)
    EXPECT_TRUE(ranges.contains(100, 50));
    EXPECT_TRUE(ranges.contains(100, 1));
    EXPECT_TRUE(ranges.contains(149, 1));
    // One byte past, at either end, is a hole.
    EXPECT_FALSE(ranges.contains(99, 1));
    EXPECT_FALSE(ranges.contains(150, 1));
    EXPECT_FALSE(ranges.contains(100, 51));
    EXPECT_FALSE(ranges.contains(99, 51));
}

TEST(PresentRangesTest, AdjacentRangesMerge) {
    // Two halves arriving back to back must become one range, or a read across
    // the seam would block forever on bytes that are all present.
    PresentRanges ranges;
    ranges.add(0, 100);
    ranges.add(100, 100);
    EXPECT_EQ(1, ranges.rangeCount());
    EXPECT_TRUE(ranges.contains(0, 200));
    EXPECT_TRUE(ranges.contains(50, 100));
}

TEST(PresentRangesTest, OverlappingRangesMerge) {
    PresentRanges ranges;
    ranges.add(0, 100);
    ranges.add(50, 100);
    EXPECT_EQ(1, ranges.rangeCount());
    EXPECT_TRUE(ranges.contains(0, 150));
    EXPECT_FALSE(ranges.contains(0, 151));
}

TEST(PresentRangesTest, ARangeArrivingTwiceDoesNotGrowTheList) {
    PresentRanges ranges;
    for (int i = 0; i < 10; ++i) {
        ranges.add(0, 100);
    }
    EXPECT_EQ(1, ranges.rangeCount());
}

TEST(PresentRangesTest, DisjointRangesStaySeparate) {
    // The head-then-tail case: a genuine hole in the middle must be reported.
    PresentRanges ranges;
    ranges.add(0, 100);
    ranges.add(1000, 100);
    EXPECT_EQ(2, ranges.rangeCount());
    EXPECT_TRUE(ranges.contains(0, 100));
    EXPECT_TRUE(ranges.contains(1000, 100));
    EXPECT_FALSE(ranges.contains(0, 1100));
    EXPECT_FALSE(ranges.contains(500, 1));
}

TEST(PresentRangesTest, ABridgingRangeJoinsBothNeighbours) {
    // Exactly what happens when the middle finally arrives between a head and a
    // tail that were fetched first.
    PresentRanges ranges;
    ranges.add(0, 100);
    ranges.add(1000, 100);
    ranges.add(100, 900);
    EXPECT_EQ(1, ranges.rangeCount());
    EXPECT_TRUE(ranges.contains(0, 1100));
}

TEST(PresentRangesTest, ARangeSwallowingSeveralCollapsesThemAll) {
    PresentRanges ranges;
    for (int i = 0; i < 10; ++i) {
        ranges.add(i * 200, 100); // ten islands
    }
    EXPECT_EQ(10, ranges.rangeCount());
    ranges.add(0, 2000);
    EXPECT_EQ(1, ranges.rangeCount());
    EXPECT_TRUE(ranges.contains(0, 2000));
}

TEST(PresentRangesTest, OutOfOrderArrivalStillMerges) {
    // Nothing guarantees the fetcher announces ranges in order.
    PresentRanges ranges;
    ranges.add(200, 100);
    ranges.add(0, 100);
    ranges.add(100, 100);
    EXPECT_EQ(1, ranges.rangeCount());
    EXPECT_TRUE(ranges.contains(0, 300));
}

TEST(PresentRangesTest, AvailableFromReportsTheContiguousRun) {
    PresentRanges ranges;
    ranges.add(0, 100);
    ranges.add(1000, 100);
    EXPECT_EQ(100, ranges.availableFrom(0));
    EXPECT_EQ(40, ranges.availableFrom(60));
    EXPECT_EQ(0, ranges.availableFrom(100)); // the hole starts here
    EXPECT_EQ(100, ranges.availableFrom(1000));
}

TEST(PresentRangesTest, ZeroLengthAddsAreIgnored) {
    PresentRanges ranges;
    ranges.add(100, 0);
    ranges.add(100, -5);
    EXPECT_EQ(0, ranges.rangeCount());
    EXPECT_FALSE(ranges.contains(100, 1));
}

// ---------------------------------------------------------------------------
// StreamingFile
// ---------------------------------------------------------------------------

class StreamingFileTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("track.mp3"));
        QFile file(m_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        // Full size from the outset, as the fetcher creates it.
        ASSERT_TRUE(file.resize(kSize));
        file.close();
    }

    /// Write real bytes at an offset, as a fetched range landing would.
    void land(StreamingFile* pStream, qint64 offset, qint64 length, char fill) {
        QFile file(m_path);
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        ASSERT_TRUE(file.seek(offset));
        ASSERT_EQ(length, file.write(QByteArray(static_cast<int>(length), fill)));
        file.close();
        pStream->markPresent(offset, length);
    }

    static constexpr qint64 kSize = 4096;
    QTemporaryDir m_dir;
    QString m_path;
};

TEST_F(StreamingFileTest, ReadsBytesThatHaveArrived) {
    StreamingFile stream(m_path, kSize);
    land(&stream, 0, 256, 'a');

    QByteArray buffer(256, '\0');
    EXPECT_EQ(256, stream.read(0, buffer.data(), 256));
    EXPECT_EQ(QByteArray(256, 'a'), buffer);
}

TEST_F(StreamingFileTest, ReadingPastTheEndIsEndOfFileNotAnError) {
    StreamingFile stream(m_path, kSize);
    stream.complete();
    QByteArray buffer(16, '\0');
    EXPECT_EQ(0, stream.read(kSize, buffer.data(), 16));
}

TEST_F(StreamingFileTest, AReadIsClampedToTheEndRatherThanOverrunning) {
    StreamingFile stream(m_path, kSize);
    stream.complete();
    QByteArray buffer(512, '\0');
    // Asking for more than is left must give what is left, not fail.
    EXPECT_EQ(96, stream.read(kSize - 96, buffer.data(), 512));
}

TEST_F(StreamingFileTest, AReadBlocksUntilTheBytesArrive) {
    // The whole point. A sparse file would answer this read instantly with
    // zeros; the deck would play silence and nothing would report a problem.
    StreamingFile stream(m_path, kSize);

    QElapsedTimer timer;
    timer.start();
    QThread* pFetcher = QThread::create([this, &stream]() {
        QThread::msleep(120);
        land(&stream, 0, 256, 'z');
    });
    pFetcher->start();

    QByteArray buffer(256, '\0');
    const qint64 read = stream.read(0, buffer.data(), 256);
    const qint64 elapsed = timer.elapsed();
    pFetcher->wait();
    delete pFetcher;

    EXPECT_EQ(256, read);
    EXPECT_EQ(QByteArray(256, 'z'), buffer);
    // It really waited rather than returning early with zeros.
    EXPECT_GE(elapsed, 100);
}

TEST_F(StreamingFileTest, AReadSpanningAHoleWaitsForTheHoleNotJustTheStart) {
    // Head and tail present, middle missing: a read across the gap must not be
    // satisfied by the fact that its first byte happens to be there.
    StreamingFile stream(m_path, kSize);
    land(&stream, 0, 256, 'h');
    land(&stream, 2048, 256, 't');

    QThread* pFetcher = QThread::create([this, &stream]() {
        QThread::msleep(80);
        land(&stream, 256, 2048 - 256, 'm');
    });
    pFetcher->start();

    QByteArray buffer(2304, '\0');
    EXPECT_EQ(2304, stream.read(0, buffer.data(), 2304));
    pFetcher->wait();
    delete pFetcher;
    EXPECT_EQ('h', buffer.at(0));
    EXPECT_EQ('m', buffer.at(300));
    EXPECT_EQ('t', buffer.at(2100));
}

TEST_F(StreamingFileTest, AFailedTransferWakesAReaderInsteadOfHangingIt) {
    StreamingFile stream(m_path, kSize);
    QThread* pFetcher = QThread::create([&stream]() {
        QThread::msleep(50);
        stream.fail(QStringLiteral("the player went away"));
    });
    pFetcher->start();

    QByteArray buffer(256, '\0');
    EXPECT_EQ(-1, stream.read(0, buffer.data(), 256));
    pFetcher->wait();
    delete pFetcher;
    EXPECT_EQ(QStringLiteral("the player went away"), stream.error());
}

TEST_F(StreamingFileTest, AbandoningWakesAReader) {
    // The track was unloaded while a read was waiting; the reader must not be
    // left holding a thread until the timeout.
    StreamingFile stream(m_path, kSize);
    QThread* pFetcher = QThread::create([&stream]() {
        QThread::msleep(50);
        stream.abandon();
    });
    pFetcher->start();

    QByteArray buffer(256, '\0');
    EXPECT_EQ(-1, stream.read(0, buffer.data(), 256));
    pFetcher->wait();
    delete pFetcher;
}

TEST_F(StreamingFileTest, CompleteSatisfiesEveryRange) {
    // complete() marks the whole file present, so a reader never waits on a
    // range the fetcher finished but forgot to announce.
    StreamingFile stream(m_path, kSize);
    stream.complete();
    QByteArray buffer(kSize, '\0');
    EXPECT_EQ(kSize, stream.read(0, buffer.data(), kSize));
    EXPECT_TRUE(stream.isComplete());
}

TEST_F(StreamingFileTest, SeveralReadersAreAllWokenByOneArrival) {
    StreamingFile stream(m_path, kSize);
    QList<QThread*> readers;
    QAtomicInt succeeded(0);
    for (int i = 0; i < 4; ++i) {
        QThread* pReader = QThread::create([&stream, &succeeded]() {
            QByteArray buffer(256, '\0');
            if (stream.read(0, buffer.data(), 256) == 256) {
                succeeded.fetchAndAddOrdered(1);
            }
        });
        pReader->start();
        readers.append(pReader);
    }
    QThread::msleep(60);
    land(&stream, 0, 256, 'q');
    for (QThread* pReader : readers) {
        pReader->wait();
        delete pReader;
    }
    EXPECT_EQ(4, succeeded.loadAcquire());
}

} // namespace
