#include "library/deck/mediaregistry.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUrl>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "library/deck/deckqueries.h"
#include "library/deck/folderlibrary.h"
#include "library/deck/ramstore.h"
#include "library/deck/trackcache.h"
#include "library/deck/volumelabel.h"
#include "library/rekordbox/rekordboxpdb.h"
#include "network/prolink/prolinkkeysync.h"
#include "network/prolink/prolinknetworkservice.h"
#include "track/keyutils.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("MediaRegistry");

const QString kMediaRoot = QStringLiteral("/media");
const QString kPdbPath = QStringLiteral("PIONEER/rekordbox/export.pdb");
/// The mount is still settling when the watcher fires; give it a moment.
constexpr int kRescanDebounceMs = 400;

/// A folder medium's tags are read this many files at a time. Small enough
/// that a stick plugged in meanwhile, or a DJ loading a track, never waits on
/// more than a second or two of somebody else's tag reads; large enough that
/// the batch's one transaction is not mostly overhead.
constexpr int kTagChunk = 40;
/// How long tag reads wait for a track copy to get off the USB bus.
constexpr int kTagYieldMs = 500;

/// Whether *path* is where a stick is mounted: a USB disk (/dev/sd*), as
/// dj-usb mounts them, rather than a directory on the filesystem it sits in.
/// dj-usb removes a slot's directory on eject, but only best-effort, and an
/// empty leftover is not a stick. Nor is the system: a release card's
/// read-only root mounts its two layers under /media too (overlayroot's
/// /media/root-ro and /media/root-rw), and a deck listed them as drives.
bool isStickMount(const QString& path) {
    const QStorageInfo info(path);
    return info.isValid() && info.isReady() &&
            QDir::cleanPath(info.rootPath()) == QDir::cleanPath(path) &&
            info.device().startsWith(QByteArrayLiteral("/dev/sd"));
}

/// The medium in a player's slot. MediaRegistry::addressOf() goes back.
mixxx::deck::MediumId remoteMediumId(const QByteArray& mac, mixxx::prolink::MediaSlot slot) {
    return mixxx::deck::MediumId::proLink(
            QString::fromLatin1(mac.toHex()), static_cast<int>(slot));
}

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

MediaRegistry::MediaRegistry(mixxx::DbConnectionPoolPtr dbConnectionPool,
        mixxx::prolink::ProLinkNetworkService* pNetwork,
        QObject* pParent)
        : QObject(pParent),
          m_dbConnectionPool(std::move(dbConnectionPool)),
          m_pNetwork(pNetwork) {
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

    // The players on the network, watched from here. Passive: it binds and
    // listens, and the counts on a source row come out of the status packets a
    // player already sends, so a remote medium is fully described before
    // anything is fetched.
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::mediaInfoFound,
            this,
            &MediaRegistry::onMediaInfo);
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::databaseFetched,
            this,
            &MediaRegistry::onDatabaseFetched);
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::deviceLost,
            this,
            &MediaRegistry::onDeviceLost);
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::deviceFound,
            this,
            [this](const mixxx::prolink::ProLinkDevice& device) {
                upsertDevice(device);
            });
    // A player renumbered in its UTILITY menu and back within the forget
    // window arrives as a change, not a loss and a find: without this its old
    // number stayed here for the session, and a master on that player
    // resolved to nothing -- or, after two decks swapped, to the other deck's
    // stick and the wrong key.
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::deviceChanged,
            this,
            [this](const mixxx::prolink::ProLinkDevice& device) {
                upsertDevice(device);
                resolveMasterKey();
            });
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::artworkFetched,
            this,
            &MediaRegistry::onArtworkFetched);
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::previewFetched,
            this,
            &MediaRegistry::onPreviewFetched);
    // What the network is told is loaded depends on what we are serving, and
    // that is not known for the first seconds of a session -- nor after a
    // rebind, nor until a stick has been mounted and offered. A track loaded in
    // that window was announced as nothing at all, and stayed that way: no CDJ
    // would follow us or draw our phase until the next load. So it is asked
    // again whenever the answer could have changed.
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::serveStatusChanged,
            this,
            [this]() {
                if (m_announcedRekordboxId != 0) {
                    announceLoadedTrack(m_announcedMedium, m_announcedRekordboxId);
                }
                // The master playing off our own stick resolves through what
                // we serve, so the answer can change with it.
                resolveMasterKey();
            });
    // ...and through our own player number, which arrives some seconds into a
    // session and may change on a rebind.
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::announceChanged,
            this,
            [this]() {
                resolveMasterKey();
            });
    m_pKeySync = std::make_unique<mixxx::prolink::ProLinkKeySync>();
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::masterTrackChanged,
            this,
            &MediaRegistry::onMasterTrackChanged);
    // The master's key is half a network answer and half a database one, and
    // the database half lands whenever a medium finishes being read. Hung off
    // our own signal rather than off the one place that emits it, because
    // every path that changes the media list emits this one and any of them
    // can be the moment the answer becomes knowable.
    connect(this, &MediaRegistry::mediaChanged, this, &MediaRegistry::resolveMasterKey);
}

