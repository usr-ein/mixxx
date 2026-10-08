#include "library/deck/trackcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <vector>

#include "library/deck/ramstore.h"
#include "library/deck/streamingfile.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("TrackCache");

/// How much of the RAM store tier 1 may take.
///
/// Two thirds of it, leaving the rest for the mirror of a remote medium and for
/// the copies that keep a consuming player alive across an eject.
///
/// **A share rather than a number.** This was a flat gigabyte, chosen against
/// the deck's 3796 MB of RAM -- and it was wrong, because the tmpfs it lands on
/// is not sized from that. `/run` here is 760 MB, so the cap could never be
/// reached: the filesystem would fill first, and a full `/run` takes systemd
/// with it. RamStore measures what is actually there.
constexpr double kTier1Share = 2.0 / 3.0;

/// A copy goes this much at a time: small enough that a stop, or a reader
/// waiting further on, is heard within a fraction of a second even off a USB 2
/// stick (70 ms at 3.7 MB/s), large enough that a fast stick is not slowed by
/// the bookkeeping.
constexpr qint64 kChunkBytes = 256 * 1024;

/// Beside a copy while it is being written. A copy cut short -- Mixxx stopped
/// or crashed mid-track -- leaves a file of the right size with holes in it,
/// and holes read back as silence; the marker is what says not to trust it.
const QString kCopyingSuffix = QStringLiteral(".copying");

QAtomicInt s_chunkDelayMs;

mixxx::deck::TrackCache* s_pInstance = nullptr;
} // namespace

namespace mixxx {
namespace deck {

TrackCache* TrackCache::instance() {
    return s_pInstance;
}

QString TrackCache::diskTierRoot() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("trimixxx/tracks"));
}

void TrackCache::setChunkDelayForTest(int milliseconds) {
    s_chunkDelayMs.storeRelaxed(milliseconds);
}

TrackCache::TrackCache(QObject* pParent)
        : QObject(pParent) {
    s_pInstance = this;

    // One at a time: the bottleneck is the USB bus, and parallel copies just
    // make the stick seek. See the member's declaration for the other, more
    // important reason this pool is ours.
    m_copyPool.setMaxThreadCount(1);

    m_tier1Root = RamStore::path(QStringLiteral("cache"));
    m_tier1Cap = RamStore::budget(kTier1Share);
    m_tier2Root = diskTierRoot();
    // Wiped at startup: tier 2 holds only what could not be re-read at the time
    // it was written, and none of that is true any more.
    QDir(m_tier2Root).removeRecursively();
    QDir().mkpath(m_tier2Root);

    // Tier 1 outlives Mixxx (it is a tmpfs, emptied by a reboot), so a restart
    // finds the last run's copies. The whole ones are taken back -- counted
    // against the cap and reclaimable like any other -- rather than left on the
    // tmpfs unaccounted for. One still being written when that run ended is a
    // file of the right size with holes in it, and it goes.
    const QFileInfoList files = QDir(m_tier1Root).entryInfoList(QDir::Files, QDir::Name);
    int dropped = 0;
    for (const QFileInfo& file : files) {
        const QString path = file.absoluteFilePath();
        if (path.endsWith(kCopyingSuffix) || path.endsWith(QStringLiteral(".part"))) {
            continue;
        }
        if (QFileInfo::exists(path + kCopyingSuffix)) {
            QFile::remove(path);
            ++dropped;
            continue;
        }
        Entry entry;
        entry.localPath = path;
        entry.size = file.size();
        entry.lastUsed = ++m_clock;
        m_entries.insert(file.fileName(), entry);
        m_ramBytes += entry.size;
    }
    for (const QFileInfo& file : files) {
        const QString path = file.absoluteFilePath();
        if (path.endsWith(kCopyingSuffix) || path.endsWith(QStringLiteral(".part"))) {
            QFile::remove(path);
        }
    }

    kLogger.info() << "tier 1" << m_tier1Root << "capped at"
                   << (m_tier1Cap / (1024 * 1024)) << "MB, holding" << m_entries.size()
                   << "copies from before (" << (m_ramBytes / (1024 * 1024)) << "MB),"
                   << dropped << "unfinished ones dropped; tier 2" << m_tier2Root;
    evictIfNeeded();
}

