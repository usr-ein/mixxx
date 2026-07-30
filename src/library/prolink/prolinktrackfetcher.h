#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "network/prolink/prolinkdefs.h"

namespace mixxx {
namespace prolink {
class ProLinkNetworkService;
}
} // namespace mixxx

/// Makes a remote track playable locally, synchronously.
///
/// **This spins a nested event loop, and that is the single most hazardous
/// thing in the feature.** `TrackModel::getTrack()` is `const` and called
/// synchronously from the view; there is no asynchronous path, because
/// returning null and loading later bypasses the `loadTrack`/`PlayerManager`
/// chain and breaks Auto DJ, samplers, preview decks and controller mappings
/// alike. So the fetch has to block.
///
/// The rules that make it survivable, all of which the caller must honour:
///
///  * **Snapshot every field before calling.** During the loop the user can
///    click another feature, the device can vanish and the model can be reset,
///    so a `QModelIndex` held across it is dangling afterwards.
///  * **Never touch that index after this returns.** The caller inlines what it
///    needs against the snapshot instead.
///  * **One fetch at a time.** Re-entering while a fetch is in flight — which a
///    second double-click during the loop would otherwise do — is refused.
///
/// Cancellation is by closing the dialog Mixxx already shows, or by the device
/// disappearing; either way the loop exits and the caller gets false.
class ProLinkTrackFetcher : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkTrackFetcher(
            mixxx::prolink::ProLinkNetworkService* pNetwork, QObject* pParent = nullptr);

    /// Fetch *remotePath* to *localPath*, returning when it is there or failed.
    ///
    /// Returns true only if the file now exists locally.
    bool fetchBlocking(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QString& remotePath,
            const QString& localPath);

    /// Best-effort companion fetch. Failure is not reported: a missing cover or
    /// beatgrid costs a feature, not the load.
    void fetchOptional(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QString& remotePath,
            const QString& localPath);

    bool isBusy() const {
        return m_busy;
    }
    QString lastError() const {
        return m_lastError;
    }

  private:
    mixxx::prolink::ProLinkNetworkService* const m_pNetwork;
    bool m_busy = false;
    QString m_lastError;
};
