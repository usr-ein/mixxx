#include "library/prolink/prolinkplaylistmodel.h"

#include "library/prolink/prolinkdbwriter.h"
#include "moc_prolinkplaylistmodel.cpp"

ProLinkPlaylistModel::ProLinkPlaylistModel(QObject* pParent,
        TrackCollectionManager* pTrackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(pParent,
                  pTrackCollectionManager,
                  "mixxx.db.model.prolink.playlistmodel",
                  mixxx::prolink::kProLinkPlaylistsTable,
                  mixxx::prolink::kProLinkPlaylistTracksTable,
                  trackSource) {
}

bool ProLinkPlaylistModel::isColumnHiddenByDefault(int column) {
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE)) {
        return true;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool ProLinkPlaylistModel::isColumnInternal(int column) {
    // analyze_path is ours to use, not the user's to look at. ColumnCache keys
    // on the plain column name, so a prolink_library table with an analyze_path
    // column gets the mapping for free -- no columncache change needed.
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}
