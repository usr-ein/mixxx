#pragma once

#include <QSharedPointer>

#include "library/baseexternalplaylistmodel.h"
#include "network/prolink/prolinkdefs.h"

class BaseTrackCache;
class TrackCollectionManager;
class ProLinkTrackFetcher;

/// The track table shown when a ProLink playlist is selected.
///
/// Rows come from the temporary `prolink_*` tables, filled from a player's
/// `export.pdb`. Everything visible — title, artist, BPM, key, duration — is
/// metadata the player's own database supplied, so the table is fully browsable
/// before a single audio byte has been fetched.
///
/// **`getTrack()` refuses to build a track whose file is not there yet**, and
/// that override is not optional.
///
/// `BaseExternalPlaylistModel::getTrack()` calls `getOrAddTrack()`
/// unconditionally, which writes a row into Mixxx's *real* `library` and
/// `track_locations` tables. And the track table calls `getTrack()` per visible
/// row to look for cover art — so without this override, merely *scrolling* a
/// ProLink playlist permanently adds one Missing Track to the user's own library
/// for every row seen, each pointing at a cache path nothing has written. It
/// also produced a TagLib warning per row and re-saved empty metadata over the
/// values the player's database supplied.
///
/// Returning null until the file exists costs nothing today: the columns render
/// from SQL, and cover art was unavailable anyway. Once the fetcher lands and
/// the audio is on disk, the base implementation takes over unchanged.
class ProLinkPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT

  public:
    ProLinkPlaylistModel(QObject* pParent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);

    /// The fetcher used to make a track local on demand. Not owned.
    void setFetcher(ProLinkTrackFetcher* pFetcher) {
        m_pFetcher = pFetcher;
    }

    /// Which medium the currently shown playlist came from, and where its files
    /// live locally. Set alongside setPlaylist().
    void setMedium(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QString& localRoot) {
        m_mac = mac;
        m_slot = slot;
        m_remotePathPrefix = localRoot;
    }



    void willLoadTrack(const QModelIndex& index) override;
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

  private:
    QString artworkPathForRow(const QModelIndex& index) const;

    ProLinkTrackFetcher* m_pFetcher = nullptr;
    QByteArray m_mac;
    mixxx::prolink::MediaSlot m_slot = mixxx::prolink::MediaSlot::Usb;
    /// Local root for this medium; a track's remote path is its location with
    /// this prefix removed, which is exact because the location was built by
    /// concatenating the two in the first place.
    QString m_remotePathPrefix;
    /// Mutable because getTrack() is const and must consume the arm.
    mutable bool m_bFetchOnMiss = false;
};