void MediaRegistry::start() {
    m_pNetwork->start();

    // Whoever asked for us before we existed. Drained *last*, deliberately:
    // the constructor's rescan emits mediumAppeared for the sticks that were
    // already plugged in at boot, and a toast for each of those on every
    // startup is noise rather than news.
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
    // Stop listening to the ProLink service first, because tearing it down is
    // not silent.
    //
    // ~ProLinkNetworkService calls shutdown(), which reports every device it
    // still had as lost -- and this object is what listens for that. Heard
    // while this registry is part-way destroyed, onDeviceLost() runs a
    // database query against it.
    //
    // That is exactly what it did, when the service was a member of this
    // class. glibc aborted inside malloc during clearMedium()'s
    // QSqlResult::savePrepare, part-way through
    // QObjectPrivate::deleteChildren -- so Mixxx segfaulted on every shutdown,
    // before CoreServices::finalize() had saved anything, mixxx.cfg included,
    // because of a signal emitted from a destructor.
    if (m_pNetwork) {
        m_pNetwork->disconnect(this);
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
        const QString path = child.absoluteFilePath();
        // Any stick that is mounted, library or not: a stick of loose files
        // was dropped here once, silently, and a DJ with one saw nothing at
        // all. A rekordbox export still counts wherever it is, mounted or not,
        // as it did before -- a development box keeps one in a plain folder.
        if (QFileInfo::exists(QDir(path).filePath(kPdbPath)) || isStickMount(path)) {
            mountPoints.append(path);
        }
    }
    mountPoints.sort();
    return mountPoints;
}

int MediaRegistry::indexOfLocal(const QString& mountPoint) const {
    for (int i = 0; i < m_media.size(); ++i) {
        if (m_media.at(i).id.isLocal() && m_media.at(i).id.mountPoint() == mountPoint) {
            return i;
        }
    }
    return -1;
}

