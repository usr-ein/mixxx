#pragma once

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>

#include "library/deck/folderscan.h"
#include "library/deck/mediumid.h"
#include "library/deck/pdbingest.h"
#include "network/prolink/prolinktypes.h"
#include "network/prolink/prolinkservestatus.h"
#include "util/db/dbconnectionpool.h"

namespace mixxx {
namespace prolink {
class ProLinkNetworkService;
class ProLinkKeySync;
} // namespace prolink
namespace deck {

/// One source row: a volume the deck can play from.
struct MediumInfo {
    enum class Kind {
        Usb,
        Sd,
    };
    /// How the medium describes what is on it.
    enum class Format {
        /// A rekordbox export, read out of its `export.pdb`.
        Rekordbox,
        /// Loose files and no usable library: read out of its directory tree
        /// (docs/plain-usb-plan.md). Local media only, for now.
        Folder,
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
    /// Folder media only: the directories that hold music, which is what the
    /// source row counts instead of playlists.
    int folderCount = 0;
    /// Settled when the medium has been read. Guessed until then from whether
    /// there is an `export.pdb` at all; a pdb that turns out to be unreadable
    /// makes it Folder after all.
    Format format = Format::Rekordbox;
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
    /// Reads the sticks already in, and watches for more. The players on the
    /// network are watched through *pNetwork*, which is used and not owned,
    /// from start().
    MediaRegistry(mixxx::DbConnectionPoolPtr dbConnectionPool,
            mixxx::prolink::ProLinkNetworkService* pNetwork,
            QObject* pParent = nullptr);
    ~MediaRegistry() override;

    /// Join the network, and hand the registry to whoever asked for it before
    /// it existed (whenReady()).
    ///
    /// Apart from the constructor so that what listens to the registry can be
    /// connected first: the session's first answers come out of this.
    void start();

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

    /// The MAC and slot behind a remote medium's id, or false for a local one.
    static bool addressOf(const MediumId& medium,
            QByteArray* pMac,
            mixxx::prolink::MediaSlot* pSlot);

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
    mixxx::prolink::ServeStatus serveStatus() const;

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
    /// Something worth a toast that is none of the three above. Today: a
    /// rekordbox library that could not be read, so the stick is being browsed
    /// by its folders instead.
    void mediumNotice(mixxx::deck::MediumInfo medium, const QString& text);
    /// A medium's rows changed in place -- a folder medium's tags arriving in
    /// the background. Not a change of shape, so nothing should rebuild a
    /// level or move a selection over it; the list on screen just re-reads.
    /// *rbIds* are the rows that changed, by their id within the medium.
    void mediumUpdated(const QString& mediumKey, const QList<quint32>& rbIds);

  private slots:
    void onReadFinished();
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
    /// Add *device*, or update the one with its MAC.
    void upsertDevice(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceLost(const QByteArray& mac);
    void onArtworkFetched(const QString& localPath, const QString& error);
    void onPreviewFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            quint32 trackId,
            const QByteArray& blob,
            const QString& error);
    /// The tempo master loaded something else. Remember it, and resolve.
    void onMasterTrackChanged(int masterPlayer,
            int sourcePlayer,
            mixxx::prolink::MediaSlot slot,
            quint32 trackId);

    /// Work out what key the tempo master is playing in, and tell KEY SYNC.
    ///
    /// **This is the only thing that can answer the question**, and it is why
    /// it is answered here rather than in the network layer: the key is not on
    /// the wire. A status packet says which player, which slot and which
    /// rekordbox id, and the key belonging to that triple is in the copy of
    /// that medium's database this registry ingested when the medium appeared.
    ///
    /// **Which is why it runs on every change to the media list as well as on
    /// every change of master track.** The two halves of the answer arrive
    /// seconds apart and in either order: a CDJ that was already master when
    /// this deck booted is announced within a poll, and the database it is
    /// playing off takes as long as it takes to fetch and ingest. Resolved once
    /// when the master was announced, the answer was "no key" for ever after —
    /// and the only way back was to take mastership away from the CDJ and hand
    /// it back, which is a thing to do to a rig mid-set and not a thing to ask
    /// anybody to do.
    ///
    /// Cheap enough to run on every media change: one lookup on a unique index,
    /// and it publishes nothing unless the answer moved.
    void resolveMasterKey();

  private:
    /// Directories under /media the deck can read: every real mount -- a
    /// stick of loose files is as much a medium as a rekordbox one -- and any
    /// directory holding PIONEER/rekordbox/export.pdb, mounted or not.
    static QStringList findLocalMountPoints();
    /// The local medium mounted at *mountPoint*, or -1. By mount point and
    /// not by id, because the id also carries the stick's UUID.
    int indexOfLocal(const QString& mountPoint) const;
    void startNextRead();

