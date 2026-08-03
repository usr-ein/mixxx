#pragma once

#include <QFile>
#include <QHash>
#include <memory>
#include <QList>
#include <QMutex>
#include <QString>
#include <QWaitCondition>

namespace mixxx {
namespace deck {

/// The set of byte ranges of a file that have actually arrived.
///
/// Kept sorted and merged, so "is this range present" is a single search rather
/// than a walk, and so a range that arrives twice does not grow the list.
///
/// Its own type because **this is the piece that decides whether a decoder ever
/// reads a hole**. If `contains()` is wrong by one byte in the optimistic
/// direction, the deck plays silence and nothing anywhere reports an error.
class PresentRanges {
  public:
    void add(qint64 offset, qint64 length);
    /// Whether every byte of `[offset, offset + length)` has arrived.
    bool contains(qint64 offset, qint64 length) const;
    /// How many contiguous bytes are available from `offset`. Zero when the
    /// byte at `offset` itself is missing.
    qint64 availableFrom(qint64 offset) const;
    void clear();
    int rangeCount() const {
        return static_cast<int>(m_ranges.size());
    }

  private:
    /// Sorted by offset, never overlapping, never adjacent.
    QList<QPair<qint64, qint64>> m_ranges;
};

/// A local file that is still arriving, which a decoder may read as if it were
/// not.
///
/// **A read of a range that has not arrived blocks until it does.** That is the
/// whole point, and it is the difference between this and simply letting a
/// decoder loose on a sparse file: an unwritten region of a sparse file reads
/// back as zeros, immediately and successfully, so the deck would play silence
/// and no layer would know anything was wrong. Silence mid-set is worse than a
/// pause, and much worse than an error.
///
/// The fetcher calls markPresent() as ranges land, complete() when the file is
/// whole, and fail() if it cannot be. Any of those wakes a blocked reader.
///
/// # One rule, and it is enforced here rather than remembered
///
/// **The GUI thread must never wait for bytes**, because the GUI thread is the
/// one that announces their arrival: `ProLinkNetworkService::poll()` runs on a
/// timer there, drains the transfer's progress events and calls markPresent().
/// A read that blocks on it therefore waits for a message only it can deliver.
/// That is not a slow load, it is a deadlock, and it presents as the deck
/// freezing for fifteen seconds and then reporting a read timeout -- which
/// reads like a network problem and is not one.
///
/// It has been written by accident once already. So read() refuses outright
/// when it is about to wait on the GUI thread, and says why. A caller that
/// needs a byte on that thread has to arrange for it some other way; a caller
/// that only *might* wait keeps working, because the refusal happens at the
/// moment of waiting and not at the moment of asking.
class StreamingFile {
  public:
    StreamingFile(const QString& localPath, qint64 size);
    ~StreamingFile();

    /// Read into *pBuffer*, blocking until the bytes are there.
    ///
    /// Returns the number of bytes read, 0 at end of file, or -1 if the
    /// transfer failed, was abandoned while waiting, or would have had to wait
    /// on the GUI thread. Never returns fewer bytes than asked for unless the
    /// file ends -- a short read looks like end of stream to a decoder, which
    /// would truncate the track.
    qint64 read(qint64 offset, char* pBuffer, qint64 length);

    /// Bytes `[offset, offset + length)` are now on disk.
    void markPresent(qint64 offset, qint64 length);
    /// The whole file is there.
    void complete();
    /// It is not coming. Wakes readers, which then fail rather than hang.
    void fail(const QString& reason);
    /// Give up: the track was unloaded while a read was waiting.
    void abandon();

    qint64 size() const {
        return m_size;
    }
    bool isComplete() const;
    QString error() const;

    /// How many reads had to wait, and how long in total. The number that says
    /// whether streaming is keeping ahead of the playhead: a healthy track
    /// waits a handful of times at the start and then never again.
    int waitCount() const;
    qint64 waitedMs() const;

    /// How long a read waits before giving up. A deck that hangs forever on a
    /// vanished player is worse than one that reports a failure and moves on.
    static constexpr int kReadTimeoutMs = 15000;

  private:
    mutable QMutex m_mutex;
    QWaitCondition m_arrived;
    PresentRanges m_present;
    QFile m_file;
    qint64 m_size;
    bool m_complete = false;
    bool m_abandoned = false;
    QString m_error;
    int m_waitCount = 0;
    qint64 m_waitedMs = 0;
};

/// Which local paths are being streamed right now.
///
/// The SoundSource provider consults this to decide whether a file is one of
/// ours: if it is not here, the provider declines and SoundSourceProxy falls
/// through to the ordinary decoders. That is what keeps local sticks on the
/// normal path and stops this machinery touching files it has no business
/// touching.
class StreamingFileRegistry {
  public:
    static void add(const QString& localPath, std::shared_ptr<StreamingFile> pFile);
    static void remove(const QString& localPath);
    /// Null when the path is an ordinary file.
    static std::shared_ptr<StreamingFile> lookup(const QString& localPath);
    static int count();
    /// Everything in flight, for the diagnostics page. A snapshot, so the
    /// caller is not holding the registry's lock while it draws.
    static QList<QPair<QString, std::shared_ptr<StreamingFile>>> snapshot();
};

} // namespace deck
} // namespace mixxx
