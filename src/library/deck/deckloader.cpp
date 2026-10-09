#include "library/deck/deckloader.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "audio/types.h"
#include "control/controlproxy.h"
#include "library/coverart.h"
#include "library/dao/analysisdao.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/pdbingest.h"
#include "library/deck/remotetrackstreamer.h"
#include "library/deck/trackcache.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxanalysis.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_deckloader.cpp"
#include "track/track.h"
#include "track/trackref.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("DeckLoader");
} // namespace

namespace mixxx {
namespace deck {

DeckLoader::DeckLoader(const QString& group,
        Library* pLibrary,
        MediaRegistry* pRegistry,
        RemoteTrackStreamer* pStreamer,
        TrackCache* pCache,
        QObject* pParent)
        : QObject(pParent),
          m_group(group),
          m_pLibrary(pLibrary),
          m_pRegistry(pRegistry),
          m_pStreamer(pStreamer),
          m_pCache(pCache) {
    connect(m_pRegistry,
            &MediaRegistry::artworkArrived,
            this,
            [this](const QString& coverPath) {
                // The deck's header, when the track on it was the one waiting.
                // The browser redraws its lists on the same signal; the deck
                // draws from the Track, and the Track has no cover yet.
                if (!m_pendingCoverPath.isEmpty() && coverPath == m_pendingCoverPath) {
                    applyCoverArt(m_pendingCoverTrack.lock(),
                            m_pendingCoverPath,
                            m_pendingArtworkPath);
                }
            });
}

DeckLoader::~DeckLoader() = default;

/// Put a track on the deck.
///
/// Five steps, in this order, and the order is the whole design:
///
///  1. **Read the row.** Every field is taken before this is called, by
///     readModelRow() or readLibraryRow(), because anything below can run a
///     nested event loop, and during one the model can be re-sorted or reset
///     underneath us.
///  2. **Get a local file.** A stick's track is *started* copying and a remote
///     one *started* downloading; either comes back as a file of the right
///     size whose unwritten parts block a reader instead of handing it zeros.
///     Either way the deck plays a copy, never the medium -- that is what lets
///     a stick be pulled mid-track.
///  3. **Make the Track.**
///  4. **Fill it in from the pdb**, never from the file: the metadata, and the
///     rekordbox grid, cues and waveform from the ANLZ files beside it.
///  5. **Hand it to the deck.**
///
/// Three rules keep this honest, and each of them has been broken at least
/// once, silently:
///
///  * **Nothing here may wait for bytes.** This is the GUI thread, and the GUI
///     thread is what announces bytes as they arrive -- so a wait here is a
///     deadlock, not a delay. StreamingFile refuses rather than trusting this
///     comment.
///  * **Nothing here may read the audio file.** Not for tags, not for a sample
///     rate, not for a duration. It may not have arrived, and every one of
///     those is in the pdb.
///  * **A file that is still arriving is only ever read through
///     StreamingFile.** An ordinary read of a sparse hole returns zeros and
///     succeeds, so the failure mode is not an error, it is silence.
bool DeckLoader::loadRow(const LoadableRow& row,
        bool play,
        const std::function<TrackPointer()>& trackFromList) {
    const QString& source = row.source;
    const QString& analyzePath = row.analyzePath;
    const int sampleRate = row.sampleRate;
    const quint32 rekordboxId = row.rekordboxId;
    const QString& title = row.title;
    const QString& key = row.key;
    const int trackRowId = row.trackRowId;
    const MediumId& medium = row.medium;

    // Play the COPY, never the medium. This is the whole point of the cache:
    // the file under the playhead has to be one that survives the stick being
    // pulled out of the deck.
    QString playPath = source;
    if (m_pCache && !source.isEmpty()) {
        QString local;
        if (medium.isLocal()) {
            // Played while it copies too, like a remote track: the copy is
            // started (or found already done) and never waited for here.
            // Waiting for it froze the deck for as long as the stick took to
            // give up the whole file -- eighteen seconds for an AIFF off a
            // slow stick.
            local = m_pCache->startLocal(medium, source);
        } else if (m_pStreamer) {
            // A remote track is not copied and then played -- it is played
            // while it copies. What comes back is a file of the right size
            // whose unwritten parts block a reader instead of handing it the
            // zeros a sparse file would.
            local = m_pStreamer->startStreaming(medium, source, analyzePath);
        }
        if (!local.isEmpty()) {
            playPath = local;
            // Guarded on the paths differing: the same track loaded twice comes
            // back as the stream already in flight, and letting go of it there
            // would abandon the one about to be played.
            if (!m_pinnedPath.isEmpty() && m_pinnedPath != local) {
                releasePinned();
            }
            // Immediately, and with no event loop between this and the adopt
            // inside startStreaming(): an eviction sweep in that gap could drop
            // the very file about to be played.
            m_pCache->pin(local);
            m_pinnedPath = local;
        } else {
            // A read error, or a player that went away mid-transfer. Falling
            // through to the original path keeps a local stick playable; a
            // remote one has nothing to fall through to and will not load.
            kLogger.warning() << "not cached, playing from source:" << source;
            // The track on the deck is not the pinned one any more. Left
            // pinned, the last track's copy made an eject say this one was
            // cached and would keep playing, while it was reading the stick.
            releasePinned();
        }
    }

    // Off the list's model when it is the medium's own file and there is a
    // list, which fills a track new to the library in from the row.
    const bool fromModel = playPath == source && trackFromList;
    TrackPointer pTrack = fromModel
            ? trackFromList()
            : m_pLibrary->trackCollectionManager()->getOrAddTrack(
                      TrackRef::fromFilePath(playPath));
    if (!pTrack) {
        kLogger.warning() << "no track for" << source;
        return false;
    }
    if (!fromModel) {
        // The cached file has no tags worth reading -- it is a byte copy of
        // somebody else's file -- so the metadata comes from the pdb, as it
        // does for the row itself.
        pTrack->setArtist(row.artist);
        pTrack->setTitle(title);
        pTrack->setAlbum(row.album);
        // Genre is deliberately not set: Track has no plain setter for it (only
        // setGenreFromTrackDAO, which is the DAO's business), and the browser
        // reads genre off the pdb row anyway.
    }

    // The key, and unconditional for the same reason as the cover below.
    //
    // **Nothing else supplies it.** Key detection is off on the deck (it costs
    // minutes per track and rekordbox has already done it), and nothing on this
    // path reads the file's tags -- deliberately, because the file may still be
    // arriving. So without this line every rekordbox track loads with no key at
    // all: the header shows nothing and KEY SYNC has nothing to shift.
    if (!key.isEmpty()) {
        pTrack->setKeyText(key, mixxx::track::io::key::FILE_METADATA);
    }

    // The cover, from the pdb for the same reason as the metadata above: the
    // art is not in the file and not beside it. Unconditional, unlike the
    // metadata -- a stick's tracks are played from a copy *and* read through
    // getTrack(), and neither route finds a cover on its own.
    applyCoverArt(pTrack, row.coverPath, row.artworkPath);

    // The beat grid, hot cues, loops and memory cues rekordbox already worked
    // out, from the ANLZ files beside the track.
    //
    // Without this the deck loads a track with no grid and Mixxx analyses it
    // from scratch -- minutes of CPU for something the medium was carrying all
    // along, and a grid that disagrees with what every CDJ on the network is
    // showing. On a stick the files are simply there; for a remote medium
    // startStreaming() fetched them above, before the audio, and that ordering
    // is deliberate.
    if (!analyzePath.isEmpty()) {
        // The pdb's sample rate, not the decoder's, and that is load-bearing.
        //
        // Everything rekordbox stores is in milliseconds and has to become
        // frames, so a rate is needed before the grid, the cues or the waveform
        // can be applied -- and for a streamed track nothing has decoded a byte
        // yet. Asking the decoder would mean waiting on the first 64 KB **here,
        // on the GUI thread**, which is the one thread that runs the poll that
        // announces bytes as they land. That is not a delay, it is a deadlock,
        // and it fails fifteen seconds later as a read timeout.
        //
        // Until recently this was free: the tag scan set the sample rate on its
        // way past. Taking that scan out to make loads fast quietly took the
        // beat grid, the hot cues and the waveform with it, and the only
        // visible symptom was Mixxx analysing every track from scratch.
        mixxx::rekordbox::applyAnalysis(pTrack,
                analyzePath,
                &m_pLibrary->trackCollectionManager()
                         ->internalCollection()
                         ->getAnalysisDAO(),
                mixxx::audio::SampleRate(sampleRate));
    }

    m_loadedTrackRowId = trackRowId > 0 ? trackRowId : -1;
    m_loadedTrackLogged = false;
    watchPlayPosition();

    // A folder track comes with no tempo unless its tags had one, and Mixxx's
    // analysis is about to find it. Written back to the row when it does, so
    // the list shows it and the BPM menu can put it in a bucket -- the one
    // thing this deck learns about a stick that is worth keeping on screen.
    QObject::disconnect(m_bpmWriteBack);
    if (trackRowId > 0 && m_pRegistry->formatOf(medium) == MediumInfo::Format::Folder) {
        // Already known if the deck analysed this file earlier in the boot:
        // the beats come back with the track, and nothing changes after the
        // load to say so.
        writeBackBpm(trackRowId, rekordboxId, medium.key(), pTrack->getBpm());
        m_bpmWriteBack = connect(pTrack.get(),
                &Track::bpmChanged,
                this,
                [this,
                        trackRowId,
                        rekordboxId,
                        mediumKey = medium.key(),
                        weakTrack = TrackWeakPointer(pTrack)]() {
                    const TrackPointer pLoaded = weakTrack.lock();
                    if (pLoaded) {
                        writeBackBpm(trackRowId, rekordboxId, mediumKey, pLoaded->getBpm());
                    }
                });
    }
    // What the deck is being handed, in the terms the deck draws from. A track
    // with beats but no waveform renders as a bare grid, which looks like a
    // rendering fault rather than a missing import.
    // What the network is told we are playing. A tempo with no track beside
    // it is a state no deck publishes, and one that no CDJ will follow.
    if (m_pRegistry) {
        m_pRegistry->announceLoadedTrack(medium, rekordboxId);
    }
    kLogger.debug() << "loading" << title << "-- beats" << (pTrack->getBeats() != nullptr)
                    << "waveform" << !pTrack->getWaveform().isNull()
                    << "summary" << !pTrack->getWaveformSummary().isNull();
    emit loadTrackToPlayer(pTrack, m_group, play);
    return true;
}

bool DeckLoader::loadLibraryRow(int rowId) {
    LoadableRow loadable;
    if (!readLibraryRow(rowId, &loadable)) {
        return false;
    }
    return loadRow(loadable, true);
}

bool DeckLoader::readLibraryRow(int rowId, LoadableRow* pRow) {
    // The same fields readModelRow() takes off the list, straight from the
    // table: autoplay's next track is in no list on screen -- the DJ may be
    // anywhere in the browser, or not in it at all.
    QSqlDatabase db = database();
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT location, analyze_path, samplerate, rb_id, artist, title, album, "
            "key, coverart_location, artwork_path, medium FROM %1 WHERE id = :id")
                          .arg(kLibraryTable));
    query.bindValue(QStringLiteral(":id"), rowId);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    if (!query.next()) {
        return false;
    }
    pRow->source = query.value(0).toString();
    pRow->analyzePath = query.value(1).toString();
    pRow->sampleRate = query.value(2).toInt();
    pRow->rekordboxId = static_cast<quint32>(query.value(3).toUInt());
    pRow->artist = query.value(4).toString();
    pRow->title = query.value(5).toString();
    pRow->album = query.value(6).toString();
    pRow->key = query.value(7).toString();
    pRow->coverPath = query.value(8).toString();
    pRow->artworkPath = query.value(9).toString();
    pRow->trackRowId = rowId;
    pRow->medium = MediumId::fromKey(query.value(10).toString());
    return true;
}

