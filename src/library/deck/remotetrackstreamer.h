#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>

#include "library/deck/mediumid.h"
#include "network/prolink/prolinktypes.h"

namespace mixxx {
namespace prolink {
class ProLinkNetworkService;
} // namespace prolink
namespace deck {

class MediaRegistry;
class TrackCache;

/// A remote track, played while it arrives.
///
/// Fetches the track's beat grid first, then streams its audio head first
/// into the track cache, and keeps the file's StreamingFile told which ranges
/// have landed: the deck decodes out of it the whole time.
/// docs/browser-streaming.md tells the whole story.
class RemoteTrackStreamer : public QObject {
    Q_OBJECT

  public:
    /// *pRegistry* says which media are still there, *pNetwork* fetches, and
    /// *pCache* names and counts the local copy. All three are used and not
    /// owned.
    RemoteTrackStreamer(MediaRegistry* pRegistry,
            mixxx::prolink::ProLinkNetworkService* pNetwork,
            TrackCache* pCache,
            QObject* pParent = nullptr);
    ~RemoteTrackStreamer() override;

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

  signals:
    /// A streamed file has become readable: its size is known and it is in the
    /// StreamingFileRegistry. Internal, and the only thing startStreaming()'s
    /// nested loop waits on.
    void streamingReady(const QString& localPath);

  private slots:
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

  private:
    /// Fetch one small companion file and wait for it, tolerating failure.
    ///
    /// The ANLZ files need this: they have to be on disk before the Track
    /// exists to apply them to, so queue-and-hope is not enough.
    void fetchCompanionBlocking(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QString& remotePath,
            const QString& localPath);

    MediaRegistry* const m_pRegistry;
    mixxx::prolink::ProLinkNetworkService* const m_pNetwork;
    TrackCache* const m_pCache;

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
};

} // namespace deck
} // namespace mixxx
