#include "library/deck/mediaregistry.h"

#include <QDir>
#include <algorithm>
#include <utility>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtConcurrentRun>

#include <QStandardPaths>

#include "library/deck/deckqueries.h"
#include "library/deck/ramstore.h"
#include "library/deck/volumelabel.h"
#include "network/prolink/prolinkpdb.h"
#include "util/db/dbconnectionpooler.h"
#ifdef __PROLINK__
#include <QElapsedTimer>
#include <QEventLoop>

#include "library/deck/streamingfile.h"
#include "library/deck/trackcache.h"
#include "network/prolink/prolinknetworkservice.h"
#endif
#include "util/db/dbconnectionpooled.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("MediaRegistry");

const QString kMediaRoot = QStringLiteral("/media");
const QString kPdbPath = QStringLiteral("PIONEER/rekordbox/export.pdb");
/// The mount is still settling when the watcher fires; give it a moment.
constexpr int kRescanDebounceMs = 400;

#ifdef __PROLINK__
/// How much of a track to pull before anything else.
///
/// Runway, measured in seconds of playback rather than in bytes: 1 MB is about
/// twenty-five seconds of a 320 kbps MP3, against a link that delivers roughly
/// thirty seconds of audio per second. So the download is never the thing that
/// runs out — the head is there for the case where the network stalls, and
/// twenty-five seconds is long enough to notice and do something about it.
///
/// Deliberately not larger. The **tail** is fetched second and an M4A cannot be
/// opened without it, so every byte of head is delay before the first sound of
/// an AAC track. Doubling this would buy runway that is never used and pay for
/// it on every single load.
constexpr quint32 kStreamHeadBytes = 1024 * 1024;

/// How long to wait for a player to say how big a file is.
///
/// This covers a connect, a mount, a lookup and a stat — and, if another
/// transfer is already running, the wait for it, because the library serialises
/// them onto one player. Generous for that reason, and a wait anywhere near it
/// is a fault worth seeing in the log rather than a slow network.
constexpr int kStreamStartTimeoutMs = 20000;

/// How long to wait for a beat grid. Tens of kilobytes, so this is only ever
/// hit when something is wrong.
constexpr int kCompanionTimeoutMs = 10000;
#endif
} // namespace

namespace mixxx {
namespace deck {

namespace {
MediaRegistry* s_pInstance = nullptr;
/// Subscribers that asked before there was anything to subscribe to.
QList<QPair<QPointer<QObject>, std::function<void(MediaRegistry*)>>> s_pending;
} // namespace

MediaRegistry* MediaRegistry::instance() {
    return s_pInstance;
}

void MediaRegistry::whenReady(QObject* pContext, std::function<void(MediaRegistry*)> callback) {
    if (s_pInstance != nullptr) {
        callback(s_pInstance);
        return;
    }
    s_pending.append({QPointer<QObject>(pContext), std::move(callback)});
}

MediaRegistry::MediaRegistry(mixxx::DbConnectionPoolPtr dbConnectionPool, QObject* pParent)
        : QObject(pParent),
          m_dbConnectionPool(std::move(dbConnectionPool)) {
    qRegisterMetaType<mixxx::deck::MediumInfo>("mixxx::deck::MediumInfo");
    s_pInstance = this;

    m_rescanDebounce.setSingleShot(true);
    m_rescanDebounce.setInterval(kRescanDebounceMs);
    connect(&m_rescanDebounce, &QTimer::timeout, this, &MediaRegistry::rescanLocal);

    connect(&m_readWatcher,
            &QFutureWatcher<ReadResult>::finished,
            this,
            &MediaRegistry::onReadFinished);

    if (QFileInfo::exists(kMediaRoot)) {
        m_watcher.addPath(kMediaRoot);
    }
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        m_rescanDebounce.start();
    });

    // And a poll behind it, because the watcher misses the case that matters.
    //
    // `directoryChanged` fires when /media gains or loses a child -- not when
    // one of those children is unmounted. A stick is normally *both* unmounted
    // and rmdir'd, so the watcher usually sees it; but the rmdir is best-effort
    // (`|| true` in dj-usb) and fails whenever anything holds the directory. It
    // fails silently, and what follows is a medium that stays in SOURCES for
    // ever, listing tracks that cannot be read, with its cached copies still
    // marked re-readable -- so they can be evicted, and the only copy of the
    // track under the needle goes with them.
    //
    // Two seconds and a stat per mount point. The same interval the Rust side
    // scans volumes on, and for the same reason.
    m_rescanPoll.setInterval(2000);
    connect(&m_rescanPoll, &QTimer::timeout, this, &MediaRegistry::rescanLocal);
    m_rescanPoll.start();