TrackCache::~TrackCache() {
    // Drain before anything else. A copy captures `this` and touches it again
    // when it finishes -- to invoke back onto this object, and to count itself
    // in m_copiesInFlight. Destroying the cache out from under one is a
    // use-after-free on the heap, and it is not hypothetical: it corrupted the
    // allocator on every shutdown, and glibc aborted in malloc_consolidate()
    // partway through Mixxx writing its settings, which is why neither
    // mixxx.cfg nor effects.xml was ever saved.
    //
    // Stopped first, so that waiting out the one running takes a chunk, not a
    // track; clear() drops the ones that have not started.
    for (const Copy& copy : std::as_const(m_copies)) {
        copy.stop->storeRelaxed(1);
    }
    m_copyPool.clear();
    m_copyPool.waitForDone();
    // What they leave behind is unfinished, and marked so: the next start
    // drops it. Nothing may go on reading them as streams.
    for (const Copy& copy : std::as_const(m_copies)) {
        copy.stream->fail(QStringLiteral("Mixxx is shutting down"));
        StreamingFileRegistry::remove(copy.localPath);
    }

    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

QString TrackCache::cacheName(const MediumId& medium, const QString& sourcePath) {
    // Hashed, not mirrored. A rekordbox path can be long and can contain
    // anything a DJ typed, and two media routinely hold clones of the same
    // file -- so the medium has to be part of the key or one would shadow the
    // other.
    const QByteArray key = (medium.key() + QChar('\0') + sourcePath).toUtf8();
    const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
    const QString suffix = QFileInfo(sourcePath).suffix();
    return suffix.isEmpty() ? digest : digest + QChar('.') + suffix;
}

QString TrackCache::localPathFor(const MediumId& medium, const QString& sourcePath) const {
    if (sourcePath.isEmpty()) {
        return QString();
    }
    const QString name = cacheName(medium, sourcePath);
    const auto it = m_entries.constFind(name);
    if (it != m_entries.constEnd()) {
        return it->localPath;
    }
    return QDir(m_tier1Root).filePath(name);
}

bool TrackCache::isComplete(const QString& localPath) const {
    return !localPath.isEmpty() && !m_copies.contains(localPath) &&
            m_entries.contains(QFileInfo(localPath).fileName()) && QFileInfo::exists(localPath);
}

bool TrackCache::isCached(const MediumId& medium, const QString& sourcePath) const {
    return isComplete(localPathFor(medium, sourcePath));
}

qint64 TrackCache::copyFile(const QString& from, const QString& to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    // Via a partial file, so a copy interrupted by a yank or a crash is never
    // mistaken for a complete one -- which would be a track that plays for
    // however far the copy got and then stops.
    const QString partial = to + QStringLiteral(".part");
    QFile::remove(partial);
    if (!QFile::copy(from, partial)) {
        QFile::remove(partial);
        return -1;
    }
    if (!QFile::rename(partial, to)) {
        QFile::remove(partial);
        return -1;
    }
    return QFileInfo(to).size();
}

bool TrackCache::copyInto(const QString& from,
        const QString& to,
        qint64 size,
        StreamingFile* pStream,
        const QAtomicInt& stop,
        QString* pError) {
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        *pError = in.errorString();
        return false;
    }
    // Unbuffered, so a chunk is in the file -- where the decoder's own handle
    // reads it -- before it is announced.
    QFile out(to);
    if (!out.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
        *pError = out.errorString();
        return false;
    }

    // Chunk by chunk from the front, as a track plays -- unless a reader is
    // waiting somewhere not copied yet, in which case the copy jumps there and
    // carries on from it. That one rule covers the decoder opening an M4A at
    // its index (usually at the end) or reading a WAV's tags after its audio,
    // a seek, and a hot cue, each for the price of one chunk; copying a fixed
    // tail up front cost every track a quarter of a second off a slow stick,
    // and a play pressed in that time is dropped.
    const qint64 chunks = (size + kChunkBytes - 1) / kChunkBytes;
    std::vector<bool> copied(static_cast<size_t>(chunks), false);
    qint64 cursor = 0;
    qint64 left = chunks;
    QByteArray buffer(static_cast<int>(kChunkBytes), Qt::Uninitialized);
    while (left > 0) {
        if (stop.loadRelaxed()) {
            *pError = QStringLiteral("stopped");
            return false;
        }
        qint64 chunk = -1;
        const qint64 wanted = pStream->wantedOffset();
        if (wanted >= 0 && wanted < size && !copied[static_cast<size_t>(wanted / kChunkBytes)]) {
            chunk = wanted / kChunkBytes;
            cursor = chunk;
        }
        if (chunk < 0) {
            // Onwards from the cursor, round to the front for anything a jump
            // skipped.
            for (qint64 i = 0; i < chunks; ++i) {
                const qint64 c = (cursor + i) % chunks;
                if (!copied[static_cast<size_t>(c)]) {
                    chunk = c;
                    break;
                }
            }
        }
        const qint64 offset = chunk * kChunkBytes;
        const qint64 length = qMin(kChunkBytes, size - offset);
        if (!in.seek(offset) || in.read(buffer.data(), length) != length) {
            *pError = in.error() != QFileDevice::NoError
                    ? in.errorString()
                    : QStringLiteral("the file ended at %1 of %2 bytes").arg(offset).arg(size);
            return false;
        }
        if (!out.seek(offset) || out.write(buffer.constData(), length) != length) {
            *pError = out.errorString();
            return false;
        }
        pStream->markPresent(offset, length);
        copied[static_cast<size_t>(chunk)] = true;
        cursor = chunk + 1;
        --left;
        if (const int delay = s_chunkDelayMs.loadRelaxed(); delay > 0) {
            QThread::msleep(static_cast<unsigned long>(delay));
        }
    }
    return true;
}