void DeckLoader::releasePinned() {
    if (m_pinnedPath.isEmpty() || !m_pCache) {
        return;
    }
    m_pCache->unpin(m_pinnedPath);
    // The outgoing track: nothing is reading it any more, so wake anything
    // that still is and let go of it -- and of its copy, if that is still
    // coming off a stick.
    m_pCache->release(m_pinnedPath);
    if (m_pStreamer) {
        m_pStreamer->stopStreaming(m_pinnedPath);
    }
    m_pinnedPath.clear();
}

void DeckLoader::applyCoverArt(const TrackPointer& pTrack,
        QString coverPath,
        QString artworkPath) {
    // Whatever the last track was waiting for, it is not waiting any more, and
    // its cover is not this widget's to defend any more either.
    m_pendingCoverPath.clear();
    m_pendingArtworkPath.clear();
    m_pendingCoverTrack.reset();
    QObject::disconnect(m_coverGuard);
    if (!pTrack || coverPath.isEmpty()) {
        return;
    }

    if (!QFileInfo::exists(coverPath)) {
        // A remote medium's covers come over dbserver one at a time, and only
        // the ones a DJ has looked at -- so a track loaded from a list that
        // scrolled past quickly can reach the deck before its image does.
        //
        // The CoverInfo is *not* set here, and that is the whole reason this
        // waits rather than setting it now and again on arrival: Track only
        // emits coverArtUpdated when the info actually changes, so the second
        // set would be a no-op and the header would keep the empty square it
        // drew for a file that was not there. It is set exactly once, when
        // there is something to load.
        if (m_pRegistry) {
            m_pRegistry->requestArtwork(coverPath);
        }
        m_pendingCoverPath = std::move(coverPath);
        m_pendingArtworkPath = std::move(artworkPath);
        m_pendingCoverTrack = pTrack;
        return;
    }

    CoverInfoRelative cover;
    cover.type = CoverInfo::FILE;
    cover.source = CoverInfo::GUESSED;
    cover.coverLocation = coverPath;
    // The same key the browser's rows are drawn with. Skipping it would leave
    // every rekordbox track sharing CoverInfo's single default key, and
    // CoverArtCache indexes its pixmaps by that -- so the header would show
    // whichever cover was decoded last until the load of the real one caught
    // up and corrected it.
    cover.setImageDigest(artworkDigest(artworkPath));
    pTrack->setCoverInfo(cover);
    guardCoverArt(pTrack, cover);
}