    rescanLocal();

#ifdef __PROLINK__
    // The players on the network, watched from here. Passive: it binds and
    // listens, and the counts on a source row come out of the status packets a
    // player already sends, so a remote medium is fully described before
    // anything is fetched.
    m_pNetwork = std::make_unique<mixxx::prolink::ProLinkNetworkService>();
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::mediaInfoFound,
            this,
            &MediaRegistry::onMediaInfo);
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::databaseFetched,
            this,
            &MediaRegistry::onDatabaseFetched);
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::deviceLost,
            this,
            &MediaRegistry::onDeviceLost);
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::deviceFound,
            this,
            [this](const mixxx::prolink::ProLinkDevice& device) {
                m_devices.append(device);
            });
    // Connected once, for the life of the registry, rather than per transfer.
    // A streamed file's ranges must be recorded even while nothing is waiting
    // on them -- the deck is decoding out of it the whole time -- so there is
    // no window in which these can be off.
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::fileFetchProgress,
            this,
            &MediaRegistry::onFetchProgress);
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            this,
            &MediaRegistry::onFetchFinished);
    connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::artworkFetched,
            this,
            &MediaRegistry::onArtworkFetched);
    m_pNetwork->start();
#endif

    // Whoever asked for us before we existed. Drained *last*, deliberately:
    // the rescan above emits mediumAppeared for the sticks that were already
    // plugged in at boot, and a toast for each of those on every startup is
    // noise rather than news.
    const auto waiting = std::exchange(s_pending, {});
    for (const auto& entry : waiting) {
        if (!entry.first.isNull()) {
            entry.second(this);
        }
    }
}

MediaRegistry::~MediaRegistry() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
    // The worker holds a pool pointer and writes to the database; letting it
    // finish is cheaper than making every step of it cancellable.
    if (m_readWatcher.isRunning()) {
        m_readWatcher.waitForFinished();
    }
}

