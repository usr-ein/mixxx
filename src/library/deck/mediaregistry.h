#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>

#include "library/deck/mediumid.h"
#include "library/deck/pdbingest.h"
#include "network/prolink/prolinkdefs.h"
#include "network/prolink/prolinkdevice.h"
#include "network/prolink/prolinkmediaquery.h"
#include "network/prolink/server/prolinkservestatus.h"
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

    /// Run *callback* with the registry, now or as soon as it exists.
    ///
    /// **Construction order in the skin is not something to rely on**, and
    /// relying on it cost the toasts entirely: the skin puts `<DeckToast>`
    /// first — it has to, to render over everything — while the registry is
    /// built by `<DeckBrowser>` four hundred lines further down. So the toast
    /// asked for an instance that did not exist yet, connected to nothing, and
    /// silently never fired again.
    ///
    /// The one line of warning it logged was true and useless: by the time
    /// anyone read it the deck had been shipped for a week.
    ///
    /// *pContext* owns the subscription; a callback whose context has been
    /// destroyed is dropped rather than called.
    static void whenReady(QObject* pContext, std::function<void(MediaRegistry*)> callback);

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

    /// Make a remote track playable **now**, before it has finished arriving.
    ///
    /// Returns the local path to hand a decoder, or empty if it cannot be
    /// played. What comes back is a sparse file of the right size whose reads
    /// block until the bytes are there; the audio keeps arriving behind it,
    /// head first, and the deck plays out of it the whole time.
    ///
    /// **This blocks the GUI thread**, but only for one NFS connect, mount,
    /// open and stat — the size is all a caller needs, and the reader waits for
    /// everything else on its own thread. That is a fraction of a second
    /// against the tens of seconds the whole-file fetch it replaces took.
    ///
    /// *analyzePath* is the local path of the rekordbox `.DAT`, which is
    /// fetched in full before the audio starts and is why a streamed track has
    /// a beat grid. Pass it empty to skip.
    ///
    /// The caller must snapshot everything it needs off its model **before**
    /// calling: a nested event loop runs inside, and a QModelIndex does not
    /// survive it.
    QString startStreaming(const MediumId& medium,
            const QString& sourcePath,
            const QString& analyzePath);

    /// The track is off the deck: nothing is reading it any more.
    ///
    /// Wakes anything still blocked on a range and unregisters the file. The
    /// download itself is left to finish — the bytes are already paid for, and
    /// a complete file in tier 1 is what makes loading it again instant.
    void stopStreaming(const QString& localPath);

    /// Tell the network which track this deck has loaded, and whose medium it
    /// came from.
    ///
    /// **Not decoration: a tempo without this is ignored.** A CDJ publishes
    /// what it is playing alongside what tempo it is playing at, and one asked
    /// to follow a master that claims a tempo and no track does not follow it.
    ///
    /// *rekordboxId* is the row id in that medium's own `export.pdb`, which is
    /// the only identifier the network understands — the deck's own row id
    /// means nothing to anyone else.
    void announceLoadedTrack(const MediumId& medium, quint32 rekordboxId);

    /// Nothing is loaded here any more.
    void announceNothingLoaded();

    /// What we are offering to the players on the network, and who is reading
    /// it. Empty without Pro DJ Link.
    ///
    /// Includes whether a slot has gone **phantom** — the stick pulled while a
    /// player was still playing off it, now being fed from a copy.
    mixxx::prolink::server::ServeStatus serveStatus() const;

    /// Ask for a cover that is not on disk yet, if it belongs to a remote
    /// medium. Cheap, idempotent, and safe to call from a paint.
    ///
    /// **On the fly, and only what is looked at.** A medium holds hundreds of
    /// covers and a DJ sees a dozen at a time; asking for all of them on
    /// detection would put hundreds of requests in front of whatever the DJ
    /// does next. So the request comes from the row being drawn, each path is
    /// asked for at most once, and `artworkArrived` says when to draw again.
    ///
    /// Does nothing for a path that is already there, for a local medium — its
    /// images are on the mount — or for one already asked for.
    void requestArtwork(const QString& coverPath);

    /// Ask a player for one of its tracks' preview waveforms.
    ///
    /// Remote media only, and the only way to get one: a remote track's
    /// analysis files are not on this machine until the track is loaded, so
    /// there is no file to read. Answered by `previewArrived` with the 900
    /// bytes a player sends, or with an empty blob when it has none.
    ///
    /// Does nothing for a local medium, which has its files on the stick.
    void requestPreview(const MediumId& medium, quint32 rekordboxId);

  signals:
    /// A streamed file has become readable: its size is known and it is in the
    /// StreamingFileRegistry. Internal, and the only thing startStreaming()'s
    /// nested loop waits on.
    void streamingReady(const QString& localPath);
    /// A cover asked for by requestArtwork() is now on disk. Whoever drew the
    /// grey square in its place should draw again.
    void artworkArrived(const QString& coverPath);
    /// A remote track's preview waveform arrived. Empty when the player had
    /// none, which is not an error.
    void previewArrived(const MediumId& medium, quint32 rekordboxId, const QByteArray& blob);
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
    /// A range of a streamed file has landed, or its size is now known.
    ///
    /// **Building the StreamingFile happens here, not in startStreaming().**
    /// The network layer's events are drained in batches, so the size and the
    /// first range of content routinely arrive in the same batch; a builder
    /// anywhere else would miss that range, and a missed range is a reader
    /// blocked on it until the read times out.
    void onFetchProgress(const QString& localPath,
            quint64 done,
            quint64 total,
            quint64 offset,
            quint64 length);
    void onFetchFinished(const QString& localPath, const QString& error);
    void onArtworkFetched(const QString& localPath, const QString& error);
    void onPreviewFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            quint32 trackId,
            const QByteArray& blob,
            const QString& error);