QString TrackCache::beginCopy(const MediumId& medium, const QString& sourcePath) {
    const QFileInfo source(sourcePath);
    if (sourcePath.isEmpty() || !source.isFile()) {
        return QString();
    }
    const qint64 size = source.size();
    // Always into RAM, whatever an old entry says: one left behind by a file
    // that has since gone is forgotten, and its bytes with it.
    const QString name = cacheName(medium, sourcePath);
    const QString local = QDir(m_tier1Root).filePath(name);
    if (const auto entry = m_entries.constFind(name); entry != m_entries.constEnd()) {
        (entry->onDisk ? m_diskBytes : m_ramBytes) -= entry->size;
        m_entries.erase(entry);
    }
    // Copying off it again means it is back in a slot.
    m_unreachable.remove(medium.key());

    // A copy stopped a moment ago whose thread has not reported back yet: this
    // one replaces it, and its report, when it comes, is for a copy nobody
    // holds. It writes the same bytes to the same places until it notices.
    if (auto it = m_copies.find(local); it != m_copies.end()) {
        m_ramBytes -= it->size;
        m_copies.erase(it);
    }

    // The marker before the file, so there is never a moment when the file is
    // there without it.
    QDir().mkpath(QFileInfo(local).absolutePath());
    QFile marker(local + kCopyingSuffix);
    QFile file(local);
    if (!marker.open(QIODevice::WriteOnly) ||
            !file.open(QIODevice::WriteOnly | QIODevice::Truncate) || !file.resize(size)) {
        kLogger.warning() << "cannot make room for" << sourcePath << "at" << local << "--"
                          << file.errorString();
        file.close();
        QFile::remove(local);
        QFile::remove(local + kCopyingSuffix);
        return QString();
    }
    marker.close();
    file.close();
    auto pStream = std::make_shared<StreamingFile>(local, size);
    if (!pStream->error().isEmpty()) {
        kLogger.warning() << "cannot read back" << local << "--" << pStream->error();
        QFile::remove(local);
        QFile::remove(local + kCopyingSuffix);
        return QString();
    }
    StreamingFileRegistry::add(local, pStream);

    Copy copy;
    copy.medium = medium;
    copy.sourcePath = sourcePath;
    copy.localPath = local;
    copy.size = size;
    copy.stream = pStream;
    copy.stop = std::make_shared<QAtomicInt>(0);
    m_copies.insert(local, copy);
    // Counted from the start, at its full size: it takes that much of the
    // tmpfs whatever has landed, so room is made for it now.
    m_ramBytes += size;
    evictIfNeeded();

    const auto pStop = copy.stop;
    QElapsedTimer started;
    started.start();
    m_copyPool.start(
            [this, sourcePath, local, size, pStream, pStop, started]() {
                m_copiesInFlight.ref();
                QString error;
                const bool ok = copyInto(sourcePath, local, size, pStream.get(), *pStop, &error);
                m_copiesInFlight.deref();
                const qint64 ms = started.elapsed();
                QMetaObject::invokeMethod(
                        this,
                        [this, local, pStream, ok, error, ms, size]() {
                            if (ok) {
                                kLogger.info() << "copied" << local << size << "bytes in" << ms
                                               << "ms, waited on" << pStream->waitCount()
                                               << "times /" << pStream->waitedMs() << "ms";
                            }
                            onCopyDone(local, pStream, ok, error);
                        },
                        Qt::QueuedConnection);
            });
    return local;
}

