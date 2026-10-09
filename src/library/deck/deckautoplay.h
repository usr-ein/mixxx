#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

#include "library/deck/autoplay.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/mediumid.h"

class ControlObject;
class ControlProxy;

namespace mixxx {
namespace deck {

class DeckLoader;

/// Autoplay: one genre of one drive, track after track, on the deck by itself.
///
/// The DJ starts it from the browser on a track of their choosing (Autoplay, a
/// drive, a genre, a track). From then on, each time the deck reaches the end
/// of a track, the next one -- the nearest in BPM among the genre's tracks not
/// yet played this round, by the rule in autoplay.h -- loads on the same deck
/// and starts at once. Back to back: no second deck, no crossfade, nothing
/// copied ahead, and a gap of whatever a load takes (Sam, 2026-10-09).
///
/// **What it does not touch** is everything a DJ does to the track playing.
/// Pause, and it waits; loop, and the track never reaches its end, so nothing
/// happens until it does; the tempo fader rules the next track exactly as it
/// rules a track loaded by hand.
///
/// It ends when the DJ stops it, loads a track by hand or ejects; when its drive
/// goes away, whose track then plays out from the cache; and after three tracks
/// in a row that would not load. What has played is remembered per drive and
/// genre until the deck restarts, in RAM only.
class DeckAutoplay : public QObject {
    Q_OBJECT

  public:
    /// *deckGroup* is the deck it plays on. *pLoader* puts its picks there,
    /// and starts them (DeckLoader::loadLibraryRow()). Used and not owned,
    /// like *pRegistry*.
    DeckAutoplay(const QString& deckGroup,
            MediaRegistry* pRegistry,
            std::function<QSqlDatabase()> database,
            DeckLoader* pLoader,
            QObject* pParent = nullptr);
    ~DeckAutoplay() override;

    bool isOn() const {
        return m_state != State::Off;
    }
    const MediumId& medium() const {
        return m_medium;
    }
    /// As the genre list shows it: "—" for the tracks with none.
    QString genreTitle() const;
    const QString& mediumName() const {
        return m_mediumName;
    }

    /// Start on *rowId*, the track the DJ picked from *genre* on *medium*. It
    /// counts as played. Starting again while on starts over on the new pick.
    void start(const MediumId& medium,
            const QString& mediumName,
            const QString& genre,
            int rowId);
    /// Turn it off, saying *notice* in a toast unless it is empty. The track
    /// playing goes on playing; nothing follows it.
    void stop(const QString& notice = QString());

    /// The DJ is loading a track by hand, which ends autoplay.
    void onManualLoad();
    /// The deck has nothing on it any more: an eject, or a load that failed.
    void onDeckEmpty();

  signals:
    /// On or off, or on with another genre.
    void stateChanged();
    /// Something worth a toast.
    void notice(const QString& text);

  private:
    enum class State {
        Off,
        /// A track has been asked for and the deck has not taken it yet.
        Loading,
        /// The deck has autoplay's track; its end is what comes next.
        Playing,
    };

    void onPlayPosition(double position);
    void onTrackLoaded(double loaded);
    void onMediumVanished(const mixxx::deck::MediumInfo& medium);
    /// The track has ended: pick the next and load it.
    void advance();
    void onLoadFailed();
    /// Where the drive's files sit under, which a track's path on it is taken
    /// relative to (autoplay::trackKey()).
    QString rootOf(const MediumId& medium) const;

    MediaRegistry* m_pRegistry;
    std::function<QSqlDatabase()> m_database;
    DeckLoader* m_pLoader;
    autoplay::PlayedMemory m_memory;

    State m_state = State::Off;
    MediumId m_medium;
    QString m_mediumName;
    QString m_genre;
    QString m_scope;
    int m_round = 1;

    /// The track on the deck that autoplay put there, and its tempo when it
    /// was picked: the reference for the next pick if the library no longer
    /// has it.
    QString m_currentKey;
    double m_currentBpm = 0.0;
    /// The one asked for and not on the deck yet.
    QString m_pendingKey;
    QString m_pendingTitle;
    double m_pendingBpm = 0.0;
    /// The pending track is the one the DJ picked, not one autoplay did.
    bool m_pendingIsStart = false;

    /// Tracks in a row that would not load.
    int m_failures = 0;
    /// The next pick, a second after a track that would not load.
    QTimer m_retry;
    /// From the end of a track, for the gap in the log.
    QElapsedTimer m_sinceEnd;

    std::unique_ptr<ControlProxy> m_pPlayPosition;
    std::unique_ptr<ControlProxy> m_pTrackLoaded;
    /// `[Browser],autoplay_picked`: 1 while the track on the deck, or on its
    /// way, is one autoplay picked by itself. The mapping leaves the screen as
    /// it is for those -- the DJ may be browsing -- and returns to the deck for
    /// every other load (TriMixxx.scripts.js).
    std::unique_ptr<ControlObject> m_pPicked;
};

} // namespace deck
} // namespace mixxx
