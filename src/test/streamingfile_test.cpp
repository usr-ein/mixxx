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

TEST_F(StreamingFileTest, AnUnannouncedRangeIsAHoleEvenThoughTheFileIsFullSize) {
    // The reason this class exists, stated as a test. The file is on disk at
    // its full length from the first instant, so an ordinary read of any part
    // of it succeeds immediately and returns zeros. Nothing above would know:
    // there is no error, no short read, no signal of any kind -- the deck just
    // plays silence. So "the file exists" must never be taken to mean "the
    // bytes are there".
    StreamingFile stream(m_path, kSize);
    QFile plain(m_path);
    ASSERT_TRUE(plain.open(QIODevice::ReadOnly));
    EXPECT_EQ(kSize, plain.size());
    QByteArray naive = plain.read(256);
    plain.close();
    EXPECT_EQ(256, naive.size());
    EXPECT_EQ(QByteArray(256, '\0'), naive) << "a sparse hole reads back as zeros";

    // The same range through the StreamingFile blocks instead, and here fails
    // rather than hanging only because the transfer is told it has failed.
    stream.fail(QStringLiteral("nothing arrived"));
    QByteArray buffer(256, '\xff');
    EXPECT_EQ(-1, stream.read(0, buffer.data(), 256));
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

// ---------------------------------------------------------------------------
// The whole thing, without a network
//
// A track arrives head, then tail, then the middle in playhead order, and a
// decoder reads it front to back while that happens. Everything below the
// network is real here: a sparse file, a background writer, and a reader that
// blocks. Only the socket is missing.
// ---------------------------------------------------------------------------

class StreamingArrivalTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("track.mp3"));
        QFile file(m_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        // Sparse and full-length from the outset, exactly as the fetcher makes
        // it. Every byte of it currently reads back as zero.
        ASSERT_TRUE(file.resize(kSize));
        file.close();
    }

    /// The byte a given offset should hold once it has really arrived.
    ///
    /// Position-dependent and never zero, so a hole cannot be mistaken for
    /// content and content cannot be mistaken for the wrong content.
    static char expectedAt(qint64 offset) {
        return static_cast<char>((offset % 251) + 1);
    }

    /// Write and announce one range, as the fetcher does: flushed to disk
    /// first, announced second. The other order would let a reader through to
    /// bytes still sitting in a buffer.
    static void deliver(const QString& path, StreamingFile* pStream, qint64 offset, qint64 length) {
        QByteArray bytes(static_cast<int>(length), '\0');
        for (qint64 i = 0; i < length; ++i) {
            bytes[static_cast<int>(i)] = expectedAt(offset + i);
        }
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        ASSERT_TRUE(file.seek(offset));
        ASSERT_EQ(length, file.write(bytes));
        file.close();
        pStream->markPresent(offset, length);
    }

    /// The order prolink::consume::nfs::progressive_plan produces: head, tail,
    /// then the middle in chunks. Kept in step with it by the Rust tests, which
    /// assert the same shape against the real thing.
    static QList<QPair<qint64, qint64>> plan(qint64 size, qint64 head, qint64 tail, qint64 chunk) {
        QList<QPair<qint64, qint64>> steps;
        const qint64 headLen = qMin(head, size);
        if (headLen > 0) {
            steps.append({0, headLen});
        }
        const qint64 tailStart = qMax(headLen, size - tail);
        if (tailStart < size) {
            steps.append({tailStart, size - tailStart});
        }
        for (qint64 at = headLen; at < tailStart; at += chunk) {
            steps.append({at, qMin(chunk, tailStart - at)});
        }
        return steps;
    }

    static constexpr qint64 kSize = 512 * 1024;
    static constexpr qint64 kHead = 64 * 1024;
    static constexpr qint64 kTail = 16 * 1024;
    static constexpr qint64 kChunk = 32 * 1024;
    QTemporaryDir m_dir;
    QString m_path;
};

TEST_F(StreamingArrivalTest, ADecoderReadingFrontToBackNeverSeesAHole) {
    StreamingFile stream(m_path, kSize);

    const QList<QPair<qint64, qint64>> steps = plan(kSize, kHead, kTail, kChunk);
    const QString path = m_path;
    QThread* pFetcher = QThread::create([path, &stream, steps]() {
        for (const auto& step : steps) {
            // Slowly enough that the reader genuinely overtakes it -- otherwise
            // the test would pass without ever exercising a wait.
            QThread::msleep(5);
            deliver(path, &stream, step.first, step.second);
        }
        stream.complete();
    });
    pFetcher->start();

    // Read it the way FFmpeg does: forward, in buffer-sized bites.
    constexpr qint64 kBite = 8 * 1024;
    QByteArray buffer(kBite, '\0');
    qint64 at = 0;
    while (at < kSize) {
        const qint64 read = stream.read(at, buffer.data(), kBite);
        ASSERT_GT(read, 0) << "read failed at " << at;
        for (qint64 i = 0; i < read; ++i) {
            // The assertion that matters. A zero here is a byte the decoder
            // would have turned into silence.
            ASSERT_EQ(expectedAt(at + i), buffer.at(static_cast<int>(i)))
                    << "hole at " << (at + i);
        }
        at += read;
    }
    pFetcher->wait();
    delete pFetcher;

    EXPECT_EQ(kSize, at);
    // It really did have to wait, so the read-ahead path above was exercised.
    EXPECT_GT(stream.waitCount(), 0);
}

TEST_F(StreamingArrivalTest, TheTailIsReadableLongBeforeTheMiddle) {
    // An M4A keeps its moov atom at the end, and a decoder seeks there during
    // the open. That is why the tail is fetched second: if it waited its turn
    // in the middle, AAC would not open until the whole track had arrived --
    // and only AAC, which is a horrible fault to track down.
    StreamingFile stream(m_path, kSize);
    const QList<QPair<qint64, qint64>> steps = plan(kSize, kHead, kTail, kChunk);
    ASSERT_GE(steps.size(), 3);

    deliver(m_path, &stream, steps.at(0).first, steps.at(0).second);
    deliver(m_path, &stream, steps.at(1).first, steps.at(1).second);

    QByteArray buffer(1024, '\0');
    const qint64 read = stream.read(kSize - 1024, buffer.data(), 1024);
    EXPECT_EQ(1024, read);
    EXPECT_EQ(expectedAt(kSize - 1024), buffer.at(0));
    // And nothing waited for it: the middle has not been delivered at all.
    EXPECT_EQ(0, stream.waitCount());
}

TEST_F(StreamingArrivalTest, RangesArrivingInOneBatchAreAllRecorded) {
    // The network layer drains its events in batches, so several ranges land
    // between one look at the queue and the next. Recording only the last would
    // leave earlier ones looking like holes for good -- the reader would block
    // on bytes that are sitting on disk, and time out rather than play.
    StreamingFile stream(m_path, kSize);
    for (const auto& step : plan(kSize, kHead, kTail, kChunk)) {
        deliver(m_path, &stream, step.first, step.second);
    }
    QByteArray buffer(kSize, '\0');
    EXPECT_EQ(kSize, stream.read(0, buffer.data(), kSize));
    EXPECT_EQ(0, stream.waitCount());
    EXPECT_EQ(expectedAt(kSize - 1), buffer.at(static_cast<int>(kSize) - 1));
}

} // namespace