void TrackCache::onCopyDone(const QString& localPath,
        const std::shared_ptr<StreamingFile>& pStream,
        bool ok,
        const QString& error) {
    const auto it = m_copies.find(localPath);
    if (it == m_copies.end() || it->stream != pStream) {
        // Replaced while it ran (see beginCopy): the file is the new copy's,
        // and so is the news.
        pStream->fail(QStringLiteral("replaced"));
        return;
    }
    const Copy copy = *it;
    m_copies.erase(it);
    QFile::remove(localPath + kCopyingSuffix);

    if (ok) {
        pStream->complete();
        // Whole now: whatever opens it next reads it as the ordinary file it
        // is. A decoder already reading it keeps its own reference to the
        // stream, which now never waits.
        StreamingFileRegistry::remove(localPath);
        Entry entry;
        entry.medium = copy.medium;
        entry.localPath = localPath;
        entry.size = copy.size;
        entry.reReadable = !m_unreachable.contains(copy.medium.key());
        entry.lastUsed = ++m_clock;
        m_entries.insert(QFileInfo(localPath).fileName(), entry);
        emit cached(localPath, true);
    } else {
        // Wakes a reader, which then fails rather than hang: the bytes it is
        // waiting for are not coming.
        pStream->fail(error);
        StreamingFileRegistry::remove(localPath);
        QFile::remove(localPath);
        m_ramBytes -= copy.size;
        if (!copy.stop->loadRelaxed()) {
            kLogger.warning() << "could not copy" << copy.sourcePath << "--" << error;
        }
        emit cached(localPath, false);
    }
}

void TrackCache::stopCopies(const QString& keep) {
    for (auto it = m_copies.begin(); it != m_copies.end(); ++it) {
        if (it.key() != keep) {
            it->stop->storeRelaxed(1);
        }
    }
}

QString TrackCache::startLocal(const MediumId& medium, const QString& sourcePath) {
    const QString local = localPathFor(medium, sourcePath);
    if (local.isEmpty()) {
        return QString();
    }
    if (isComplete(local)) {
        touch(local);
        return local;
    }
    if (auto it = m_copies.find(local); it != m_copies.end() && !it->stop->loadRelaxed()) {
        // The same track loaded again while it is still arriving.
        stopCopies(local);
        kLogger.debug() << "loading the copy already under way:" << sourcePath;
        return local;
    }
    // The deck holds one track: a copy for any other is not wanted any more.
    stopCopies();
    const QString started = beginCopy(medium, sourcePath);
    if (started.isEmpty()) {
        kLogger.warning() << "could not start copying" << sourcePath;
    }
    return started;
}

void TrackCache::release(const QString& localPath) {
    const auto it = m_copies.find(localPath);
    if (it == m_copies.end() || it->stop->loadRelaxed()) {
        return;
    }
    // Nobody is going to play the rest, and copying it would only keep the
    // stick from the track that replaces it.
    it->stop->storeRelaxed(1);
    kLogger.debug() << "the deck let go of" << localPath << "before its copy finished";
}

