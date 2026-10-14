#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
#include <memory>

#include "library/deck/historystore.h"
#include "library/deck/mediumid.h"
#include "library/deck/playhistory.h"
#include "network/prolink/prolinktypes.h"
#include "util/db/dbconnectionpool.h"

class ControlProxy;

namespace mixxx {
namespace prolink {
class ProLinkNetworkService;
} // namespace prolink
namespace deck {

class DeckLoader;
class MediaRegistry;
struct MediumInfo;

/// The History: what played on the link this power-up and the two before it,
/// from which stick, and for how long -- whichever player played it, this
/// deck, another TriMixxx or a CDJ.
///
/// **Sampled.** Four times a second it asks what plays: this deck's `play`
/// and the track DeckLoader put on it, and every player's status on the link.
/// history::Recorder turns that into plays starting and stopping, which go
/// into the session and, on DeckHistory's own thread, onto the card
/// (HistoryStore). While anything plays, a heartbeat every five seconds moves
/// the open plays' end on, so a pulled plug loses at most that.
///
/// **Named from what the deck already read.** A rekordbox track on any stick
/// the deck sees -- its own, or a player's on the link -- is a row of that
/// medium's database, which MediaRegistry ingested when the medium appeared:
/// title, artist, cover, all there before the track starts. The cover is
/// copied as a thumbnail while the stick is still in. A track nothing names
/// -- a CDJ's plain file, a deck announcing none -- is kept as "unknown",
/// with where it came from.
class DeckHistory : public QObject {
    Q_OBJECT

  public:
    /// Where the History is kept on a deck's card: the state partition on a
    /// release card, the root on a dev card (pi_config/trimixxx-history.conf).
    static QString defaultDirectory();

    /// How often what plays is sampled. A row's length is good to this.
    static constexpr int kSampleMs = 250;
    /// How often the open plays are written on while anything plays: what a
    /// cut loses, at most (Sam: 5 s).
    static constexpr int kHeartbeatMs = 5000;
    /// A player whose last status is older than this has gone: a cable
    /// pulled, a deck switched off. Its track stopped when it was last heard.
    static constexpr double kStatusGoneMs = 2000.0;
    /// How often the store's directory is looked for, while it is not there.
    static constexpr int kReopenMs = 5000;

    /// *group* is this deck. The rest are used and not owned. *directory* is
    /// where the store goes; with none there, the History is this power-up's,
    /// in RAM.
    DeckHistory(const QString& group,
            MediaRegistry* pRegistry,
            mixxx::prolink::ProLinkNetworkService* pNetwork,
            DeckLoader* pLoader,
            mixxx::DbConnectionPoolPtr dbConnectionPool,
            const QString& directory = defaultDirectory(),
            QObject* pParent = nullptr);
    ~DeckHistory() override;

    /// Newest first: this power-up's, then the last, then two ago. This
    /// power-up's is always there.
    const QList<HistorySession>& sessions() const {
        return m_sessions;
    }

    /// The History's clock: milliseconds of the boot's, which no NTP moves.
    /// What its plays are timed by, and what a recording's track list is cut
    /// against (DeckRecorder).
    static qint64 clockMs();
    /// Which power-up this is: `/proc/sys/kernel/random/boot_id`.
    static QByteArray bootId();

    /// This power-up's plays, those still open ending now: what a recording's
    /// track list is cut from (DeckRecorder).
    QList<history::Play> playsNow() const;

    /// The rows of the session at *index*, as the History lists them: the
    /// latest first (Sam). This power-up's are worked out on each call, its
    /// open plays ending now; the power-ups before it never change, and theirs
    /// were worked out once, when the store opened.
    QList<history::Row> rowsOf(int index) const;

    /// Where the cover of *track* is on this machine, or empty.
    QString artworkPath(const HistoryTrack& track) const;

    /// Whether the History is on the card. False: this power-up's only, and
    /// gone at the next.
    bool isStored() const {
        return m_stored;
    }

    /// Whether anything plays on the link now, this deck included.
    bool isPlaying() const {
        return !m_openPlays.isEmpty();
    }

  signals:
    /// What the History lists has changed: a track started or stopped, or a
    /// track's name arrived.
    void changed();
    /// A cover is now on disk where artworkPath() said it would be.
    void artworkArrived();

