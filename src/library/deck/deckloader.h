#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

#include "library/deck/mediumid.h"
#include "track/track_decl.h"

class ControlProxy;
class CoverInfoRelative;
class Library;
class QSqlDatabase;

namespace mixxx {
namespace deck {

class MediaRegistry;
class RemoteTrackStreamer;
class TrackCache;

/// Puts a track on the deck: the DJ's loads from the browser and autoplay's
/// alike, by one path.
///
/// The track is played from a copy, filled in from its medium's database
/// (metadata, key, cover, and rekordbox's grid, cues and waveform), announced
/// to the network and handed to the deck. Around a load it keeps what belongs
/// to the track on the deck: its copy pinned in the cache, its cover defended,
/// a folder track's BPM written back once Mixxx finds it, and the play log.
class DeckLoader : public QObject {
    Q_OBJECT

  public:
    /// Everything a load needs off a track's row, read in one go: see
    /// loadRow() for why it has to be.
    struct LoadableRow {
        QString source;
        QString analyzePath;
        int sampleRate = 0;
        quint32 rekordboxId = 0;
        QString artist;
        QString title;
        QString album;
        QString key;
        QString coverPath;
        QString artworkPath;
        int trackRowId = -1;
        MediumId medium;
    };

    /// *group* is the deck loaded into. The rest are used and not owned.
    DeckLoader(const QString& group,
            Library* pLibrary,
            MediaRegistry* pRegistry,
            RemoteTrackStreamer* pStreamer,
            TrackCache* pCache,
            QObject* pParent = nullptr);
    ~DeckLoader() override;

    /// Put *row* on the deck. *trackFromList* gives the row's Track as the
    /// browser's list has it, when the load came from the list: that is where
    /// a track not copied off its medium is taken from. False when nothing was
    /// handed to the deck -- Mixxx would not make a Track of the file -- so
    /// that neither a `track_loaded` nor a failed load will ever come of it.
    bool loadRow(const LoadableRow& row,
            bool play,
            const std::function<TrackPointer()>& trackFromList = {});

    /// A `deck_library` row by its id, wherever the browser is, and playing.
    /// False when the row has gone.
    bool loadLibraryRow(int rowId);

    /// The `deck_library` row on the deck, or -1. Remembered at load rather
    /// than matched back from the Track: two media can hold clones of the
    /// same file, so the path does not identify the row and the medium would
    /// have to be guessed.
    int loadedRowId() const {
        return m_loadedTrackRowId;
    }

    /// The deck has nothing on it any more -- ejected, or a track that would
    /// not load: forget the row it had.
    void onDeckEmpty();

  signals:
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);
    /// A row of a medium changed in place: a folder track's BPM, found by
    /// Mixxx's analysis and written back. *rbIds* are the rows, by their id
    /// within the medium, as MediaRegistry::mediumUpdated() says them.
    void rowsUpdated(const QString& mediumKey, const QList<quint32>& rbIds);

  private:
    bool readLibraryRow(int rowId, LoadableRow* pRow);
    /// Unpin the track that was on the deck and let go of it: its stream, and
    /// its copy if that is still coming off a stick.
    void releasePinned();
    /// Point a Track at the cover its medium carries, so the deck's header
    /// draws it.
    ///
    /// Nothing else does: a rekordbox medium keeps its art under `PIONEER/`,
    /// nowhere near the audio file, and the deck plays from a byte copy in the
    /// cache anyway -- so every way Mixxx has of finding a cover on its own
    /// (embedded tags, an image beside the file) comes back with nothing and
    /// the header falls back to the empty square. The path is in the pdb, and
    /// the browser row already has it.
    ///
    /// Taken by value because the caller may be handing over the pending-cover
    /// members, which this clears.
    void applyCoverArt(const TrackPointer& pTrack,
            QString coverPath,
            QString artworkPath);
    /// Put the medium's cover back if something guesses over it.
    ///
    /// `TrackDAO::getOrAddTrack()` fires `guessTrackCoverInfoConcurrently()` on
    /// a worker for every track it adds to the library for the first time. That
    /// worker looks for an image beside the audio file — which for this deck is
    /// the byte-copy cache, and holds nothing but other tracks — finishes a few
    /// milliseconds after the load has returned, and writes `CoverInfo::NONE`
    /// over the path taken out of the pdb.
    ///
    /// It is a race and it cannot be won by ordering: the guess is already
    /// running when `getOrAddTrack()` returns. So the cover is *defended*
    /// instead, once, and the guess loses the rematch.
    ///
    /// This is the whole of "the artwork appears on the second load and never
    /// the first": the second load finds the track already in the library, so
    /// no guess is fired and nothing overwrites anything.
    void guardCoverArt(const TrackPointer& pTrack, const CoverInfoRelative& cover);
    void writeBackBpm(int trackRowId, quint32 rekordboxId, const QString& mediumKey, double bpm);
    void logPlay();
    /// The deck got far enough into the loaded track to call it played.
    void onPlayPositionChanged(double position);
    /// Start watching the deck's play position for the play log, once.
    void watchPlayPosition();
    QSqlDatabase database() const;

    const QString m_group;
    Library* const m_pLibrary;
    MediaRegistry* const m_pRegistry;
    RemoteTrackStreamer* const m_pStreamer;
    TrackCache* const m_pCache;

    /// The deck_library row currently on the deck, and whether it has already
    /// been logged.
    int m_loadedTrackRowId = -1;
    bool m_loadedTrackLogged = false;
    std::unique_ptr<ControlProxy> m_pPlayPosition;
    /// The cached file the deck is playing, so it can be unpinned when another
    /// takes its place.
    QString m_pinnedPath;
    /// A cover the track on the deck is still waiting for, and the track that
    /// wants it. Only ever set for a remote medium: its images come over the
    /// network one at a time, as they are looked at, so a track can reach the
    /// deck before its own cover does.
    ///
    /// **Weak on purpose.** The deck owns what it is playing; holding a
    /// TrackPointer here would keep the last one alive in GlobalTrackCache
    /// until the next load, waveform and all, for a cover that may never come.
    QString m_pendingCoverPath;
    QString m_pendingArtworkPath;
    TrackWeakPointer m_pendingCoverTrack;
    /// Armed by guardCoverArt() for the track on the deck, and only that one.
    QMetaObject::Connection m_coverGuard;
    /// Writes the BPM Mixxx's analysis finds back to the row of the folder
    /// track on the deck, so the list shows it and the BPM menu can bucket it.
    /// Re-armed on every load; see loadRow().
    QMetaObject::Connection m_bpmWriteBack;
};

} // namespace deck
} // namespace mixxx
