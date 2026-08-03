#include "library/deck/mediaregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QtConcurrentRun>

#include "library/deck/deckqueries.h"
#include "library/deck/volumelabel.h"
#include "network/prolink/prolinkpdb.h"
#include "util/db/dbconnectionpooler.h"
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

MediaRegistry::MediaRegistry(mixxx::DbConnectionPoolPtr dbConnectionPool, QObject* pParent)
        : QObject(pParent),
          m_dbConnectionPool(std::move(dbConnectionPool)) {
    qRegisterMetaType<mixxx::deck::MediumInfo>("mixxx::deck::MediumInfo");

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
}

MediaRegistry::~MediaRegistry() {
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
        m_readQueue.append({id, mountPoint});
        changed = true;
        kLogger.info() << "medium found:" << medium.name << "at" << mountPoint;
        emit mediumAppeared(medium);
    }

    if (changed) {
        emit mediaChanged();
    }
    startNextRead();
}

void MediaRegistry::startNextRead() {
    if (m_reading || m_readQueue.isEmpty()) {
        return;
    }
    const auto next = m_readQueue.takeFirst();
    m_reading = true;
    m_readWatcher.setFuture(QtConcurrent::run(
            &MediaRegistry::readMedium, m_dbConnectionPool, next.first, next.second));
}

MediaRegistry::ReadResult MediaRegistry::readMedium(
        mixxx::DbConnectionPoolPtr pool, MediumId id, QString mountPoint) {
    ReadResult result;
    result.id = id;

    // A connection of this thread's own. A QSqlDatabase cannot be shared across
    // threads, which is what the pooler exists to arrange.
    const mixxx::DbConnectionPooler pooler(pool);
    QSqlDatabase database = mixxx::DbConnectionPooled(pool);
    if (!database.isOpen()) {
        result.error = QStringLiteral("no database connection");
        return result;
    }

    const QString pdbPath = QDir(mountPoint).filePath(kPdbPath);
    QFile pdbFile(pdbPath);
    if (!pdbFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("cannot read %1").arg(pdbPath);
        return result;
    }

    // Through the same shim, and so the same Rust parser, the remote side uses.
    // Reading the file in whole rather than calling read_pdb(path) keeps both
    // sources on one code path; a rekordbox export is a megabyte or two, so
    // there is nothing to gain by streaming it.
    mixxx::prolink::PdbContents contents = mixxx::prolink::parsePdb(pdbFile.readAll());
    if (!contents.ok) {
        result.error = contents.error;
        return result;
    }

    // Track locations are the mount point plus the medium-relative path the pdb
    // stores, concatenated rather than translated (see PdbIngest).
    result.ingest = writeMedium(database, contents, id, mountPoint);
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

} // namespace deck
} // namespace mixxx

#include "moc_mediaregistry.cpp"
