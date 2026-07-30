#pragma once

#include <QSharedPointer>

#include "library/baseexternalplaylistmodel.h"

class BaseTrackCache;
class TrackCollectionManager;

/// The track table shown when a ProLink playlist is selected.
///
/// Rows come from the temporary `prolink_*` tables, filled from a player's
/// `export.pdb`. Everything visible — title, artist, BPM, key, duration — is
/// metadata the player's own database supplied, so the table is fully browsable
/// before a single audio byte has been fetched.
///
/// **Loading a track is not implemented yet**, and `getTrack()` is deliberately
/// left to the base class rather than stubbed. The base builds a track from the
/// row's `location`, which currently points into a cache directory that nothing
/// has written to, so a double-click fails to find the file and does nothing.
/// That is the honest intermediate state: making it work means fetching the
/// audio and the ANLZ files first, which is the next increment. Faking it — by
/// synthesising a track with no file behind it — would put unplayable rows into
/// Mixxx's real library, which is much harder to undo than an inert
/// double-click.
class ProLinkPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT

  public:
    ProLinkPlaylistModel(QObject* pParent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);

    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;
};
