#include "library/deck/remotetrackstreamer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include "library/deck/mediaregistry.h"
#include "library/deck/streamingfile.h"
#include "library/deck/trackcache.h"
#include "moc_remotetrackstreamer.cpp"
#include "network/prolink/prolinknetworkservice.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("RemoteTrackStreamer");

/// How much of a track to pull before anything else.
///
/// Runway, measured in seconds of playback rather than in bytes: 1 MB is about
/// twenty-five seconds of a 320 kbps MP3, against a link that delivers roughly
/// thirty seconds of audio per second. So the download is never the thing that
/// runs out — the head is there for the case where the network stalls, and
/// twenty-five seconds is long enough to notice and do something about it.
///
/// Deliberately not larger. The **tail** is fetched second and an M4A cannot be
/// opened without it, so every byte of head is delay before the first sound of
/// an AAC track. Doubling this would buy runway that is never used and pay for
/// it on every single load.
constexpr quint32 kStreamHeadBytes = 1024 * 1024;

/// How long to wait for a player to say how big a file is.
///
/// This covers a connect, a mount, a lookup and a stat — and, if another
/// transfer is already running, the wait for it, because the library serialises
/// them onto one player. Generous for that reason, and a wait anywhere near it
/// is a fault worth seeing in the log rather than a slow network.
constexpr int kStreamStartTimeoutMs = 20000;

/// How long to wait for a beat grid. Tens of kilobytes, so this is only ever
/// hit when something is wrong.
constexpr int kCompanionTimeoutMs = 10000;
} // namespace

namespace mixxx {
namespace deck {

RemoteTrackStreamer::RemoteTrackStreamer(MediaRegistry* pRegistry,
        mixxx::prolink::ProLinkNetworkService* pNetwork,
        TrackCache* pCache,
        QObject* pParent)
        : QObject(pParent),
          m_pRegistry(pRegistry),
          m_pNetwork(pNetwork),
          m_pCache(pCache) {
    // Connected once, for the life of the streamer, rather than per transfer.
    // A streamed file's ranges must be recorded even while nothing is waiting
    // on them -- the deck is decoding out of it the whole time -- so there is
    // no window in which these can be off.
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::fileFetchProgress,
            this,
            &RemoteTrackStreamer::onFetchProgress);
    connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            this,
            &RemoteTrackStreamer::onFetchFinished);
}

RemoteTrackStreamer::~RemoteTrackStreamer() = default;

void RemoteTrackStreamer::fetchCompanionBlocking(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const QString& remotePath,
        const QString& localPath) {
    if (remotePath.isEmpty() || localPath.isEmpty() || QFileInfo::exists(localPath)) {
        return;
    }

    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    bool finished = false;
    const auto connection = connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            &loop,
            [&loop, &finished, localPath](const QString& path, const QString&) {
                if (path == localPath) {
                    finished = true;
                    loop.quit();
                }
            });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, [&loop]() { loop.quit(); });
    timeout.start(kCompanionTimeoutMs);

    m_pNetwork->fetchFile(mac, slot, remotePath, localPath);
    // A fetch that cannot even start -- no session, or a player that has left --
    // reports it by emitting synchronously, from inside that call. quit() on a
    // loop that has not begun does nothing, so entering it anyway would wait out
    // the whole timeout for a failure already in hand.
    if (!finished) {
        // ExcludeUserInputEvents, like every nested loop on this path: the
        // caller has snapshotted its row and must not have the model reset
        // underneath it.
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    disconnect(connection);

    if (QFileInfo::exists(localPath)) {
        kLogger.debug() << "fetched" << remotePath << "in" << elapsed.elapsed() << "ms";
    } else {
        // Tolerated. A track with no beat grid still plays; it just arrives
        // unanalysed, and that is a far better outcome than refusing to load.
        kLogger.warning() << "no" << remotePath << "after" << elapsed.elapsed() << "ms";
    }
}