void MediaRegistry::rescanLocal() {
    const QStringList mountPoints = findLocalMountPoints();

    bool changed = false;

    // Gone: anything local we hold that is no longer mounted -- or that is
    // mounted, but is now a different stick. Slots are handed out in plug
    // order, so a stick pulled and another pushed in between two scans lands at
    // the same mount point, and only its UUID says it is not the same one.
    for (int i = m_media.size() - 1; i >= 0; --i) {
        const MediumInfo& medium = m_media.at(i);
        if (!medium.id.isLocal()) {
            continue;
        }
        if (mountPoints.contains(medium.id.mountPoint())) {
            const QString held = medium.id.volumeId();
            const QString now = held.isEmpty() ? QString() : volumeIdFor(medium.id.mountPoint());
            // Either side unknown is no evidence of a swap: a sidecar written a
            // moment after the mount would otherwise read as a new stick.
            if (held.isEmpty() || now.isEmpty() || now == held) {
                continue;
            }
            kLogger.info() << "a different stick is now at" << medium.id.mountPoint();
        }
        const MediumInfo gone = medium;
        m_media.removeAt(i);
        // Whatever was still queued for it -- its tags, typically -- belongs to
        // a stick that is not there any more.
        m_readQueue.erase(std::remove_if(m_readQueue.begin(),
                                  m_readQueue.end(),
                                  [&gone](const PendingRead& pending) {
                                      return pending.id == gone.id;
                                  }),
                m_readQueue.end());
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
        if (indexOfLocal(mountPoint) >= 0) {
            continue;
        }
        const MediumId id = MediumId::local(mountPoint, volumeIdFor(mountPoint));
        MediumInfo medium;
        medium.id = id;
        medium.name = volumeLabelFor(mountPoint);
        medium.kind = MediumInfo::Kind::Usb;
        // A guess until it has been read: the pdb may yet turn out unreadable.
        // Made now because the insert toast says which it is.
        medium.format = QFileInfo::exists(QDir(mountPoint).filePath(kPdbPath))
                ? MediumInfo::Format::Rekordbox
                : MediumInfo::Format::Folder;
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
        pending.volumeName = medium.name;
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
    //
    // Whole media before tags. A stick waiting to appear in SOURCES outranks
    // the titles of one that is already browsable, so a second stick plugged in
    // while the first one's tags are being read does not wait behind them.
    int next = -1;
    for (int i = 0; i < m_readQueue.size(); ++i) {
        if (m_readQueue.at(i).kind == PendingRead::Kind::Medium) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        // Only tags left, and tags give way to a track being copied off a
        // stick: it is the same USB bus, and the copy is what the DJ who just
        // pressed load is waiting on.
        const TrackCache* pCache = TrackCache::instance();
        if (pCache && pCache->isCopying()) {
            if (!m_tagRetryScheduled) {
                m_tagRetryScheduled = true;
                QTimer::singleShot(kTagYieldMs, this, [this]() {
                    m_tagRetryScheduled = false;
                    startNextRead();
                });
            }
            return;
        }
        next = 0;
    }
    const PendingRead pending = m_readQueue.takeAt(next);
    m_reading = true;
    m_readWatcher.setFuture(
            QtConcurrent::run(&MediaRegistry::readMedium, m_dbConnectionPool, pending));
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
    result.kind = pending.kind;
    result.id = pending.id;

    // A connection of this thread's own. A QSqlDatabase cannot be shared across
    // threads, which is what the pooler exists to arrange.
    const mixxx::DbConnectionPooler pooler(pool);
    QSqlDatabase database = mixxx::DbConnectionPooled(pool);
    if (!database.isOpen()) {
        result.error = QStringLiteral("no database connection");
        return result;
    }

    // Pass B of a folder medium: a batch of its files' tags, written over what
    // their names said.
    if (pending.kind == PendingRead::Kind::Tags) {
        const QList<TrackTagUpdate> updates = readFolderTags(pending.tagTargets);
        result.tagsUpdated = updateTrackTags(database, pending.id, updates);
        result.updatedRbIds.reserve(updates.size());
        for (const TrackTagUpdate& update : updates) {
            result.updatedRbIds.append(update.rbId);
        }
        result.ok = true;
        return result;
    }

    // A local medium is read off its mount; a remote one arrives with the bytes
    // already fetched. Past this line the two are the same thing, which is the
    // whole point of there being one ingest.
    const bool local = !pending.mountPoint.isEmpty();
    QByteArray raw = pending.data;
    if (raw.isEmpty() && local) {
        const QString pdbPath = QDir(pending.mountPoint).filePath(kPdbPath);
        if (QFileInfo::exists(pdbPath)) {
            QFile pdbFile(pdbPath);
            if (pdbFile.open(QIODevice::ReadOnly)) {
                raw = pdbFile.readAll();
            }
            if (raw.isEmpty()) {
                result.notice = tr("rekordbox library unreadable, browsing folders");
                kLogger.warning() << "cannot read" << pdbPath << "-- reading the folders instead";
            }
        }
    }
    if (raw.isEmpty()) {
        if (!local) {
            result.error = tr("empty database");
            return result;
        }
        return readFolderMedium(&database, pending, std::move(result));
    }

    // Keep the bytes we ingested. A medium's pdb is the one input to all of
    // this and it is otherwise unrecoverable -- a remote one never touches the
    // disk, and a stick can be unplugged -- so "this playlist came up empty"
    // is a question nobody can answer afterwards without it. 660 kB, replaced
    // per medium, and it makes the difference between reading the parser and
    // reading the data.
    {
        // databaseName() is a URL here ("file:/home/.../mixxxdb.sqlite"), not
        // a path, and QFileInfo on it yields something relative to $HOME.
        const QString name = database.databaseName();
        const QString file = name.startsWith(QLatin1String("file:"))
                ? QUrl(name).toLocalFile()
                : name;
        const QString dir = QFileInfo(file).absolutePath();
        const QString path = QDir(dir).filePath(QStringLiteral("last-ingest.pdb"));
        QFile copy(path);
        if (copy.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            copy.write(raw);
            kLogger.info() << "kept the ingested pdb:" << path << raw.size() << "bytes";
        } else {
            kLogger.warning() << "could not keep the ingested pdb at" << path
                              << copy.errorString();
        }
    }

    mixxx::rekordbox::PdbContents contents = mixxx::rekordbox::parsePdb(raw);
    if (!contents.ok) {
        if (!local) {
            result.error = contents.error;
            return result;
        }
        // A stick whose library cannot be read is still a stick of music, and
        // its files are right there. Browsed by folder rather than refused --
        // that also covers a stick written by rekordbox 7 for Device Library
        // Plus alone, whose classic pdb may be absent or empty of its tracks.
        kLogger.warning() << "unreadable pdb on" << pending.mountPoint << "--" << contents.error
                          << "-- reading the folders instead";
        result.notice = tr("rekordbox library unreadable, browsing folders");
        return readFolderMedium(&database, pending, std::move(result));
    }

    // Track locations are the root plus the medium-relative path the pdb
    // stores, concatenated rather than translated (see PdbIngest). For a stick
    // that root is the mount; for a remote medium it is where its files will be
    // mirrored once fetched.
    result.ingest = writeMedium(database, contents, pending.id, pending.localRoot);
    result.format = MediumInfo::Format::Rekordbox;
    result.ok = true;
    return result;
}

MediaRegistry::ReadResult MediaRegistry::readFolderMedium(QSqlDatabase* pDatabase,
        const PendingRead& pending,
        ReadResult result) {
    result.format = MediumInfo::Format::Folder;
    QElapsedTimer elapsed;
    elapsed.start();

    // Pass A: the walk and the tree, from names, sizes and dates alone. No file
    // is opened, which is what makes a stick browsable within seconds of going
    // in; its tags follow in pass B.
    const FolderListing listing = walkFolderMedium(pending.mountPoint);
    const FolderLibrary library = buildFolderLibrary(listing, pending.volumeName);
    if (library.contents.tracks.isEmpty()) {
        result.error = tr("no music found");
        return result;
    }
    result.ingest = writeMedium(*pDatabase, library.contents, pending.id, pending.mountPoint);
    result.folderCount = library.directoryCount;
    result.ok = true;
    result.tagTargets.reserve(library.contents.tracks.size());
    for (const mixxx::rekordbox::PdbTrack& track : library.contents.tracks) {
        FolderTagTarget target;
        target.rbId = track.id;
        target.path = pending.mountPoint + track.filePath;
        result.tagTargets.append(target);
    }
    kLogger.info() << "read" << pending.mountPoint << "as folders:" << result.ingest.trackCount
                   << "tracks in" << result.folderCount << "folders, in" << elapsed.elapsed()
                   << "ms" << (listing.truncated ? "(stopped at the cap)" : "");
    return result;
}

void MediaRegistry::onReadFinished() {
    m_reading = false;
    const ReadResult result = m_readWatcher.result();

    const int index = indexOf(result.id);
    if (result.kind == PendingRead::Kind::Tags) {
        // Rows changed in place. Nothing changed shape, so this is not
        // mediaChanged: that rebuilds the source list and would make the
        // browser treat a stick's tags arriving like a stick arriving.
        if (index >= 0 && result.tagsUpdated > 0) {
            emit mediumUpdated(result.id.key(), result.updatedRbIds);
        }
        startNextRead();
        return;
    }

    if (index >= 0) {
        MediumInfo& medium = m_media[index];
        medium.format = result.format;
        medium.folderCount = result.folderCount;
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
        if (!result.notice.isEmpty()) {
            emit mediumNotice(medium, result.notice);
        }
        // Pass B, a chunk at a time, behind anything already waiting. Each
        // chunk is its own read so that the queue can put a newly plugged-in
        // stick, or a track copy, ahead of the rest.
        for (int start = 0; start < result.tagTargets.size(); start += kTagChunk) {
            PendingRead tags;
            tags.kind = PendingRead::Kind::Tags;
            tags.id = result.id;
            tags.tagTargets = result.tagTargets.mid(start, kTagChunk);
            m_readQueue.append(tags);
        }
        emit mediaChanged();
    }

    startNextRead();
}

int MediaRegistry::playerNumberFor(const QByteArray& mac) const {
    for (const mixxx::prolink::ProLinkDevice& device : m_devices) {
        if (device.mac == mac) {
            return device.deviceNumber;
        }
    }
    return 0;
}

MediumId MediaRegistry::mediumOf(int player, mixxx::prolink::MediaSlot slot) const {
    if (player <= 0 || slot == mixxx::prolink::MediaSlot::Empty) {
        return MediumId();
    }
    if (m_pNetwork && player == m_pNetwork->announcedNumber()) {
        // Ours. The other deck is playing off a stick in this one, which it
        // reached over LINK, so the medium is a mount point and not a network
        // address -- and the tracks on it were ingested when the stick went in.
        // Looked up by mount point rather than rebuilt from it: a local id
        // carries the stick's UUID too, which the serve status does not.
        for (const auto& served : m_pNetwork->serveStatus().media) {
            // Not a phantom: that stick has been pulled, and its mount point
            // may already belong to the next one -- whose rows, looked up by
            // the old track's id, would name an unrelated track.
            if (served.slot == slot && !served.localPath.isEmpty() && !served.phantom) {
                const int index = indexOfLocal(served.localPath);
                return index >= 0 ? m_media.at(index).id : MediumId();
            }
        }
        return MediumId();
    }
    for (const mixxx::prolink::ProLinkDevice& device : m_devices) {
        if (device.deviceNumber == player && !device.mac.isEmpty()) {
            return remoteMediumId(device.mac, slot);
        }
    }
    return MediumId();
}

void MediaRegistry::onMasterTrackChanged(int masterPlayer,
        int sourcePlayer,
        mixxx::prolink::MediaSlot slot,
        quint32 trackId) {
    m_masterPlayer = masterPlayer;
    m_masterSourcePlayer = sourcePlayer;
    m_masterSlot = slot;
    m_masterTrackId = trackId;
    resolveMasterKey();
}

void MediaRegistry::resolveMasterKey() {
    if (!m_pKeySync) {
        return;
    }
    int keyId = 0;
    const MediumId medium = mediumOf(m_masterSourcePlayer, m_masterSlot);
    if (m_masterTrackId != 0 && medium.isValid()) {
        const mixxx::DbConnectionPooler pooler(m_dbConnectionPool);
        QSqlDatabase database = mixxx::DbConnectionPooled(m_dbConnectionPool);
        if (database.isOpen()) {
            keyId = keyIdForTrack(database, medium, m_masterTrackId);
        }
    }
    const bool otherIsMaster = m_masterPlayer != 0;
    // The first answer is always published: a registry rebuilt with the skin
    // starts from (false, 0), and the controls it publishes to outlived the
    // last one, still saying what that one said.
    if (m_masterKeyPublished && otherIsMaster == m_publishedOtherIsMaster &&
            keyId == m_publishedMasterKeyId) {
        // Nothing moved. Worth checking, because this runs on every change to
        // the media list and most of those have nothing to do with the master.
        return;
    }
    m_masterKeyPublished = true;
    m_publishedOtherIsMaster = otherIsMaster;
    m_publishedMasterKeyId = keyId;
    const auto key = KeyUtils::keyFromNumericValue(keyId);
    kLogger.debug() << "master is player" << m_masterPlayer << "playing"
                    << medium.key() << m_masterTrackId << "in"
                    << KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot);
    m_pKeySync->setLink(otherIsMaster, key);
}