#endif

  private:
#ifdef __PROLINK__
    /// The MAC and slot behind a remote medium's id, or false if it is not one
    /// of ours or its player has gone.
    bool addressOf(const MediumId& medium,
            QByteArray* pMac,
            mixxx::prolink::MediaSlot* pSlot) const;
    /// Fetch one small companion file and wait for it, tolerating failure.
    ///
    /// The ANLZ files need this: they have to be on disk before the Track
    /// exists to apply them to, so queue-and-hope is not enough.
    void fetchCompanionBlocking(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QString& remotePath,
            const QString& localPath);
#endif

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
    /// A slow backstop for what the watcher cannot see: a mount point that is
    /// unmounted without its directory going away. See the constructor.
    QTimer m_rescanPoll;

#ifdef __PROLINK__
    /// Owned here rather than by a library feature, because the browser is the
    /// only thing that shows media now and the old ProLinkFeature no longer has
    /// a UI to hang off.
    std::unique_ptr<mixxx::prolink::ProLinkNetworkService> m_pNetwork;
    QList<mixxx::prolink::ProLinkDevice> m_devices;

    /// Local paths we asked to be streamed. What tells a progress signal for a
    /// streamed track apart from one for an ordinary whole-file fetch, which
    /// must not be given a StreamingFile.
    QSet<QString> m_streaming;
    /// Streams that finished. A partly-downloaded file is indistinguishable
    /// from a whole one by looking at it — it is created at full size and the
    /// gaps read back as zeros — so "is it already here" cannot be answered by
    /// the filesystem and is answered by this instead.
    QSet<QString> m_streamComplete;
    /// Streams unloaded while still arriving, kept registered until their
    /// download finishes so a reload can still tell a real byte from a hole.
    QSet<QString> m_removeWhenDone;
    /// One start at a time. Two nested loops would unwind in the wrong order.
    bool m_startingStream = false;
    /// Covers already asked for, whether or not they arrived. A paint asks on
    /// every redraw, so without this a cover the player does not have would be
    /// requested for as long as its row is on screen.
    QSet<QString> m_artworkAsked;
#endif
};

} // namespace deck
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::deck::MediumInfo)