QString RemoteTrackStreamer::startStreaming(const MediumId& medium,
        const QString& sourcePath,
        const QString& analyzePath) {
    if (m_pCache == nullptr || sourcePath.isEmpty()) {
        return QString();
    }
    QByteArray mac;
    mixxx::prolink::MediaSlot slot = mixxx::prolink::MediaSlot::Usb;
    if (!MediaRegistry::addressOf(medium, &mac, &slot)) {
        return QString();
    }
    const QString localPath = m_pCache->localPathFor(medium, sourcePath);
    if (localPath.isEmpty()) {
        return QString();
    }

    // Already here in full, from an earlier load. Note that the filesystem
    // cannot answer this -- a half-streamed file is on disk at its full size
    // with zeros in the gaps, and it plays as silence rather than failing.
    if (m_streamComplete.contains(localPath) && QFileInfo::exists(localPath)) {
        kLogger.debug() << "already streamed in full:" << sourcePath;
        return localPath;
    }
    // Already arriving: the same track loaded twice, or loaded again while it
    // downloads. Handing back the stream in flight is right and restarting it
    // is not -- a restart would delete the file the deck is reading and leave
    // the finished transfer reporting completion for a stream nobody holds.
    if (m_streaming.contains(localPath) && StreamingFileRegistry::lookup(localPath)) {
        // It is wanted again, so it is no longer waiting to be swept up.
        m_removeWhenDone.remove(localPath);
        kLogger.debug() << "already streaming:" << sourcePath;
        return localPath;
    }
    // Arriving, but with nothing left holding it -- which cannot happen while
    // the stream stays registered for the life of its transfer, and is checked
    // for anyway because the alternative is two transfers writing one file.
    if (m_streaming.contains(localPath)) {
        kLogger.warning() << "a transfer is already running for" << sourcePath;
        return QString();
    }
    if (m_startingStream) {
        kLogger.warning() << "already starting a stream; refusing" << sourcePath;
        return QString();
    }
    // The medium has to still be there. Checked before anything is deleted
    // below, because a player that has left cannot be streamed from and the
    // local file may by then be the only copy of the track in existence --
    // spilled to tier 2 exactly because the medium went away.
    if (m_pRegistry->indexOf(medium) < 0) {
        kLogger.warning() << "that medium is no longer on the network:" << medium.key();
        return QString();
    }

    // The medium-relative path, which is what the player is asked for. The
    // location column is the local root and that path concatenated, so this is
    // the same string the pdb held.
    const QString root = MediaRegistry::remoteCacheRoot(medium);
    if (!sourcePath.startsWith(root)) {
        kLogger.warning() << "not under this medium's root:" << sourcePath;
        return QString();
    }
    const QString remotePath = sourcePath.mid(root.size());

    // The beat grid FIRST, before the audio.
    //
    // Not merely for tidiness: the library runs one transfer at a time against
    // a player, and a streaming fetch holds its turn for the whole download.
    // Anything queued behind it would wait for the entire track -- so a grid
    // asked for afterwards would arrive minutes late, long after the Track it
    // had to be applied to existed.
    if (!analyzePath.isEmpty() && analyzePath.startsWith(root)) {
        const QString datRemote = analyzePath.mid(root.size());
        fetchCompanionBlocking(mac, slot, datRemote, analyzePath);
        // Grids are only right in the legacy .DAT, cues only in the .EXT, so
        // both are wanted. Upper case deliberately: rekordbox writes them that
        // way and a player looks for exactly those names.
        if (datRemote.endsWith(QStringLiteral(".DAT"))) {
            fetchCompanionBlocking(mac,
                    slot,
                    datRemote.left(datRemote.size() - 3) + QStringLiteral("EXT"),
                    analyzePath.left(analyzePath.size() - 3) + QStringLiteral("EXT"));
        }
    }

    // A file left over from a stream that did not finish. Removed rather than
    // reused, because its holes are indistinguishable from silence.
    QFile::remove(localPath);
    StreamingFileRegistry::remove(localPath);

    m_startingStream = true;
    m_streaming.insert(localPath);

    QEventLoop loop;
    QString error;
    const auto ready = connect(this,
            &RemoteTrackStreamer::streamingReady,
            &loop,
            [&loop, localPath](const QString& path) {
                if (path == localPath) {
                    loop.quit();
                }
            });
    // A transfer that fails before it ever reports a size ends here instead.
    const auto failed = connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            &loop,
            [&loop, &error, localPath](const QString& path, const QString& reason) {
                if (path != localPath) {
                    return;
                }
                error = reason.isEmpty() ? tr("the transfer ended before it started")
                                         : reason;
                loop.quit();
            });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, [&loop, &error]() {
        error = tr("timed out waiting for the file size");
        loop.quit();
    });
    timeout.start(kStreamStartTimeoutMs);

    QElapsedTimer elapsed;
    elapsed.start();
    m_pNetwork->fetchFileStreaming(mac, slot, remotePath, localPath, kStreamHeadBytes);
    // A transfer that cannot even start -- no session, or a player that has
    // left -- reports it by emitting synchronously, from inside that call.
    // quit() on a loop that has not begun does nothing, so entering it anyway
    // would freeze the deck for the whole timeout over a failure already known.
    if (error.isEmpty() && !StreamingFileRegistry::lookup(localPath)) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    disconnect(ready);
    disconnect(failed);
    m_startingStream = false;

    const auto pStream = StreamingFileRegistry::lookup(localPath);
    if (!pStream) {
        kLogger.warning() << "could not start streaming" << remotePath << "--"
                          << (error.isEmpty() ? tr("no size was reported") : error);
        m_streaming.remove(localPath);
        QFile::remove(localPath);
        return QString();
    }

    // The number that says whether this is working. It should be well under a
    // second: it covers one connect, mount, open and stat, and nothing else.
    kLogger.info() << "streaming" << remotePath << "--" << pStream->size()
                   << "bytes, playable after" << elapsed.elapsed() << "ms";
    // So the file counts against the RAM cap. Pinned by the caller immediately
    // after this returns, which is why no eviction may run in between.
    m_pCache->adopt(medium, localPath, pStream->size());
    return localPath;
}