void DeckLoader::guardCoverArt(const TrackPointer& pTrack, const CoverInfoRelative& cover) {
    QObject::disconnect(m_coverGuard);
    m_coverGuard = connect(pTrack.get(),
            &Track::coverArtUpdated,
            this,
            [this, weakTrack = TrackWeakPointer(pTrack), cover]() {
                const TrackPointer pLoaded = weakTrack.lock();
                if (!pLoaded || pLoaded->getCoverInfo() == cover) {
                    // Still ours, or gone. Either way there is nothing here to
                    // put right -- and this fires for our own set as well as
                    // for the one that overwrote it.
                    return;
                }
                // Disconnected BEFORE setting, not after: setCoverInfo() emits
                // this signal synchronously, so a guard still armed would be
                // straight back in here looking at its own work.
                QObject::disconnect(m_coverGuard);
                pLoaded->setCoverInfo(cover);
            });
}

void DeckLoader::onDeckEmpty() {
    // Set at load, before the deck has said yes. A file that would not decode
    // left the marker on its row, claiming a track the deck does not have.
    m_loadedTrackRowId = -1;
    m_loadedTrackLogged = false;
}

void DeckLoader::writeBackBpm(int trackRowId,
        quint32 rekordboxId,
        const QString& mediumKey,
        double bpm) {
    if (bpm <= 0.0) {
        return;
    }
    QSqlDatabase db = database();
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE %1 SET bpm = :bpm WHERE id = :id").arg(kLibraryTable));
    query.bindValue(QStringLiteral(":bpm"), bpm);
    query.bindValue(QStringLiteral(":id"), trackRowId);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }
    emit rowsUpdated(mediumKey, {rekordboxId});
}