int MediaRegistry::indexOf(const MediumId& id) const {
    for (int i = 0; i < m_media.size(); ++i) {
        if (m_media.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

QStringList MediaRegistry::findLocalMountPoints() {
    QStringList mountPoints;
    // Immediate children of /media only, matching where dj-usb mounts and what
    // the Rekordbox feature looked at before it.
    const QFileInfoList children = QDir(kMediaRoot)
                                           .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& child : children) {
        const QString pdb = QDir(child.absoluteFilePath()).filePath(kPdbPath);
        if (QFileInfo::exists(pdb)) {
            mountPoints.append(child.absoluteFilePath());
        }
    }
    mountPoints.sort();
    return mountPoints;
}

void MediaRegistry::rescanLocal() {
    const QStringList mountPoints = findLocalMountPoints();

    bool changed = false;

    // Gone: anything local we hold that is no longer mounted.
    for (int i = m_media.size() - 1; i >= 0; --i) {
        const MediumInfo& medium = m_media.at(i);
        if (!medium.id.isLocal()) {
            continue;
        }
        if (mountPoints.contains(medium.id.mountPoint())) {
            continue;
        }
        const MediumInfo gone = medium;
        m_media.removeAt(i);
        {
            const mixxx::DbConnectionPooler pooler(m_dbConnectionPool);
            QSqlDatabase database = mixxx::DbConnectionPooled(m_dbConnectionPool);
            if (database.isOpen()) {
                clearMedium(database, gone.id);
            }
        }
        changed = true;
        kLogger.info() << "medium gone:" << gone.name;
        emit mediumVanished(gone);
    }

    // New: anything mounted we do not hold yet.
    for (const QString& mountPoint : mountPoints) {
        const MediumId id = MediumId::local(mountPoint);
        if (indexOf(id) >= 0) {
            continue;
        }
        MediumInfo medium;
        medium.id = id;
        medium.name = volumeLabelFor(mountPoint);
        medium.kind = MediumInfo::Kind::Usb;
        // "/media/DJ_USB_2" -> 2. Anything mounted elsewhere has no slot, which
        // is right: the number means a port on this deck, not an ordinal.
        const QString slotName = QFileInfo(mountPoint).fileName();
        if (slotName.startsWith(QStringLiteral("DJ_USB_"))) {
            medium.slot = slotName.mid(7).toInt();
        }
        medium.state = MediumInfo::State::Reading;
        m_media.append(medium);
        PendingRead pending;
        pending.id = id;
        pending.mountPoint = mountPoint;
        pending.localRoot = mountPoint;
        m_readQueue.append(pending);
        changed = true;
        kLogger.info() << "medium found:" << medium.name << "at" << mountPoint;
        emit mediumAppeared(medium);
    }

    if (changed) {
        emit mediaChanged();
    }
    startNextRead();
}

void MediaRegistry::enqueue(PendingRead pending) {
    m_readQueue.append(std::move(pending));
    startNextRead();
}

void MediaRegistry::startNextRead() {
    if (m_reading || m_readQueue.isEmpty()) {
        return;
    }
    // One at a time, local and remote alike: two parses would fight over one
    // USB bus or one network for no gain, and the ingest holds a write
    // transaction while it runs.
    const PendingRead next = m_readQueue.takeFirst();
    m_reading = true;
    m_readWatcher.setFuture(
            QtConcurrent::run(&MediaRegistry::readMedium, m_dbConnectionPool, next));
}

QString MediaRegistry::remoteCacheRoot(const MediumId& id) {
    QString key = id.key();
    key.replace(QChar(':'), QChar('_')).replace(QChar('|'), QChar('-'));

    // Boot-scoped scratch, and in RAM. What is on somebody else's stick is
    // worthless the moment they unplug it, so none of it is worth a write to
    // the SD card -- and under CacheLocation, where this used to be, nothing
    // ever cleaned it up either: every medium ever seen left its covers and
    // beat grids on the card for good.
    //
    // The same measured store as the track cache's first tier; see RamStore for
    // why the size of it cannot be assumed.
    static const QString root = RamStore::path(QStringLiteral("remote"));
    return QDir(root).filePath(key);
}

MediaRegistry::ReadResult MediaRegistry::readMedium(
        mixxx::DbConnectionPoolPtr pool, PendingRead pending) {
    ReadResult result;
    result.id = pending.id;

    // A connection of this thread's own. A QSqlDatabase cannot be shared across
    // threads, which is what the pooler exists to arrange.
    const mixxx::DbConnectionPooler pooler(pool);
    QSqlDatabase database = mixxx::DbConnectionPooled(pool);
    if (!database.isOpen()) {
        result.error = QStringLiteral("no database connection");
        return result;
    }

    // A local medium is read off its mount; a remote one arrives with the bytes
    // already fetched. Past this line the two are the same thing, which is the
    // whole point of there being one ingest.
    QByteArray raw = pending.data;
    if (raw.isEmpty()) {
        const QString pdbPath = QDir(pending.mountPoint).filePath(kPdbPath);
        QFile pdbFile(pdbPath);
        if (!pdbFile.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("cannot read %1").arg(pdbPath);
            return result;
        }
        raw = pdbFile.readAll();
    }

    mixxx::prolink::PdbContents contents = mixxx::prolink::parsePdb(raw);
    if (!contents.ok) {
        result.error = contents.error;
        return result;
    }

    // Track locations are the root plus the medium-relative path the pdb
    // stores, concatenated rather than translated (see PdbIngest). For a stick
    // that root is the mount; for a remote medium it is where its files will be
    // mirrored once fetched.
    result.ingest = writeMedium(database, contents, pending.id, pending.localRoot);
    result.ok = true;
    return result;
}

void MediaRegistry::onReadFinished() {
    m_reading = false;
    const ReadResult result = m_readWatcher.result();

    const int index = indexOf(result.id);
    if (index >= 0) {
        MediumInfo& medium = m_media[index];
        if (result.ok) {
            medium.state = MediumInfo::State::Ready;
            medium.trackCount = result.ingest.trackCount;
            medium.playlistCount = result.ingest.playlistCount;
            kLogger.info() << "medium ready:" << medium.name << medium.trackCount
                           << "tracks," << medium.playlistCount << "playlists";
        } else {
            medium.state = MediumInfo::State::Failed;
            medium.error = result.error;
            kLogger.warning() << "medium failed:" << medium.name << result.error;
            emit mediumFailed(medium);
        }
        emit mediaChanged();
    }

    startNextRead();
}

#ifdef __PROLINK__

int MediaRegistry::playerNumberFor(const QByteArray& mac) const {
    for (const mixxx::prolink::ProLinkDevice& device : m_devices) {
        if (device.mac == mac) {
            return device.deviceNumber;
        }
    }
    return 0;
}

void MediaRegistry::onMediaInfo(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const mixxx::prolink::MediaInfo& info) {
    const MediumId id = MediumId::proLink(QString::fromLatin1(mac.toHex()),
            static_cast<int>(slot));
    const int index = indexOf(id);

    if (!info.isOccupied()) {
        // An empty slot answers too, with everything zeroed -- which is how a
        // medium being taken out of a player reaches us.
        if (index >= 0) {
            const MediumInfo gone = m_media.takeAt(index);
            const mixxx::DbConnectionPooler pooler(m_dbConnectionPool);
            QSqlDatabase database = mixxx::DbConnectionPooled(m_dbConnectionPool);
            if (database.isOpen()) {
                clearMedium(database, gone.id);
            }
            kLogger.info() << "remote medium gone:" << gone.name;
            emit mediumVanished(gone);
            emit mediaChanged();
        }
        return;
    }

    if (index >= 0) {
        // Already known. Refresh what the player says and leave the rest --
        // re-fetching a database we already hold would cost seconds of network
        // for nothing.
        MediumInfo& medium = m_media[index];
        if (!info.name.isEmpty()) {
            medium.name = info.name;
        }
        medium.playerNumber = playerNumberFor(mac);
        emit mediaChanged();
        return;
    }

    MediumInfo medium;
    medium.id = id;
    // An unlabelled medium is normal and is not an error: the slot kind is the
    // name in that case, exactly as a CDJ shows it.
    medium.name = info.name.isEmpty()
            ? (slot == mixxx::prolink::MediaSlot::Sd ? QStringLiteral("SD")
                                                     : QStringLiteral("USB"))
            : info.name;
    medium.kind = slot == mixxx::prolink::MediaSlot::Sd ? MediumInfo::Kind::Sd
                                                        : MediumInfo::Kind::Usb;
    medium.playerNumber = playerNumberFor(mac);
    // The counts come free, from the status packet. So the row is complete
    // before the database has been touched -- which is the whole reason a
    // remote source can be listed instantly.
    medium.trackCount = static_cast<int>(info.trackCount);
    medium.playlistCount = static_cast<int>(info.playlistCount);
    medium.state = MediumInfo::State::Reading;
    m_media.append(medium);
    kLogger.info() << "remote medium found:" << medium.name << "on player"
                   << medium.playerNumber;
    emit mediumAppeared(medium);
    emit mediaChanged();

    // Read on detection, like a stick: the fetch takes seconds, and doing it
    // now is what makes entering it instant later.
    m_pNetwork->fetchDatabase(mac, slot);
}

void MediaRegistry::onDatabaseFetched(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const QByteArray& data,
        const QString& error) {
    const MediumId id = MediumId::proLink(QString::fromLatin1(mac.toHex()),
            static_cast<int>(slot));
    const int index = indexOf(id);
    if (index < 0) {
        return; // The medium went away while we were fetching it.
    }
    if (!error.isEmpty() || data.isEmpty()) {
        m_media[index].state = MediumInfo::State::Failed;
        m_media[index].error = error.isEmpty() ? tr("empty database") : error;
        kLogger.warning() << "remote medium failed:" << m_media[index].name << error;
        emit mediumFailed(m_media[index]);
        emit mediaChanged();
        return;
    }

    PendingRead pending;
    pending.id = id;
    pending.data = data;
    pending.localRoot = remoteCacheRoot(id);
    enqueue(std::move(pending));
}

bool MediaRegistry::addressOf(const MediumId& medium,
        QByteArray* pMac,
        mixxx::prolink::MediaSlot* pSlot) const {
    if (medium.isLocal() || !medium.isValid()) {
        return false;
    }
    // "prolink:<mac hex>|<slot>", as MediumId::proLink built it. Parsed rather
    // than carried alongside because the key is what survives a round trip
    // through SQL, and everything here comes back out of the database.
    const QString key = medium.key();
    const int bar = key.indexOf(QChar('|'));
    if (bar < 0) {
        return false;
    }
    const QString hex = key.mid(QStringLiteral("prolink:").size(),
            bar - static_cast<int>(QStringLiteral("prolink:").size()));
    *pMac = QByteArray::fromHex(hex.toLatin1());
    *pSlot = static_cast<mixxx::prolink::MediaSlot>(key.mid(bar + 1).toInt());
    return !pMac->isEmpty();
}

void MediaRegistry::fetchCompanionBlocking(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const QString& remotePath,
        const QString& localPath) {
    if (remotePath.isEmpty() || localPath.isEmpty() || QFileInfo::exists(localPath)) {
        return;
    }

    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    bool finished = false;
    const auto connection = connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            &loop,
            [&loop, &finished, localPath](const QString& path, const QString&) {
                if (path == localPath) {
                    finished = true;
                    loop.quit();
                }
            });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, [&loop]() { loop.quit(); });
    timeout.start(kCompanionTimeoutMs);

    m_pNetwork->fetchFile(mac, slot, remotePath, localPath, true);
    // A fetch that cannot even start -- no session, or a player that has left --
    // reports it by emitting synchronously, from inside that call. quit() on a
    // loop that has not begun does nothing, so entering it anyway would wait out
    // the whole timeout for a failure already in hand.
    if (!finished) {
        // ExcludeUserInputEvents, like every nested loop on this path: the
        // caller has snapshotted its row and must not have the model reset
        // underneath it.
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    disconnect(connection);

    if (QFileInfo::exists(localPath)) {
        kLogger.debug() << "fetched" << remotePath << "in" << elapsed.elapsed() << "ms";
    } else {
        // Tolerated. A track with no beat grid still plays; it just arrives
        // unanalysed, and that is a far better outcome than refusing to load.
        kLogger.warning() << "no" << remotePath << "after" << elapsed.elapsed() << "ms";
    }
}

QString MediaRegistry::startStreaming(const MediumId& medium,
        const QString& sourcePath,
        const QString& analyzePath) {
    TrackCache* pCache = TrackCache::instance();
    if (pCache == nullptr || sourcePath.isEmpty()) {
        return QString();
    }
    QByteArray mac;
    mixxx::prolink::MediaSlot slot = mixxx::prolink::MediaSlot::Usb;
    if (!addressOf(medium, &mac, &slot)) {
        return QString();
    }
    const QString localPath = pCache->localPathFor(medium, sourcePath);
    if (localPath.isEmpty()) {
        return QString();
    }

    // Already here in full, from an earlier load. Note that the filesystem
    // cannot answer this -- a half-streamed file is on disk at its full size
    // with zeros in the gaps, and it plays as silence rather than failing.
    if (m_streamComplete.contains(localPath) && QFileInfo::exists(localPath)) {
        kLogger.debug() << "already streamed in full:" << sourcePath;
        return localPath;
    }
    // Already arriving: the same track loaded twice, or loaded again while it
    // downloads. Handing back the stream in flight is right and restarting it
    // is not -- a restart would delete the file the deck is reading and leave
    // the finished transfer reporting completion for a stream nobody holds.
    if (m_streaming.contains(localPath) && StreamingFileRegistry::lookup(localPath)) {
        // It is wanted again, so it is no longer waiting to be swept up.
        m_removeWhenDone.remove(localPath);
        kLogger.debug() << "already streaming:" << sourcePath;
        return localPath;
    }
    // Arriving, but with nothing left holding it -- which cannot happen while
    // the stream stays registered for the life of its transfer, and is checked
    // for anyway because the alternative is two transfers writing one file.
    if (m_streaming.contains(localPath)) {
        kLogger.warning() << "a transfer is already running for" << sourcePath;
        return QString();
    }
    if (m_startingStream) {
        kLogger.warning() << "already starting a stream; refusing" << sourcePath;
        return QString();
    }
    // The medium has to still be there. Checked before anything is deleted
    // below, because a player that has left cannot be streamed from and the
    // local file may by then be the only copy of the track in existence --
    // spilled to tier 2 exactly because the medium went away.
    if (indexOf(medium) < 0) {
        kLogger.warning() << "that medium is no longer on the network:" << medium.key();
        return QString();
    }

    // The medium-relative path, which is what the player is asked for. The
    // location column is the local root and that path concatenated, so this is
    // the same string the pdb held.
    const QString root = remoteCacheRoot(medium);
    if (!sourcePath.startsWith(root)) {
        kLogger.warning() << "not under this medium's root:" << sourcePath;
        return QString();
    }
    const QString remotePath = sourcePath.mid(root.size());

    // The beat grid FIRST, before the audio.
    //
    // Not merely for tidiness: the library runs one transfer at a time against
    // a player, and a streaming fetch holds its turn for the whole download.
    // Anything queued behind it would wait for the entire track -- so a grid
    // asked for afterwards would arrive minutes late, long after the Track it
    // had to be applied to existed.
    if (!analyzePath.isEmpty() && analyzePath.startsWith(root)) {
        const QString datRemote = analyzePath.mid(root.size());
        fetchCompanionBlocking(mac, slot, datRemote, analyzePath);
        // Grids are only right in the legacy .DAT, cues only in the .EXT, so
        // both are wanted. Upper case deliberately: rekordbox writes them that
        // way and a player looks for exactly those names.
        if (datRemote.endsWith(QStringLiteral(".DAT"))) {
            fetchCompanionBlocking(mac,
                    slot,
                    datRemote.left(datRemote.size() - 3) + QStringLiteral("EXT"),
                    analyzePath.left(analyzePath.size() - 3) + QStringLiteral("EXT"));
        }
    }

    // A file left over from a stream that did not finish. Removed rather than
    // reused, because its holes are indistinguishable from silence.
    QFile::remove(localPath);
    StreamingFileRegistry::remove(localPath);

    m_startingStream = true;
    m_streaming.insert(localPath);

    QEventLoop loop;
    QString error;
    const auto ready = connect(this,
            &MediaRegistry::streamingReady,
            &loop,
            [&loop, localPath](const QString& path) {
                if (path == localPath) {
                    loop.quit();
                }
            });
    // A transfer that fails before it ever reports a size ends here instead.
    const auto failed = connect(m_pNetwork.get(),
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            &loop,
            [&loop, &error, localPath](const QString& path, const QString& reason) {
                if (path != localPath) {
                    return;
                }
                error = reason.isEmpty() ? tr("the transfer ended before it started")
                                         : reason;
                loop.quit();
            });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, [&loop, &error]() {
        error = tr("timed out waiting for the file size");
        loop.quit();
    });
    timeout.start(kStreamStartTimeoutMs);

    QElapsedTimer elapsed;
    elapsed.start();
    m_pNetwork->fetchFileStreaming(mac, slot, remotePath, localPath, kStreamHeadBytes);
    // A transfer that cannot even start -- no session, or a player that has
    // left -- reports it by emitting synchronously, from inside that call.
    // quit() on a loop that has not begun does nothing, so entering it anyway
    // would freeze the deck for the whole timeout over a failure already known.
    if (error.isEmpty() && !StreamingFileRegistry::lookup(localPath)) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    disconnect(ready);
    disconnect(failed);
    m_startingStream = false;

    const auto pStream = StreamingFileRegistry::lookup(localPath);
    if (!pStream) {
        kLogger.warning() << "could not start streaming" << remotePath << "--"
                          << (error.isEmpty() ? tr("no size was reported") : error);
        m_streaming.remove(localPath);
        QFile::remove(localPath);
        return QString();
    }

    // The number that says whether this is working. It should be well under a
    // second: it covers one connect, mount, open and stat, and nothing else.
    kLogger.info() << "streaming" << remotePath << "--" << pStream->size()
                   << "bytes, playable after" << elapsed.elapsed() << "ms";
    // So the file counts against the RAM cap. Pinned by the caller immediately
    // after this returns, which is why no eviction may run in between.
    pCache->adopt(medium, localPath, pStream->size());
    return localPath;
}