    /// Player number for a MAC, or 0. Linear over a handful of devices.
    int playerNumberFor(const QByteArray& mac) const;
    /// The medium *player* keeps in *slot*, or an invalid id.
    ///
    /// Handles the case a rig makes ordinary and a lone deck never sees: the
    /// master playing a track off a stick that is in **this** deck, over LINK.
    /// That medium is one of ours and is not on the network at all.
    MediumId mediumOf(int player, mixxx::prolink::MediaSlot slot) const;

    /// One pending read. A local medium names a mount to read from; a remote
    /// one arrives with the bytes already in hand, because the network layer
    /// fetched them.
    struct PendingRead {
        enum class Kind {
            /// Read the whole medium: its pdb, or its directory tree.
            Medium,
            /// Read tags for some of a folder medium's tracks, already listed.
            Tags,
        };
        Kind kind = Kind::Medium;
        MediumId id;
        QString mountPoint; ///< Local media only.
        QByteArray data;    ///< Remote media only.
        QString localRoot;
        /// Folder media: what the files at the very top of the stick are
        /// listed as -- its own name.
        QString volumeName;
        /// Tags only.
        QList<FolderTagTarget> tagTargets;
    };
    /// What a worker produced. Copyable, because QFuture demands it.
    struct ReadResult {
        PendingRead::Kind kind = PendingRead::Kind::Medium;
        MediumId id;
        IngestResult ingest;
        bool ok = false;
        QString error;
        MediumInfo::Format format = MediumInfo::Format::Rekordbox;
        int folderCount = 0;
        /// For a toast: why a stick with a pdb is being read as folders.
        QString notice;
        /// A folder medium's tracks, whose tags are read next, in chunks.
        QList<FolderTagTarget> tagTargets;
        /// Tags only: how many rows the batch changed, and which.
        int tagsUpdated = 0;
        QList<quint32> updatedRbIds;
    };
    static ReadResult readMedium(mixxx::DbConnectionPoolPtr pool, PendingRead pending);
    /// The folder half of readMedium(): pass A, and the list pass B will read.
    /// Also where a stick lands whose pdb cannot be read.
    static ReadResult readFolderMedium(QSqlDatabase* pDatabase,
            const PendingRead& pending,
            ReadResult result);
    void enqueue(PendingRead pending);

    mixxx::DbConnectionPoolPtr m_dbConnectionPool;
    QList<MediumInfo> m_media;
    /// Media detected but not yet read, in detection order. One read at a time:
    /// two sticks parsed in parallel would fight over one USB bus for no gain.
    QList<PendingRead> m_readQueue;
    QFutureWatcher<ReadResult> m_readWatcher;
    bool m_reading = false;
    /// A tag read is waiting for a track copy to finish, and a retry is
    /// already scheduled; see startNextRead().
    bool m_tagRetryScheduled = false;

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

    /// The Pro DJ Link session, used and not owned.
    mixxx::prolink::ProLinkNetworkService* const m_pNetwork;
    /// What announceLoadedTrack() was last asked to say, so it can be asked
    /// again when what we serve changes.
    MediumId m_announcedMedium;
    quint32 m_announcedRekordboxId = 0;
    /// KEY SYNC. Owned here for one reason: it needs a key resolved out of the
    /// medium databases, and this is what holds those.
    std::unique_ptr<mixxx::prolink::ProLinkKeySync> m_pKeySync;
    /// What the network last said the tempo master is playing. Kept because the
    /// key it resolves to can change without any of it changing — see
    /// resolveMasterKey().
    int m_masterPlayer = 0;
    int m_masterSourcePlayer = 0;
    mixxx::prolink::MediaSlot m_masterSlot = mixxx::prolink::MediaSlot::Empty;
    quint32 m_masterTrackId = 0;
    /// What was last handed to KEY SYNC, so a resolution that comes out the
    /// same is dropped rather than republished. A ChromaticKey, as an int, so
    /// this header does not have to pull in the key protobuf.
    int m_publishedMasterKeyId = 0;
    bool m_publishedOtherIsMaster = false;
    bool m_masterKeyPublished = false;
    QList<mixxx::prolink::ProLinkDevice> m_devices;

    /// Covers already asked for, whether or not they arrived. A paint asks on
    /// every redraw, so without this a cover the player does not have would be
    /// requested for as long as its row is on screen.
    QSet<QString> m_artworkAsked;
};

} // namespace deck
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::deck::MediumInfo)
