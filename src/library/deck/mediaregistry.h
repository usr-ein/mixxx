#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QTimer>

#include "library/deck/mediumid.h"
#include "library/deck/pdbingest.h"
#include "network/prolink/prolinkdefs.h"
#include "network/prolink/prolinkdevice.h"
#include "network/prolink/prolinkmediaquery.h"
#include "util/db/dbconnectionpool.h"

namespace mixxx {
namespace prolink {
class ProLinkNetworkService;
}
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
    /// Local media only: which of the deck's ports it is in, from the mount
    /// point (DJ_USB_1 -> 1). Slots are handed out in plug order, so this is
    /// unrelated to the name and is exactly why it has to be shown.
    int slot = 0;
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

    /// The one that exists, or null before the browser is built.
    ///
    /// A deliberate shortcut, and a small one. The toast widget has to watch
    /// these signals, and it lives at the top of the skin's stack rather than
    /// inside the browser -- a stick landing mid-set has to be visible over the
    /// waveform, not only over a menu. Wiring the two through the skin parser
    /// would make widget creation order load-bearing in a file skin authors
    /// edit, which is a worse thing to owe than one accessor.
    static MediaRegistry* instance();

    const QList<MediumInfo>& media() const {
        return m_media;
    }
    /// -1 when there is no such medium.
    int indexOf(const MediumId& id) const;

    /// Look again for local sticks. Cheap, and safe to call from a signal.
    void rescanLocal();

    /// Where a remote medium's files are mirrored locally. Track locations are
    /// this plus the medium-relative path the pdb stores, concatenated rather
    /// than translated, so the two never have to be reconciled.
    static QString remoteCacheRoot(const MediumId& id);

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
#ifdef __PROLINK__
    /// A player answered about one of its slots. This is where a remote medium
    /// is born -- and it already carries the counts, straight from the status
    /// packet, so the source row is complete before a byte is fetched.
    void onMediaInfo(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const mixxx::prolink::MediaInfo& info);
    void onDatabaseFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QByteArray& data,
            const QString& error);
    void onDeviceLost(const QByteArray& mac);
#endif

  private:
    /// Directories under /media whose child holds PIONEER/rekordbox/export.pdb.
    static QStringList findLocalMountPoints();
    void startNextRead();

#ifdef __PROLINK__
    /// Player number for a MAC, or 0. Linear over a handful of devices.
    int playerNumberFor(const QByteArray& mac) const;
#endif

    /// What a worker produced. Copyable, because QFuture demands it.
    struct ReadResult {
        MediumId id;
        IngestResult ingest;
        bool ok = false;
        QString error;
    };
    /// One pending read. A local medium names a mount to read from; a remote
    /// one arrives with the bytes already in hand, because the network layer
    /// fetched them.
    struct PendingRead {
        MediumId id;
        QString mountPoint; ///< Local media only.
        QByteArray data;    ///< Remote media only.
        QString localRoot;
    };
    static ReadResult readMedium(mixxx::DbConnectionPoolPtr pool, PendingRead pending);
    void enqueue(PendingRead pending);

    mixxx::DbConnectionPoolPtr m_dbConnectionPool;
    QList<MediumInfo> m_media;
    /// Media detected but not yet read, in detection order. One read at a time:
    /// two sticks parsed in parallel would fight over one USB bus for no gain.
    QList<PendingRead> m_readQueue;
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

#ifdef __PROLINK__
    /// Owned here rather than by a library feature, because the browser is the
    /// only thing that shows media now and the old ProLinkFeature no longer has
    /// a UI to hang off.
    std::unique_ptr<mixxx::prolink::ProLinkNetworkService> m_pNetwork;
    QList<mixxx::prolink::ProLinkDevice> m_devices;
#endif
};

} // namespace deck
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::deck::MediumInfo)
