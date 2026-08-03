#include "library/prolink/prolinktrackfetcher.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include "library/prolink/dlgprolinkfetch.h"

#include "moc_prolinktrackfetcher.cpp"
#include "network/prolink/prolinknetworkservice.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkFetcher");

/// Ceiling on one file. A 4-deep window of 1280-byte reads runs about
/// 1.5 MB/s, so a 60 MB lossless track needs roughly 40 s; this leaves room for
/// that and for a retry or two, while still bounding a fetch from a deck that
/// has gone quiet mid-transfer rather than hanging the UI indefinitely.
constexpr int kFetchTimeoutMs = 120000;
} // namespace

ProLinkTrackFetcher::ProLinkTrackFetcher(
        mixxx::prolink::ProLinkNetworkService* pNetwork, QObject* pParent)
        : QObject(pParent),
          m_pNetwork(pNetwork) {
}

bool ProLinkTrackFetcher::fetchBlocking(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const QString& remotePath,
        const QString& localPath) {
    m_lastError.clear();
    if (QFile::exists(localPath)) {
        return true;
    }
    if (m_busy) {
        // A second double-click while the first is still in the nested loop.
        // Refused rather than queued: two nested loops would unwind in the
        // wrong order.
        m_lastError = tr("another track is already being fetched");
        return false;
    }
    if (remotePath.isEmpty()) {
        m_lastError = tr("no path for this track");
        return false;
    }

    m_busy = true;
    QEventLoop loop;
    QString error;
    bool finished = false;

    const auto connection = connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::fileFetched,
            &loop,
            [&](const QString& path, const QString& fetchError) {
                if (path != localPath) {
                    // Another fetch's result -- an optional companion, most
                    // likely. Ignoring it is what keeps the loop waiting for the
                    // file we actually asked for.
                    return;
                }
                error = fetchError;
                finished = true;
                loop.quit();
            });

    // A hard stop, so a peer that vanishes mid-transfer cannot wedge the UI. The
    // RPC layer retries and eventually times out on its own, but only for calls
    // it has outstanding; a reply that never comes at all is this timer's job.
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(kFetchTimeoutMs);
    connect(&timeout, &QTimer::timeout, &loop, [&]() {
        error = tr("timed out");
        loop.quit();
    });
    timeout.start();

    // The ring. Indeterminate until the first bytes land, because the size is
    // only known once GETATTR has answered and showing 0% before then would
    // read as a stall.
    mixxx::DlgProLinkFetch dialog(
            QFileInfo(remotePath).fileName(), QApplication::activeWindow());
    const auto progressConnection = connect(m_pNetwork,
            &mixxx::prolink::ProLinkNetworkService::fileFetchProgress,
            &dialog,
            [&dialog, localPath](const QString& path,
                    quint64 done,
                    quint64 total,
                    quint64 offset,
                    quint64 length) {
                Q_UNUSED(offset);
                Q_UNUSED(length);
                // This path fetches whole files, so the range a streaming
                // fetch would report says nothing a ring can show.
                if (path != localPath) {
                    return;
                }
                if (total == 0) {
                    dialog.setIndeterminate();
                } else {
                    dialog.setProgress(static_cast<double>(done) /
                            static_cast<double>(total));
                }
            });

    // Shown after a short delay, so a track already cached -- or one that
    // arrives quickly -- never flashes a dialog.
    QTimer showTimer;
    showTimer.setSingleShot(true);
    connect(&showTimer, &QTimer::timeout, &dialog, [&dialog]() { dialog.show(); });
    showTimer.start(250);

    // Priority: the user is watching this one, and a few hundred cover images
    // may already be queued ahead of it.
    m_pNetwork->fetchFile(mac, slot, remotePath, localPath, true);

    // ExcludeUserInputEvents: during the loop the user must not be able to
    // click another feature or double-click a second track, because the model
    // index the caller is mid-call on would be invalidated underneath it. The
    // dialog above still paints and animates, so the freeze is visible as
    // progress rather than as a hang.
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    showTimer.stop();
    disconnect(progressConnection);
    dialog.close();

    disconnect(connection);
    m_busy = false;

    if (!finished && error.isEmpty()) {
        error = tr("cancelled");
    }
    m_lastError = error;

    const bool ok = error.isEmpty() && QFile::exists(localPath);
    if (!ok) {
        kLogger.warning() << "fetch of" << remotePath << "failed:" << m_lastError;
    }
    return ok;
}

void ProLinkTrackFetcher::fetchOptional(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const QString& remotePath,
        const QString& localPath) {
    if (remotePath.isEmpty() || QFile::exists(localPath)) {
        return;
    }
    // Fire and forget: the result lands whenever it lands, and nothing waits for
    // it.
    m_pNetwork->fetchFile(mac, slot, remotePath, localPath);
}

void ProLinkTrackFetcher::fetchOptionalBlocking(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        const QString& remotePath,
        const QString& localPath) {
    if (remotePath.isEmpty() || QFile::exists(localPath)) {
        return;
    }
    // The return value is deliberately dropped: the caller has already decided
    // this file is optional, and a missing beatgrid is not a failed load. The
    // error is still logged by fetchBlocking.
    fetchBlocking(mac, slot, remotePath, localPath);
}

void ProLinkTrackFetcher::fetchArtwork(const QByteArray& mac,
        mixxx::prolink::MediaSlot slot,
        quint32 artworkId,
        const QString& localPath) {
    if (artworkId == 0 || localPath.isEmpty() || QFile::exists(localPath)) {
        return;
    }
    // Over dbserver rather than NFS, and fire-and-forget: a cover that arrives
    // after the track is loaded still gets used the next time the row is drawn.
    m_pNetwork->fetchArtwork(mac, slot, artworkId, localPath);
}