void MediaRegistry::stopStreaming(const QString& localPath) {
    if (localPath.isEmpty()) {
        return;
    }
    const auto pStream = StreamingFileRegistry::lookup(localPath);
    if (!pStream) {
        return;
    }
    if (pStream->isComplete()) {
        // A whole file now, so it is an ordinary one: unregistering it is what
        // makes the next open of it go through the normal decoders.
        StreamingFileRegistry::remove(localPath);
        return;
    }

    // Still arriving, and deliberately left alone. Not abandoned, and not
    // unregistered:
    //
    //  * the download is left running because those bytes are already paid for,
    //    and a complete file in tier 1 is what makes playing it again instant;
    //  * the record of which ranges have landed has to outlive the load, or a
    //    reload before the download finishes could not tell a real byte from a
    //    sparse hole and would have to wait for the whole file.
    //
    // The cost is that a read blocked at the moment of the unload stays blocked
    // until the next range lands -- under a second in practice, and bounded by
    // the read timeout regardless. That happens on the reader's own thread.
    m_removeWhenDone.insert(localPath);
    kLogger.info() << "unloaded while still arriving, after" << pStream->waitCount()
                   << "waits /" << pStream->waitedMs() << "ms:" << localPath;
}

mixxx::prolink::server::ServeStatus MediaRegistry::serveStatus() const {
    return m_pNetwork ? m_pNetwork->serveStatus() : mixxx::prolink::server::ServeStatus();
}