  private:
    /// What the deck knows of a track when it first plays: enough to name it,
    /// now or once its medium has been read.
    struct Origin {
        MediumId medium;
        quint32 rekordboxId = 0;
        bool rekordbox = false;
        /// Whose medium, in the network's numbers, and which slot.
        int sourcePlayer = 0;
        mixxx::prolink::MediaSlot slot = mixxx::prolink::MediaSlot::Empty;
    };

    /// Open the store on the card and put this power-up's History into it.
    /// False, and the History stays in RAM, when it will not open.
    bool openStore();
    void sample();
    void heartbeat();
    /// Plays started and stopped: into the session and onto the card.
    void apply(const QList<history::Recorder::Change>& changes);
    /// What a player on the link plays, keyed as tracks are told apart, and
    /// remembered under that key as its origin.
    QString keyFor(const mixxx::prolink::ProLinkPlayer& player);
    /// What a rekordbox track on *medium* is called in the History.
    QString mediumTrackKey(const MediumId& medium, quint32 rekordboxId);
    /// Which stick a slot holds, as a number that moves when another one is
    /// put in: see onMediumAppeared().
    int generationOf(const MediumId& medium) const;
    void onMediumAppeared(const MediumInfo& medium);
    /// A thumbnail's name in the store, from its track and the cover's path.
    static QString thumbnailName(const QString& trackKey, const QString& coverPath);
    /// The session's id for *key*, adding the track to it if it is new.
    int trackIdFor(const QString& key, HistoryWrite* pWrite);
    /// Fill in what the deck can say about *pTrack* from its origin: its
    /// source, and its name and cover once its medium has been read. True
    /// when there is nothing more to learn: named, or nameable by nothing.
    bool resolve(HistoryTrack* pTrack, const Origin& origin);
    /// The tracks of this power-up still unnamed, named if they can be now.
    void resolvePending();
    void requestArtwork(const QString& coverPath, const QString& name);
    void post(const HistoryWrite& write);
    HistorySession& current() {
        return m_sessions.first();
    }

    MediaRegistry* const m_pRegistry;
    mixxx::prolink::ProLinkNetworkService* const m_pNetwork;
    DeckLoader* const m_pLoader;
    const mixxx::DbConnectionPoolPtr m_dbConnectionPool;
    std::unique_ptr<ControlProxy> m_pPlay;
    std::unique_ptr<ControlProxy> m_pTrackLoaded;

    QThread m_thread;
    /// Lives on m_thread; deleted once it has stopped.
    std::unique_ptr<HistoryStore> m_pStore;
    bool m_stored = false;
    const QString m_directory;
    const QString m_artworkDirectory;
    /// Which power-up this is.
    const QByteArray m_bootId;
    /// Looks for the store's directory until it is there; see the constructor.
    QTimer m_reopenTimer;

    QList<HistorySession> m_sessions;
    /// The rows of the power-ups before this one, the last first.
    QList<QList<history::Row>> m_pastRows;
    history::Recorder m_recorder;
    /// The plays open now, by player: their ids in the session.
    QHash<int, int> m_openPlays;
    /// When a player whose status went stale was last heard, on the boot's
    /// clock: where its track stops.
    QHash<int, qint64> m_lastHeard;
    int m_nextTrackId = 1;
    int m_nextPlayId = 1;
    /// The session's tracks by key.
    QHash<QString, int> m_trackIds;
    /// Where each key seen this run came from, for naming it later.
    QHash<QString, Origin> m_origins;
    /// Tracks of this power-up with no name yet, by id.
    QSet<int> m_unnamed;
    /// This run of Mixxx, in keys: a slot's stick numbering starts again with
    /// every run, and two runs within one power-up must not share a key.
    const QString m_epoch;
    struct Generation {
        QString fingerprint;
        int number = 0;
    };
    QHash<QString, Generation> m_generations;
    /// Covers asked of the network for thumbnails, and the names they go to.
    QHash<QString, QStringList> m_coversWaited;

    QTimer m_sampleTimer;
    QTimer m_heartbeatTimer;
};

} // namespace deck
} // namespace mixxx