void DeckLoader::logPlay() {
    if (m_loadedTrackRowId < 0 || m_loadedTrackLogged) {
        return;
    }
    m_loadedTrackLogged = true;

    QSqlDatabase db = database();
    if (!db.isOpen()) {
        return;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT INTO deck_play_log (track_id, played_at) "
            "VALUES (:track_id, :played_at)"));
    query.bindValue(QStringLiteral(":track_id"), m_loadedTrackRowId);
    // Seconds since the epoch. Only ever compared against other rows in this
    // same table, and the table does not survive a boot, so the absolute value
    // never has to mean anything.
    query.bindValue(QStringLiteral(":played_at"),
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

void DeckLoader::onPlayPositionChanged(double position) {
    // Half way through is "played". Mixxx's own threshold for its play counter
    // is configurable and this is not it: what Last played answers is "did I
    // already drop this tonight", and a track a DJ pulled out after eight bars
    // is one they did not.
    if (position >= 0.5) {
        logPlay();
    }
}

void DeckLoader::watchPlayPosition() {
    if (m_pPlayPosition) {
        return;
    }
    // Our half of "Last played": what THIS deck played, which is the half that
    // matters during a set, since the stick's own history is somebody else's
    // and stops at the last time a real CDJ mounted it.
    //
    // Watched from the first load, as there is nothing to log before one.
    // That also keeps it after autoplay's watch on the same control, made
    // with autoplay, as the browser's was: at the end of a track, autoplay
    // hears the position first and loads the next.
    m_pPlayPosition = std::make_unique<ControlProxy>(
            m_group, QStringLiteral("playposition"), this);
    m_pPlayPosition->connectValueChanged(this, &DeckLoader::onPlayPositionChanged);
}

QSqlDatabase DeckLoader::database() const {
    return m_pLibrary->trackCollectionManager()->internalCollection()->database();
}

} // namespace deck
} // namespace mixxx
