#pragma once

#include <QSharedPointer>
#include <QString>

#include "library/baseexternalplaylistmodel.h"

class BaseTrackCache;
class TrackCollectionManager;

namespace mixxx {
namespace deck {

/// Every track list the browser shows, whatever produced it.
///
/// **One model class for playlists, genres, albums, BPM buckets, search results
/// and play history alike**, because they are all the same thing: a set of track
/// ids in an order. `BaseExternalPlaylistModel` already had the mechanism —
/// `setPlaylistById()` builds a `CREATE TEMPORARY VIEW` yielding
/// `(track_id, position)` and points `BaseSqlTableModel` at it — and nothing
/// about that is playlist-specific. This exposes it for an arbitrary query.
///
/// What that buys, for free and without a line of new model code: sorting,
/// searching, cover art, the Camelot key mapping, and column formatting.
///
/// It also settles what "unsorted" means. The view carries the intended order in
/// `position` and the default sort is `position ascending`, so the browser's
/// Default sort (browser-prd.md 9.2) is simply *no* sort — the DJ's own order
/// for a playlist, the medium's for anything else.
class DeckTrackModel : public BaseExternalPlaylistModel {
    Q_OBJECT

  public:
    DeckTrackModel(QObject* pParent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);

    /// Show exactly the tracks *selectSql* yields, in the order it yields them.
    ///
    /// *selectSql* must produce two columns named `track_id` and `position`.
    /// Anything expressible in SQLite is allowed — a join against the playlist
    /// entries, a `WHERE genre = ?`, a `LIKE` over three columns with a computed
    /// rank. Binding is the caller's job: this takes finished SQL, because the
    /// temporary view it wraps cannot carry placeholders.
    void setQuery(const QString& selectSql);

  private:
    QSharedPointer<BaseTrackCache> m_trackSource;
    /// Views are named per instance and per call. A single reused name would
    /// have to be dropped while the model is still selecting from it, and the
    /// counter is cheaper than reasoning about when that is safe.
    int m_viewSerial = 0;
    QString m_currentView;
};

} // namespace deck
} // namespace mixxx
