#include "library/deck/mediaregistry.h"

#include <QDir>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QtConcurrentRun>

#include "library/deck/deckqueries.h"
#include "library/deck/volumelabel.h"
#include "network/prolink/prolinkpdb.h"
#include "util/db/dbconnectionpooler.h"
#ifdef __PROLINK__
#include <QStandardPaths>

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
} // namespace

namespace mixxx {
namespace deck {

namespace {
MediaRegistry* s_pInstance = nullptr;
} // namespace

MediaRegistry* MediaRegistry::instance() {
    return s_pInstance;
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
    m_pNetwork->start();
#endif
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
    // Boot-scoped scratch: what is on somebody else's stick is worthless the
    // moment they unplug it.
    QString key = id.key();
    key.replace(QChar(':'), QChar('_')).replace(QChar('|'), QChar('-'));
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("deck/") + key);
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

#endif // __PROLINK__

} // namespace deck
} // namespace mixxx

#include "moc_mediaregistry.cpp"
