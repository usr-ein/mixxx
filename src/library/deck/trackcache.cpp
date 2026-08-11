#include "library/deck/trackcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtConcurrentRun>

#include "library/deck/ramstore.h"
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

mixxx::deck::TrackCache* s_pInstance = nullptr;
} // namespace

namespace mixxx {
namespace deck {

TrackCache* TrackCache::instance() {
    return s_pInstance;
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
    m_tier2Root = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                          .filePath(QStringLiteral("trimixxx/tracks"));
    // Wiped at startup: tier 2 holds only what could not be re-read at the time
    // it was written, and none of that is true any more.
    QDir(m_tier2Root).removeRecursively();
    QDir().mkpath(m_tier2Root);

    kLogger.info() << "tier 1" << m_tier1Root << "capped at"
                   << (m_tier1Cap / (1024 * 1024)) << "MB; tier 2" << m_tier2Root;
}

TrackCache::~TrackCache() {
    // Drain before anything else. A queued copy captures `this` and touches it
    // again when it finishes -- both to invoke back onto this object and, in
    // the continuation, to update m_entries and m_ramBytes. Destroying the
    // cache out from under one is a use-after-free on the heap, and it is not
    // hypothetical: it corrupted the allocator on every shutdown, and glibc
    // aborted in malloc_consolidate() partway through Mixxx writing its
    // settings, which is why neither mixxx.cfg nor effects.xml was ever saved.
    //
    // clear() drops the ones that have not started; waitForDone() waits out the
    // one that has. A track copy is a second or two, so this is not a stall.
    m_copyPool.clear();
    m_copyPool.waitForDone();

    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

QString TrackCache::localPathFor(const MediumId& medium, const QString& sourcePath) const {
    if (sourcePath.isEmpty()) {
        return QString();
    }
    // Hashed, not mirrored. A rekordbox path can be long and can contain
    // anything a DJ typed, and two media routinely hold clones of the same
    // file -- so the medium has to be part of the key or one would shadow the
    // other.
    const QByteArray key = (medium.key() + QChar('\0') + sourcePath).toUtf8();
    const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
    const QString suffix = QFileInfo(sourcePath).suffix();
    const QString name = suffix.isEmpty() ? digest : digest + QChar('.') + suffix;

    const auto it = m_entries.constFind(name);
    if (it != m_entries.constEnd()) {
        return it->localPath;
    }
    return QDir(m_tier1Root).filePath(name);
}

bool TrackCache::isCached(const MediumId& medium, const QString& sourcePath) const {
    const QString local = localPathFor(medium, sourcePath);
    return !local.isEmpty() && QFileInfo::exists(local);
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

QString TrackCache::ensureLocal(const MediumId& medium, const QString& sourcePath) {
    const QString local = localPathFor(medium, sourcePath);
    if (local.isEmpty()) {
        return QString();
    }
    if (QFileInfo::exists(local)) {
        touch(local);
        return local;
    }
    if (!QFileInfo::exists(sourcePath)) {
        return QString();
    }

    const qint64 size = copyFile(sourcePath, local);
    if (size < 0) {
        kLogger.warning() << "could not cache" << sourcePath;
        return QString();
    }

    Entry entry;
    entry.medium = medium;
    entry.localPath = local;
    entry.size = size;
    entry.lastUsed = ++m_clock;
    m_entries.insert(QFileInfo(local).fileName(), entry);
    m_ramBytes += size;
    evictIfNeeded();
    return local;
}

void TrackCache::prefetch(const MediumId& medium, const QString& sourcePath) {
    if (sourcePath.isEmpty() || isCached(medium, sourcePath)) {
        return;
    }
    // Fire and forget. The result is only interesting to whoever asked, and by
    // the time it lands the selection has usually moved on anyway -- the point
    // is that the bytes are here, not that anyone is told.
    const QString local = localPathFor(medium, sourcePath);
    QtConcurrent::run(&m_copyPool, [this, medium, sourcePath, local]() {
        const qint64 size = copyFile(sourcePath, local);
        QMetaObject::invokeMethod(
                this,
                [this, medium, local, size]() {
                    if (size < 0) {
                        emit cached(local, false);
                        return;
                    }
                    Entry entry;
                    entry.medium = medium;
                    entry.localPath = local;
                    entry.size = size;
                    entry.lastUsed = ++m_clock;
                    m_entries.insert(QFileInfo(local).fileName(), entry);
                    m_ramBytes += size;
                    evictIfNeeded();
                    emit cached(local, true);
                },
                Qt::QueuedConnection);
    });
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
    if (count > 0) {
        kLogger.info() << count << "cached files can no longer be re-read from"
                       << medium.key();
    }
}

bool TrackCache::hasPinnedFrom(const MediumId& medium) const {
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (it->medium == medium && m_pinned.contains(it->localPath)) {
            return true;
        }
    }
    return false;
}

void TrackCache::evictIfNeeded() {
    if (m_ramBytes <= m_tier1Cap) {
        return;
    }
    // Oldest first, and pinned entries are never candidates however old.
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