void MediaRegistry::onMediaInfo(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const mixxx::prolink::MediaInfo& info) {
    const MediumId id = remoteMediumId(mac, slot);
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
    const MediumId id = remoteMediumId(mac, slot);
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
        mixxx::prolink::MediaSlot* pSlot) {
    if (medium.isLocal() || !medium.isValid()) {
        return false;
    }
    // Read back out of the id rather than carried alongside it, because the
    // id is what survives a round trip through SQL, and everything here
    // comes back out of the database.
    *pMac = QByteArray::fromHex(medium.deviceKey().toLatin1());
    *pSlot = static_cast<mixxx::prolink::MediaSlot>(medium.slot());
    return !pMac->isEmpty();
}

void MediaRegistry::announceLoadedTrack(const MediumId& medium, quint32 rekordboxId) {
    // Kept so it can be asked again when what we serve changes: see the
    // serveStatusChanged connection in the constructor.
    m_announcedMedium = medium;
    m_announcedRekordboxId = rekordboxId;
    if (!m_pNetwork || rekordboxId == 0) {
        kLogger.debug() << "nothing to announce as loaded -- rekordbox id" << rekordboxId
                        << "for" << medium.key();
        announceNothingLoaded();
        return;
    }
    // Whose medium, in the network's terms. A remote medium belongs to the
    // player it came from; a local stick belongs to us, and a CDJ browsing it
    // over LINK sees exactly that -- so this is the same answer a real deck
    // gives when it plays off its own USB.
    int player = 0;
    mixxx::prolink::MediaSlot slot = mixxx::prolink::MediaSlot::Usb;
    if (medium.isLocal()) {
        // Which slot we are serving it in, rather than an assumption: two
        // sticks are served as USB and SD, and naming the wrong one sends a
        // peer looking in an empty slot.
        //
        // **And nothing at all when we are not serving it.** This used to fall
        // back to the USB slot, which names a track that is not there -- or,
        // worse, a different track in whatever stick *is* being served as USB.
        // A folder medium is never served (docs/plain-usb-plan.md D5), so for
        // one of those this is the only right answer.
        bool served = false;
        for (const mixxx::prolink::ServedSlot& candidate :
                m_pNetwork->serveStatus().media) {
            if (!candidate.localPath.isEmpty() && medium.mountPoint() == candidate.localPath) {
                slot = candidate.slot;
                served = true;
                break;
            }
        }
        if (!served) {
            kLogger.debug() << "not serving" << medium.key() << "-- announcing nothing loaded";
            announceNothingLoaded();
            return;
        }
        // Not announcedNumber(), which is 0 for the first seconds of a session
        // and may change on a rebind: the service resolves this to whatever
        // number we hold each time it publishes.
        player = mixxx::prolink::kThisPlayer;
    } else {
        QByteArray mac;
        if (!addressOf(medium, &mac, &slot)) {
            announceNothingLoaded();
            return;
        }
        player = playerNumberFor(mac);
    }
    if (player == 0) {
        // Nothing to attribute the track to. Saying nothing is right: a track
        // id without a player is meaningless to every other device.
        announceNothingLoaded();
        return;
    }
    kLogger.debug() << "announcing what is loaded: player" << player << "slot"
                    << static_cast<int>(slot) << "rekordbox id" << rekordboxId;
    m_pNetwork->setLoadedTrack(player, slot, rekordboxId);
}