void MediaRegistry::requestArtwork(const QString& coverPath) {
    if (coverPath.isEmpty() || m_artworkAsked.contains(coverPath)) {
        return;
    }
    // Asked at most once whatever happens, including when it fails. A cover the
    // player does not have would otherwise be requested on every single repaint
    // of the row it belongs to.
    m_artworkAsked.insert(coverPath);
    if (QFileInfo::exists(coverPath)) {
        return;
    }

    // Which medium and which image, looked up by the path itself. The caller is
    // a delegate mid-paint and has nothing but the path -- it does not know
    // which medium the row came from, and should not have to.
    const mixxx::DbConnectionPooler pooler(m_dbConnectionPool);
    QSqlDatabase database = mixxx::DbConnectionPooled(m_dbConnectionPool);
    if (!database.isOpen()) {
        return;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT medium, artwork_id FROM %1 WHERE coverart_location = :path LIMIT 1")
                          .arg(kLibraryTable));
    query.bindValue(QStringLiteral(":path"), coverPath);
    if (!query.exec() || !query.next()) {
        return;
    }
    const MediumId medium = MediumId::fromKey(query.value(0).toString());
    const quint32 artworkId = query.value(1).toUInt();
    if (medium.isLocal() || artworkId == 0) {
        // A local stick carries its own images, so a missing one there is
        // missing on the medium and no amount of asking will produce it.
        return;
    }

    QByteArray mac;
    mixxx::prolink::MediaSlot slot = mixxx::prolink::MediaSlot::Usb;
    if (!addressOf(medium, &mac, &slot)) {
        return;
    }
    // Over dbserver rather than NFS, and fire and forget. Asking NFS for an
    // image churns the player's filehandle table until it answers NFSERR_STALE
    // to everything -- including the track a DJ is loading (F49). It also means
    // covers do not queue behind a streaming transfer, which holds the one NFS
    // turn for the length of a whole download.
    m_pNetwork->fetchArtwork(mac, slot, artworkId, coverPath);
}

