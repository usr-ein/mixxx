#include "library/prolink/prolinkplaylistmodel.h"

#include <QDir>
#include <QFile>

#include "track/track.h"

#include "library/prolink/prolinkdbwriter.h"
#include "library/prolink/prolinktrackfetcher.h"
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

QString ProLinkPlaylistModel::artworkPathForRow(const QModelIndex& index) const {
    // Not a ColumnCache column -- artwork_path is ours alone -- so it is read
    // from the record by name.
    const int column = fieldIndex(QStringLiteral("artwork_path"));
    if (column < 0) {
        return QString();
    }
    return index.sibling(index.row(), column).data().toString();
}

void ProLinkPlaylistModel::willLoadTrack(const QModelIndex& index) {
    Q_UNUSED(index);
    // Armed for the single getTrack() that follows, and cleared by it. Anything
    // else asking -- the sort, the cover-art delegate, getTrackId() -- arrives
    // unarmed and gets a null rather than a download.
    m_bFetchOnMiss = true;
}

TrackPointer ProLinkPlaylistModel::getTrack(const QModelIndex& index) const {
    const QString location = QDir::fromNativeSeparators(
            getFieldString(index, ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION));
    if (location.isEmpty()) {
        return TrackPointer();
    }

    if (!QFile::exists(location)) {
        // Not local yet.
        //
        // Two very different callers arrive here. The table asks for every
        // visible row, looking for cover art -- those must return null, or
        // scrolling would both fetch megabytes and add the row to the user's
        // real library (see the header). A double-click means the user actually
        // wants it, and only that case may block.
        //
        // m_bFetchOnMiss is set for the duration of a deliberate load and
        // cleared straight after, so the distinction never has to be guessed
        // from context.
        // Consume the arm immediately, whatever happens next: a fetch that
        // fails must not leave the next incidental getTrack() armed.
        const bool wanted = m_bFetchOnMiss;
        m_bFetchOnMiss = false;
        if (m_pFetcher == nullptr || !wanted) {
            return TrackPointer();
        }

        // Everything the fetch needs is read *before* the nested event loop, and
        // nothing below touches `index` again. During the loop the model can be
        // reset and the index invalidated; see ProLinkTrackFetcher's header.
        const QString remotePath = m_remotePathPrefix.isEmpty()
                ? QString()
                : location.mid(m_remotePathPrefix.size());
        if (!m_pFetcher->fetchBlocking(m_mac, m_slot, remotePath, location)) {
            return TrackPointer();
        }

        // Cover art, best effort and not waited for -- a missing cover costs a
        // thumbnail, not the load. It is requested after the audio so the track
        // starts playing at the earliest possible moment.
        const QString artworkRemote = artworkPathForRow(index);
        if (!artworkRemote.isEmpty()) {
            m_pFetcher->fetchOptional(m_mac,
                    m_slot,
                    artworkRemote,
                    m_remotePathPrefix + artworkRemote);
        }
    }

    TrackPointer pTrack = BaseExternalPlaylistModel::getTrack(index);
    if (pTrack) {
        const QString artworkRemote = artworkPathForRow(index);
        const QString artworkLocal = artworkRemote.isEmpty()
                ? QString()
                : m_remotePathPrefix + artworkRemote;
        if (!artworkLocal.isEmpty() && QFile::exists(artworkLocal)) {
            // Point the track at the medium's own cover file. Without this Mixxx
            // guesses from the audio file's directory, which on a rekordbox
            // medium holds no image at all -- the art lives under PIONEER/.
            CoverInfoRelative cover;
            cover.type = CoverInfo::FILE;
            cover.source = CoverInfo::GUESSED;
            cover.coverLocation = artworkLocal;
            pTrack->setCoverInfo(cover);
        }
    }
    return pTrack;
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