void MediaRegistry::announceNothingLoaded() {
    if (m_pNetwork) {
        m_pNetwork->setLoadedTrack(0, mixxx::prolink::MediaSlot::Empty, 0);
    }
}

mixxx::prolink::ServeStatus MediaRegistry::serveStatus() const {
    return m_pNetwork ? m_pNetwork->serveStatus() : mixxx::prolink::ServeStatus();
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

void MediaRegistry::requestPreview(const MediumId& medium, quint32 rekordboxId) {
    if (medium.isLocal() || rekordboxId == 0 || !m_pNetwork) {
        // A local medium has its analysis files on the stick, and reading them
        // is both cheaper and better -- the file carries the colour preview,
        // which the wire does not.
        return;
    }
    QByteArray mac;
    mixxx::prolink::MediaSlot slot = mixxx::prolink::MediaSlot::Usb;
    if (!addressOf(medium, &mac, &slot)) {
        return;
    }
    m_pNetwork->fetchWaveformPreview(mac, slot, rekordboxId);
}

void MediaRegistry::onPreviewFetched(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        quint32 trackId,
        const QByteArray& blob,
        const QString& error) {
    if (!error.isEmpty()) {
        // Not worth a warning. A medium has hundreds of tracks and a player
        // answers for the ones it has; the strip draws its baseline either way.
        kLogger.debug() << "no preview for track" << trackId << "--" << error;
    }
    // Back to a MediumId, because that is what the browser keyed its request
    // by. The MAC and slot are what the network layer speaks.
    const MediumId id = remoteMediumId(mac, slot);
    if (indexOf(id) >= 0) {
        emit previewArrived(id, trackId, blob);
    }
}

void MediaRegistry::onArtworkFetched(const QString& localPath, const QString& error) {
    if (!error.isEmpty() || !QFileInfo::exists(localPath)) {
        // Normal and not worth a warning: a rekordbox medium always has a few
        // tracks with no art, and the row draws its grey square either way.
        return;
    }
    emit artworkArrived(localPath);
}

void MediaRegistry::upsertDevice(const mixxx::prolink::ProLinkDevice& device) {
    // By MAC: a number can move, and a re-read of the device table after
    // dropped events announces every device again.
    for (auto& known : m_devices) {
        if (known.mac == device.mac) {
            known = device;
            return;
        }
    }
    m_devices.append(device);
}

void MediaRegistry::onDeviceLost(const QByteArray& mac) {
    m_devices.erase(std::remove_if(m_devices.begin(),
                            m_devices.end(),
                            [&mac](const mixxx::prolink::ProLinkDevice& d) {
                                return d.mac == mac;
                            }),
            m_devices.end());

    const QString deviceKey = QString::fromLatin1(mac.toHex());
    bool changed = false;
    for (int i = m_media.size() - 1; i >= 0; --i) {
        const MediumId id = m_media.at(i).id;
        if (id.isLocal() || id.deviceKey() != deviceKey) {
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

} // namespace deck
} // namespace mixxx

#include "moc_mediaregistry.cpp"
