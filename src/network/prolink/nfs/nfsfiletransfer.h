#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <functional>

#include "network/prolink/nfs/nfsv2client.h"

namespace mixxx {
namespace prolink {
namespace nfs {

/// Fetch one whole file from a player, with several reads in flight.
///
/// NFSv2 caps a single READ at 8192 bytes, so a 1 MB database is ~130 round
/// trips and a track is thousands. Issuing them one at a time would make the
/// transfer latency-bound: at a 1 ms round trip that is 8 MB/s in theory and far
/// less in practice, because each reply must be fully processed before the next
/// request leaves. A small window of concurrent reads removes that.
///
/// **Chunk size: 8192**, the NFSv2 maximum and what real players use, relying on
/// IP fragmentation to carry it over a 1500-byte MTU (F19). An earlier default
/// of 1280 stayed under the MTU and measured 1459 KiB/s, but that is 6.4x the
/// round trips a player itself makes, and a track load on this link is bound by
/// latency rather than bandwidth. Since a CDJ does exactly this to its peers all
/// day, matching it is not the rude choice it might appear.
///
/// Out-of-order replies are normal on UDP and are reassembled by offset.
class NfsFileTransfer : public QObject {
    Q_OBJECT

  public:
    struct Result {
        bool ok = false;
        QString error;
        QByteArray data;
        /// Wall-clock milliseconds, for the throughput line in the log.
        qint64 elapsedMs = 0;
        int reads = 0;
        /// Reads that came back shorter than asked for, other than the last.
        /// Legal in NFS, but if it is not zero the window logic should be
        /// re-examined rather than trusted.
        int shortReads = 0;
    };
    using Callback = std::function<void(const Result&)>;
    /// Bytes assembled so far, and the total expected. Emitted as replies
    /// arrive, so it can jump by a window's worth at a time.
    using ProgressCallback = std::function<void(quint32 done, quint32 total)>;

    void setProgressCallback(ProgressCallback callback) {
        m_progress = std::move(callback);
    }

    NfsFileTransfer(NfsV2Client* pClient, QObject* parent = nullptr);

    void setChunkSize(quint32 bytes) {
        m_chunkSize = bytes;
    }
    void setWindow(int reads) {
        m_window = reads;
    }

    /// *handle* must already be resolved. *expectedSize* comes from GETATTR or
    /// from the LOOKUP that produced the handle.
    void fetch(const QByteArray& handle, quint32 expectedSize, Callback callback);

  private:
    void pump();
    void finish();

    NfsV2Client* const m_pClient;

    QByteArray m_handle;
    quint32 m_expectedSize = 0;
    Callback m_callback;

    /// Offset -> bytes. A map rather than one big buffer written in place,
    /// because replies arrive out of order and a short read makes the next
    /// offset depend on the previous reply's actual length.
    QMap<quint32, QByteArray> m_chunks;
    quint32 m_nextOffset = 0;
    int m_inFlight = 0;
    bool m_failed = false;
    bool m_done = false;
    QString m_error;

    ProgressCallback m_progress;
    quint32 m_assembled = 0;

    QElapsedTimer m_timer;
    int m_reads = 0;
    int m_shortReads = 0;

    /// 8192 is the NFSv2 maximum and what real players use, relying on IP
    /// fragmentation to carry it over a 1500-byte MTU (F19). We defaulted to
    /// 1280 to stay under the MTU, which cost 6.4x the round trips for no
    /// measured benefit -- and on this link a track load is latency-bound, not
    /// bandwidth-bound. Reverting to 1280 is a one-line change if fragmentation
    /// ever turns out to matter on a busier network.
    quint32 m_chunkSize = 8192;
    int m_window = 4;
};

} // namespace nfs
} // namespace prolink
} // namespace mixxx
