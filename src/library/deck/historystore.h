#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSqlDatabase>
#include <QString>

#include "library/deck/playhistory.h"

namespace mixxx {
namespace deck {

/// One track of a session, as the History keeps it.
///
/// Everything the list draws, copied out of wherever the track came from when
/// it played: by the time anyone looks, the stick may be in someone else's
/// bag.
struct HistoryTrack {
    /// The session's own number for it, which a Play's `track` names.
    int id = 0;
    /// How DeckHistory told it from other tracks (see there). Never shown.
    QString key;
    /// Empty when nobody could say: a CDJ's plain file, a deck that announced
    /// none. The list says so rather than invent one.
    QString title;
    QString artist;
    /// What the medium calls itself: the stick's name.
    QString sourceName;
    /// The player whose medium it is, on the link; 0 for this deck's own
    /// stick while it was on no link.
    int sourcePlayer = 0;
    /// `usb` or `sd`, or empty.
    QString sourceSlot;
    /// The thumbnail's file name in the store's `art/`, or empty.
    QString artwork;
};

/// One play as the store keeps it: a history::Play, and its row.
struct HistoryPlay {
    int id = 0;
    history::Play play;
};

/// One power-up's history.
struct HistorySession {
    int id = 0;
    /// Which boot it was: `/proc/sys/kernel/random/boot_id`.
    QByteArray bootId;
    /// The wall clock when it was opened, for a person reading the file. The
    /// deck never shows it (Sam: "no date, we're not date synced"): a Pi has
    /// no clock that keeps time, and a gig has no NTP.
    qint64 openedWallMs = 0;
    QList<HistoryTrack> tracks;
    QList<HistoryPlay> plays;

    const HistoryTrack* track(int id) const;
    /// Every play, as history::segmentsOf() takes them. *nowMs* is the end of
    /// the plays still open, for this session; -1 for one that has ended.
    QList<history::Play> playsUntil(const QList<int>& openPlayIds, qint64 nowMs) const;
};

/// Everything one sample, heartbeat or answer changes: written as one
/// transaction, so a cut leaves all of it or none.
struct HistoryWrite {
    QList<HistoryTrack> newTracks;
    /// Tracks whose names, source or artwork are known now.
    QList<HistoryTrack> changedTracks;
    /// Started: written with their end at their start.
    QList<HistoryPlay> newPlays;
    /// Play ids and where they now end: a heartbeat, or a stop.
    QList<QPair<int, qint64>> ends;

    bool isEmpty() const {
        return newTracks.isEmpty() && changedTracks.isEmpty() && newPlays.isEmpty() &&
                ends.isEmpty();
    }
};

/// The History on the card: a database of its own beside its thumbnails, in a
/// directory the deck keeps across boots and updates.
///
/// Apart from Mixxx's database on purpose. Mixxx's lives with its settings,
/// which on a release card are RAM; and what Mixxx keeps there is about its own
/// library, which the deck's sticks are not.
///
/// **What survives a cut.** SQLite in WAL mode with `synchronous=FULL`: each
/// write() is one transaction, on the card when it returns. A cut in the
/// middle of one leaves the one before, which the next open finds. A database
/// that will not open at all is put aside and a new one started, so a broken
/// card costs the History and never the deck.
///
/// Lives on a thread of its own (DeckHistory's), because a commit waits for
/// the card, and an SD card can take a long time to say it has written.
class HistoryStore : public QObject {
    Q_OBJECT

  public:
    /// How many power-ups are kept: this one, the last, two ago.
    static constexpr int kSessionsKept = 3;
    /// The thumbnails' largest side, in pixels: twice the list's cover, which
    /// is enough for any screen of the deck's and keeps each to a few kB.
    static constexpr int kArtworkSize = 160;

    explicit HistoryStore(QObject* pParent = nullptr);
    ~HistoryStore() override;

    struct Opened {
        /// Whether the store is on the card. False: the History is this
        /// power-up's, in RAM, and gone at the next.
        bool ok = false;
        QString error;
        /// Newest first: this power-up's, then the last, then two ago.
        QList<HistorySession> sessions;
    };

    /// Open the store in *directory* for the boot *bootId*, and rotate: a boot
    /// it has not seen starts a session, and the oldest beyond kSessionsKept go
    /// with their thumbnails. The boot it has seen -- Mixxx started again within
    /// one power-up -- carries on its session.
    ///
    /// A power-up the History lists nothing for is no session to keep: the
    /// next boot takes its place rather than the oldest's, so a deck switched
    /// on and off twice after a gig still has the gig.
    Opened open(const QString& directory, const QByteArray& bootId, qint64 nowWallMs);

    /// Write *write* to the session *sessionId*. Nothing without an open store.
    void write(int sessionId, const HistoryWrite& write);

    /// Make a thumbnail of the image at *imagePath*, as `art/<name>`. Says
    /// artworkSaved() when it is there; nothing when the image will not load.
    void saveArtwork(const QString& imagePath, const QString& name);

    /// Close the database. Everything written is on the card already.
    void close();

    /// Where a thumbnail of this store is, by its name.
    QString artworkPath(const QString& name) const;

  signals:
    void artworkSaved(const QString& name);

  private:
    bool openDatabase(const QString& path, QString* pError);
    bool createSchema();
    /// The session of *bootId*, made if new, and the old ones dropped.
    bool rotate(const QByteArray& bootId, qint64 nowWallMs);
    QList<HistorySession> readSessions();
    /// Thumbnails no track names any more.
    void dropUnusedArtwork();

    QString m_directory;
    QString m_connectionName;
    QSqlDatabase m_database;
};

} // namespace deck
} // namespace mixxx