void TrackCache::adopt(const MediumId& medium, const QString& localPath, qint64 size) {
    if (localPath.isEmpty()) {
        return;
    }
    const QString name = QFileInfo(localPath).fileName();
    auto it = m_entries.find(name);
    if (it != m_entries.end()) {
        // Already known, streamed a second time. Correct the size rather than
        // double-count it: the file was created at its full length up front, so
        // this is the same bytes, not more of them.
        m_ramBytes += size - it->size;
        it->size = size;
        it->lastUsed = ++m_clock;
        return;
    }
    Entry entry;
    entry.medium = medium;
    entry.localPath = localPath;
    entry.size = size;
    entry.lastUsed = ++m_clock;
    m_entries.insert(name, entry);
    m_ramBytes += size;
    kLogger.debug() << "adopted" << name << size << "bytes; tier 1 now" << m_ramBytes;
}

void TrackCache::touch(const QString& localPath) {
    const QString name = QFileInfo(localPath).fileName();
    auto it = m_entries.find(name);
    if (it != m_entries.end()) {
        it->lastUsed = ++m_clock;
    }
}

void TrackCache::pin(const QString& localPath) {
    if (!localPath.isEmpty()) {
        m_pinned.insert(localPath);
        touch(localPath);
    }
}

void TrackCache::unpin(const QString& localPath) {
    m_pinned.remove(localPath);
}

void TrackCache::markUnreachable(const MediumId& medium) {
    int count = 0;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->medium == medium) {
            it->reReadable = false;
            ++count;
        }
    }
    // And for copies still running: if one finishes anyway, off what the
    // kernel still holds of the stick, it is the only copy there is.
    for (const Copy& copy : std::as_const(m_copies)) {
        if (copy.medium == medium) {
            m_unreachable.insert(medium.key());
        }
    }
    if (count > 0) {
        kLogger.info() << count << "cached files can no longer be re-read from"
                       << medium.key();
    }
}

void TrackCache::markReachable(const MediumId& medium) {
    m_unreachable.remove(medium.key());
    int count = 0;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->medium == medium && !it->reReadable) {
            it->reReadable = true;
            ++count;
        }
    }
    if (count > 0) {
        kLogger.info() << count << "cached files can be re-read from" << medium.key()
                       << "again";
    }
}

bool TrackCache::hasPinnedFrom(const MediumId& medium) const {
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (it->medium == medium && m_pinned.contains(it->localPath) &&
                !m_copies.contains(it->localPath)) {
            return true;
        }
    }
    return false;
}

void TrackCache::evictIfNeeded() {
    if (m_ramBytes <= m_tier1Cap) {
        return;
    }
    // Oldest first, and pinned entries are never candidates however old. A
    // copy still running is not an entry yet, so it is never one either.
    QList<QString> order;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!m_pinned.contains(it->localPath) && !it->onDisk) {
            order.append(it.key());
        }
    }
    std::sort(order.begin(), order.end(), [this](const QString& a, const QString& b) {
        return m_entries.value(a).lastUsed < m_entries.value(b).lastUsed;
    });

    for (const QString& name : order) {
        if (m_ramBytes <= m_tier1Cap) {
            break;
        }
        Entry entry = m_entries.value(name);
        if (entry.reReadable) {
            // The source is still there, so the cheapest thing that can happen
            // is nothing: drop it and copy it again if it is wanted. Zero card
            // writes.
            QFile::remove(entry.localPath);
            m_ramBytes -= entry.size;
            m_entries.remove(name);
            continue;
        }
        // The only copy there is. Spill it rather than lose it -- this is the
        // one path that touches the card, and it exists for exactly one
        // situation: holding bytes whose source has just been unplugged.
        const QString target = QDir(m_tier2Root).filePath(name);
        if (copyFile(entry.localPath, target) < 0) {
            kLogger.warning() << "could not spill" << entry.localPath << "to disk";
            continue;
        }
        QFile::remove(entry.localPath);
        m_ramBytes -= entry.size;
        m_diskBytes += entry.size;
        m_diskBytesWritten += entry.size;
        entry.localPath = target;
        entry.onDisk = true;
        m_entries.insert(name, entry);
        kLogger.info() << "spilled to disk:" << name << entry.size << "bytes";
    }
}

} // namespace deck
} // namespace mixxx

#include "moc_trackcache.cpp"
