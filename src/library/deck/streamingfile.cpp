#include "library/deck/streamingfile.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QThread>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StreamingFile");
} // namespace

namespace mixxx {
namespace deck {

void PresentRanges::add(qint64 offset, qint64 length) {
    if (length <= 0) {
        return;
    }
    qint64 start = offset;
    qint64 end = offset + length;

    // Absorb every range this one touches or bridges, then insert the union.
    // Merging on the way in is what keeps contains() from having to consider
    // that a range might be split across neighbours.
    int insertAt = 0;
    while (insertAt < m_ranges.size()) {
        const qint64 otherStart = m_ranges.at(insertAt).first;
        const qint64 otherEnd = m_ranges.at(insertAt).second;
        if (otherEnd < start) {
            ++insertAt;
            continue;
        }
        if (otherStart > end) {
            break;
        }
        // Overlapping or adjacent: swallow it. Adjacent counts, or two halves
        // of a file arriving back to back would stay two ranges forever and a
        // read spanning the seam would block on bytes that are present.
        start = qMin(start, otherStart);
        end = qMax(end, otherEnd);
        m_ranges.removeAt(insertAt);
    }
    m_ranges.insert(insertAt, {start, end});
}

bool PresentRanges::contains(qint64 offset, qint64 length) const {
    if (length <= 0) {
        return true;
    }
    const qint64 end = offset + length;
    for (const auto& range : m_ranges) {
        if (range.first <= offset && range.second >= end) {
            return true;
        }
        if (range.first > offset) {
            break; // Sorted, so nothing later can start early enough.
        }
    }
    return false;
}

qint64 PresentRanges::availableFrom(qint64 offset) const {
    for (const auto& range : m_ranges) {
        if (range.first <= offset && range.second > offset) {
            return range.second - offset;
        }
        if (range.first > offset) {
            break;
        }
    }
    return 0;
}

void PresentRanges::clear() {
    m_ranges.clear();
}

StreamingFile::StreamingFile(const QString& localPath, qint64 size)
        : m_file(localPath), m_size(size) {
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open %1").arg(localPath);
    }
}

StreamingFile::~StreamingFile() {
    abandon();
}

void StreamingFile::markPresent(qint64 offset, qint64 length) {
    QMutexLocker locked(&m_mutex);
    m_present.add(offset, length);
    m_arrived.wakeAll();
}

void StreamingFile::complete() {
    QMutexLocker locked(&m_mutex);
    m_complete = true;
    // Everything, so a reader never waits on a range the fetcher forgot to
    // announce.
    m_present.add(0, m_size);
    m_arrived.wakeAll();
}

void StreamingFile::fail(const QString& reason) {
    QMutexLocker locked(&m_mutex);
    m_error = reason;
    m_arrived.wakeAll();
}

void StreamingFile::abandon() {
    QMutexLocker locked(&m_mutex);
    m_abandoned = true;
    m_arrived.wakeAll();
}

bool StreamingFile::isComplete() const {
    QMutexLocker locked(&m_mutex);
    return m_complete;
}

QString StreamingFile::error() const {
    QMutexLocker locked(&m_mutex);
    return m_error;
}

qint64 StreamingFile::read(qint64 offset, char* pBuffer, qint64 length) {
    if (offset >= m_size) {
        return 0; // Genuine end of file.
    }
    // Never read past the end, whatever was asked for.
    const qint64 wanted = qMin(length, m_size - offset);
    if (wanted <= 0) {
        return 0;
    }

    QMutexLocker locked(&m_mutex);
    QDeadlineTimer deadline(kReadTimeoutMs);
    QElapsedTimer waited;
    bool didWait = false;
    while (!m_present.contains(offset, wanted)) {
        // The one rule this class enforces rather than documents; see the
        // header. Refused at the moment of waiting, not the moment of asking,
        // so a GUI-thread read of bytes that are already here still works.
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            kLogger.warning()
                    << "refusing to wait for" << wanted << "bytes at" << offset
                    << "on the GUI thread, which is the thread that announces "
                       "their arrival -- this read would never have completed";
            return -1;
        }
        if (!didWait) {
            didWait = true;
            waited.start();
            ++m_waitCount;
            // Worth a line each time: a read that waits is the download losing
            // the race with the playhead, and a burst of these is the warning
            // that a track is about to stutter.
            kLogger.debug() << "waiting for" << wanted << "bytes at" << offset
                            << "(wait" << m_waitCount << ")";
        }
        if (m_abandoned) {
            return -1;
        }
        if (!m_error.isEmpty()) {
            kLogger.warning() << "read failed while waiting:" << m_error;
            return -1;
        }
        // Waiting is the entire feature. The alternative -- returning what has
        // arrived so far -- reads to a decoder as end of stream, and it would
        // stop the track wherever the download happened to be.
        if (!m_arrived.wait(&m_mutex, deadline)) {
            kLogger.warning() << "timed out waiting for" << wanted << "bytes at"
                              << offset;
            return -1;
        }
    }

    if (didWait) {
        const qint64 elapsed = waited.elapsed();
        m_waitedMs += elapsed;
        kLogger.info() << "waited" << elapsed << "ms for" << wanted << "bytes at"
                       << offset << "-- total waits" << m_waitCount << "/"
                       << m_waitedMs << "ms";
    }

    if (!m_file.isOpen()) {
        return -1;
    }
    // The file itself is only touched with the lock held, so a concurrent
    // markPresent cannot move the read position underneath this.
    if (!m_file.seek(offset)) {
        return -1;
    }
    return m_file.read(pBuffer, wanted);
}

int StreamingFile::waitCount() const {
    QMutexLocker locked(&m_mutex);
    return m_waitCount;
}

qint64 StreamingFile::waitedMs() const {
    QMutexLocker locked(&m_mutex);
    return m_waitedMs;
}

namespace {
QMutex s_registryMutex;
QHash<QString, std::shared_ptr<StreamingFile>> s_registry;
} // namespace

void StreamingFileRegistry::add(const QString& localPath, std::shared_ptr<StreamingFile> pFile) {
    QMutexLocker locked(&s_registryMutex);
    s_registry.insert(localPath, std::move(pFile));
    kLogger.info() << "streaming" << localPath << "--" << s_registry.size() << "in flight";
}

void StreamingFileRegistry::remove(const QString& localPath) {
    QMutexLocker locked(&s_registryMutex);
    if (s_registry.remove(localPath) > 0) {
        kLogger.info() << "no longer streaming" << localPath;
    }
}

std::shared_ptr<StreamingFile> StreamingFileRegistry::lookup(const QString& localPath) {
    QMutexLocker locked(&s_registryMutex);
    return s_registry.value(localPath);
}

int StreamingFileRegistry::count() {
    QMutexLocker locked(&s_registryMutex);
    return static_cast<int>(s_registry.size());
}

QList<QPair<QString, std::shared_ptr<StreamingFile>>> StreamingFileRegistry::snapshot() {
    QMutexLocker locked(&s_registryMutex);
    QList<QPair<QString, std::shared_ptr<StreamingFile>>> out;
    out.reserve(s_registry.size());
    for (auto it = s_registry.constBegin(); it != s_registry.constEnd(); ++it) {
        out.append({it.key(), it.value()});
    }
    return out;
}

} // namespace deck
} // namespace mixxx