void MediaRegistry::onArtworkFetched(const QString& localPath, const QString& error) {
    if (!error.isEmpty() || !QFileInfo::exists(localPath)) {
        // Normal and not worth a warning: a rekordbox medium always has a few
        // tracks with no art, and the row draws its grey square either way.
        return;
    }
    emit artworkArrived(localPath);
}

void MediaRegistry::onFetchProgress(const QString& localPath,
        quint64 done,
        quint64 total,
        quint64 offset,
        quint64 length) {
    if (!m_streaming.contains(localPath)) {
        return; // An ordinary whole-file fetch. Not ours to make readable.
    }
    auto pStream = StreamingFileRegistry::lookup(localPath);
    if (!pStream) {
        if (total == 0) {
            return; // The size is not known yet, so there is nothing to build.
        }
        // Built here rather than in startStreaming() because the events are
        // drained in batches: the size and the first range of content often
        // arrive together, and a range that lands before the file exists to
        // record it against is a range nothing will ever be told about.
        pStream = std::make_shared<StreamingFile>(localPath, static_cast<qint64>(total));
        if (!pStream->error().isEmpty()) {
            kLogger.warning() << "cannot read back" << localPath << "--"
                              << pStream->error();
            return;
        }
        StreamingFileRegistry::add(localPath, pStream);
        emit streamingReady(localPath);
    }
    if (length > 0) {
        pStream->markPresent(static_cast<qint64>(offset), static_cast<qint64>(length));
    }
    Q_UNUSED(done);
}

