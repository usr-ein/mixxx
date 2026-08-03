#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QTimer>

#include "library/deck/mediumid.h"
#include "library/deck/pdbingest.h"
#include "util/db/dbconnectionpool.h"

namespace mixxx {
namespace deck {

/// One source row: a rekordbox-prepared volume the deck can play from.
struct MediumInfo {
    enum class Kind {
        Usb,
        Sd,
    };
    enum class State {
        Reading, ///< Being read. Selectable, not enterable; drawn dimmed.
        Ready,
        Failed,
        Offline, ///< Remote only: keep-alives stopped.
    };

    MediumId id;
    /// What the medium calls itself. Never the mount point (browser-prd.md 5).
    QString name;
    Kind kind = Kind::Usb;
    /// Remote media only: the owning player's device number, 1..4. 0 for local.
    int playerNumber = 0;
    int trackCount = 0;
    int playlistCount = 0;
    State state = State::Reading;
    QString error;

    bool isEnterable() const {
        return state == State::Ready && trackCount > 0;
    }
};

/// Everything the deck can play from, and what state it is in.
///
/// **The single source of truth for the source list and for toasts.** The
/// browser renders it, the toast widget watches its signals, and the
/// diagnostics page reads it — none of them poll the filesystem or the network
/// themselves.
///
/// Media are read **as soon as they are detected**, not when they are entered
/// (browser-prd.md 11.1). That is what makes every level below level 0 open
/// instantly, and it is also the only way level 0 can print track and playlist
/// counts at all — there is nothing to count until something has read the pdb.
class MediaRegistry : public QObject {
    Q_OBJECT

  public:
    explicit MediaRegistry(mixxx::DbConnectionPoolPtr dbConnectionPool,
            QObject* pParent = nullptr);
    ~MediaRegistry() override;

    const QList<MediumInfo>& media() const {
        return m_media;
    }
    /// -1 when there is no such medium.
    int indexOf(const MediumId& id) const;

    /// Look again for local sticks. Cheap, and safe to call from a signal.
    void rescanLocal();

  signals:
    /// The list changed shape: a medium appeared, vanished, or finished reading.
    /// The browser rebuilds level 0 on this.
    void mediaChanged();
    /// For toasts. Carries a copy, because the entry may be gone by the time
    /// the toast draws.
    void mediumAppeared(mixxx::deck::MediumInfo medium);
    void mediumVanished(mixxx::deck::MediumInfo medium);
    void mediumFailed(mixxx::deck::MediumInfo medium);

  private slots:
    void onReadFinished();

  private:
    /// Directories under /media whose child holds PIONEER/rekordbox/export.pdb.
    static QStringList findLocalMountPoints();
    void startNextRead();

    /// What a worker produced. Copyable, because QFuture demands it.
    struct ReadResult {
        MediumId id;
        IngestResult ingest;
        bool ok = false;
        QString error;
    };
    static ReadResult readMedium(mixxx::DbConnectionPoolPtr pool,
            MediumId id,
            QString mountPoint);

    mixxx::DbConnectionPoolPtr m_dbConnectionPool;
    QList<MediumInfo> m_media;
    /// Media detected but not yet read, in detection order. One read at a time:
    /// two sticks parsed in parallel would fight over one USB bus for no gain.
    QList<QPair<MediumId, QString>> m_readQueue;
    QFutureWatcher<ReadResult> m_readWatcher;
    bool m_reading = false;

    /// /media gains and loses children as sticks come and go. Cheaper and more
    /// responsive than polling, and it catches a mount made by something other
    /// than dj-usb.
    QFileSystemWatcher m_watcher;
    /// The watcher fires while the mount is still being set up, so a rescan is
    /// deferred rather than immediate -- otherwise the pdb is not there yet and
    /// the medium is recorded as failed.
    QTimer m_rescanDebounce;
};

} // namespace deck
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::deck::MediumInfo)