void RemoteTrackStreamer::stopStreaming(const QString& localPath) {
    if (localPath.isEmpty()) {
        return;
    }
    const auto pStream = StreamingFileRegistry::lookup(localPath);
    if (!pStream) {
        return;
    }
    if (pStream->isComplete()) {
        // A whole file now, so it is an ordinary one: unregistering it is what
        // makes the next open of it go through the normal decoders.
        StreamingFileRegistry::remove(localPath);
        return;
    }

    // Still arriving, and deliberately left alone. Not abandoned, and not
    // unregistered:
    //
    //  * the download is left running because those bytes are already paid for,
    //    and a complete file in tier 1 is what makes playing it again instant;
    //  * the record of which ranges have landed has to outlive the load, or a
    //    reload before the download finishes could not tell a real byte from a
    //    sparse hole and would have to wait for the whole file.
    //
    // The cost is that a read blocked at the moment of the unload stays blocked
    // until the next range lands -- under a second in practice, and bounded by
    // the read timeout regardless. That happens on the reader's own thread.
    m_removeWhenDone.insert(localPath);
    kLogger.info() << "unloaded while still arriving, after" << pStream->waitCount()
                   << "waits /" << pStream->waitedMs() << "ms:" << localPath;
}

void RemoteTrackStreamer::onFetchProgress(const QString& localPath,
        quint64 done,
        quint64 total,
        quint64 offset,
        quint64 length) {
    if (!m_streaming.contains(localPath)) {
        return; // An ordinary whole-file fetch. Not ours to make readable.
    }
    auto pStream = StreamingFileRegistry::lookup(localPath);
    if (!pStream) {
        if (total == 0) {
            return; // The size is not known yet, so there is nothing to build.
        }
        // Built here rather than in startStreaming() because the events are
        // drained in batches: the size and the first range of content often
        // arrive together, and a range that lands before the file exists to
        // record it against is a range nothing will ever be told about.
        pStream = std::make_shared<StreamingFile>(localPath, static_cast<qint64>(total));
        if (!pStream->error().isEmpty()) {
            kLogger.warning() << "cannot read back" << localPath << "--"
                              << pStream->error();
            return;
        }
        StreamingFileRegistry::add(localPath, pStream);
        emit streamingReady(localPath);
    }
    if (length > 0) {
        pStream->markPresent(static_cast<qint64>(offset), static_cast<qint64>(length));
    }
    Q_UNUSED(done);
}

void RemoteTrackStreamer::onFetchFinished(const QString& localPath, const QString& error) {
    if (m_streaming.remove(localPath) == 0) {
        return; // Not a streamed track.
    }
    const auto pStream = StreamingFileRegistry::lookup(localPath);
    if (error.isEmpty()) {
        m_streamComplete.insert(localPath);
        // The line that says the switch from a blocking read to an ordinary one
        // is safe to make, and the one to look for when a track stutters: if it
        // is late, the download lost the race with the playhead.
        kLogger.info() << "download complete:" << localPath
                       << (pStream ? QStringLiteral("-- %1 waits / %2 ms")
                                                .arg(pStream->waitCount())
                                                .arg(pStream->waitedMs())
                                   : QStringLiteral("-- no longer loaded"));
        if (pStream) {
            pStream->complete();
            // Unloaded while it was still arriving, and kept registered only so
            // its ranges would survive a reload. It is a whole file now, so
            // nothing is owed to it.
            if (m_removeWhenDone.remove(localPath) > 0) {
                StreamingFileRegistry::remove(localPath);
            }
        }
        return;
    }

    kLogger.warning() << "download failed:" << localPath << "--" << error;
    m_removeWhenDone.remove(localPath);
    if (pStream) {
        // Wakes readers, which then fail rather than hang. Without this the
        // deck would keep waiting on bytes that are never coming.
        pStream->fail(error);
        // Unregistered whatever happens: a failed stream must never be handed
        // to a decoder opening this path again.
        StreamingFileRegistry::remove(localPath);
    }
    // Whatever arrived is not a playable file and must not be mistaken for one
    // by the next load.
    QFile::remove(localPath);
}

} // namespace deck
} // namespace mixxx