void MediaRegistry::onFetchFinished(const QString& localPath, const QString& error) {
    if (m_streaming.remove(localPath) == 0) {
        return; // Not a streamed track.
    }
    const auto pStream = StreamingFileRegistry::lookup(localPath);
    if (error.isEmpty()) {
        m_streamComplete.insert(localPath);
        // The line that says the switch from a blocking read to an ordinary one
        // is safe to make, and the one to look for when a track stutters: if it
        // is late, the download lost the race with the playhead.
        kLogger.info() << "download complete:" << localPath
                       << (pStream ? QStringLiteral("-- %1 waits / %2 ms")
                                                .arg(pStream->waitCount())
                                                .arg(pStream->waitedMs())
                                   : QStringLiteral("-- no longer loaded"));
        if (pStream) {
            pStream->complete();
            // Unloaded while it was still arriving, and kept registered only so
            // its ranges would survive a reload. It is a whole file now, so
            // nothing is owed to it.
            if (m_removeWhenDone.remove(localPath) > 0) {
                StreamingFileRegistry::remove(localPath);
            }
        }
        return;
    }

    kLogger.warning() << "download failed:" << localPath << "--" << error;
    m_removeWhenDone.remove(localPath);
    if (pStream) {
        // Wakes readers, which then fail rather than hang. Without this the
        // deck would keep waiting on bytes that are never coming.
        pStream->fail(error);
        // Unregistered whatever happens: a failed stream must never be handed
        // to a decoder opening this path again.
        StreamingFileRegistry::remove(localPath);
    }
    // Whatever arrived is not a playable file and must not be mistaken for one
    // by the next load.
    QFile::remove(localPath);
}

void MediaRegistry::onDeviceLost(const QByteArray& mac) {
    m_devices.erase(std::remove_if(m_devices.begin(),
                            m_devices.end(),
                            [&mac](const mixxx::prolink::ProLinkDevice& d) {
                                return d.mac == mac;
                            }),
            m_devices.end());

    const QString prefix = QStringLiteral("prolink:") +
            QString::fromLatin1(mac.toHex()) + QChar('|');
    bool changed = false;
    for (int i = m_media.size() - 1; i >= 0; --i) {
        if (!m_media.at(i).id.key().startsWith(prefix)) {
            continue;
        }
        const MediumInfo gone = m_media.takeAt(i);
        {
            const mixxx::DbConnectionPooler pooler(m_dbConnectionPool);
            QSqlDatabase database = mixxx::DbConnectionPooled(m_dbConnectionPool);
            if (database.isOpen()) {
                clearMedium(database, gone.id);
            }
        }
        kLogger.info() << "player gone, medium with it:" << gone.name;
        emit mediumVanished(gone);
        changed = true;
    }
    if (changed) {
        emit mediaChanged();
    }
}

#else // __PROLINK__

QString MediaRegistry::startStreaming(const MediumId& medium,
        const QString& sourcePath,
        const QString& analyzePath) {
    // Without Pro DJ Link there are no remote media to stream from, so this is
    // never reached rather than merely unsupported.
    Q_UNUSED(medium);
    Q_UNUSED(sourcePath);
    Q_UNUSED(analyzePath);
    return QString();
}

void MediaRegistry::stopStreaming(const QString& localPath) {
    Q_UNUSED(localPath);
}

void MediaRegistry::requestArtwork(const QString& coverPath) {
    // A local medium carries its own images, so there is never anything to ask
    // for without Pro DJ Link.
    Q_UNUSED(coverPath);
}

mixxx::prolink::server::ServeStatus MediaRegistry::serveStatus() const {
    return {};
}

#endif // __PROLINK__

} // namespace deck
} // namespace mixxx

#include "moc_mediaregistry.cpp"
