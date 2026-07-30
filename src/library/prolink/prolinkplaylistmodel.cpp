#include "library/prolink/prolinkplaylistmodel.h"

#include <QDir>
#include <QFile>

#include "track/track.h"

#include "library/prolink/prolinkdbwriter.h"
#include "library/prolink/prolinktrackfetcher.h"
#include "library/rekordbox/rekordboxanalysis.h"
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

QString ProLinkPlaylistModel::analyzePathForRow(const QModelIndex& index) const {
    return QDir::fromNativeSeparators(
            getFieldString(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH));
}

quint32 ProLinkPlaylistModel::artworkIdForRow(const QModelIndex& index) const {
    const int column = fieldIndex(QStringLiteral("artwork_id"));
    if (column < 0) {
        return 0;
    }
    return index.sibling(index.row(), column).data().toUInt();
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
        // Everything read off the row is snapshotted here, before any nested
        // event loop runs. `index` is not touched again until the fetches are
        // done, because the model can be reset underneath us while one is in
        // flight -- see ProLinkTrackFetcher's header.
        const QString analyzeLocal = analyzePathForRow(index);
        const QString artworkRemote = artworkPathForRow(index);
        const quint32 artworkId = artworkIdForRow(index);

        if (!m_pFetcher->fetchBlocking(m_mac, m_slot, remotePath, location)) {
            return TrackPointer();
        }

        // The rekordbox analysis: beat grid, hot cues, loops, memory cues. Two
        // files, and both are wanted -- grids are only correct in the legacy
        // .DAT while cues are better in the .EXT.
        //
        // Fetched synchronously, unlike the cover, because they have to be on
        // disk before the Track exists to apply them to. They are tens of
        // kilobytes, so they usually land inside the 250 ms before the progress
        // ring appears at all. Failure is tolerated: a track with no grid still
        // plays, it just arrives unanalysed.
        if (!analyzeLocal.isEmpty() && !m_remotePathPrefix.isEmpty()) {
            const QString analyzeRemote = analyzeLocal.mid(m_remotePathPrefix.size());
            m_pFetcher->fetchOptionalBlocking(m_mac, m_slot, analyzeRemote, analyzeLocal);
            const QString extRemote =
                    analyzeRemote.left(analyzeRemote.length() - 3) + QStringLiteral("EXT");
            const QString extLocal =
                    analyzeLocal.left(analyzeLocal.length() - 3) + QStringLiteral("EXT");
            m_pFetcher->fetchOptionalBlocking(m_mac, m_slot, extRemote, extLocal);
        }

        // Cover art, best effort and not waited for -- a missing cover costs a
        // thumbnail, not the load. Requested last so the track starts playing at
        // the earliest possible moment. Normally already on disk from the
        // prefetch; this is the retry for one that was not.
        if (!artworkRemote.isEmpty()) {
            m_pFetcher->fetchArtwork(m_mac,
                    m_slot,
                    artworkId,
                    m_remotePathPrefix + artworkRemote);
        }
    }

    TrackPointer pTrack = BaseExternalPlaylistModel::getTrack(index);
    if (pTrack) {
        // Beat grid and cues, from the ANLZ files fetched above or from a
        // previous load of the same track. Shared with the Rekordbox feature:
        // once the files are on local disk the two cases are identical, and the
        // beats are tagged with rekordbox's subversion so Mixxx's own analyzer
        // will not overwrite the grid.
        mixxx::rekordbox::applyAnalysis(pTrack, analyzePathForRow(index));

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
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART)) {
        // Shown by default, unlike in the main library. A rekordbox medium
        // always carries its own art and it is prefetched with the database, so
        // the column has something in it from the moment the playlist opens --
        // which is not true of a freshly scanned local folder.
        return false;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool ProLinkPlaylistModel::isColumnInternal(int column) {
    // analyze_path is ours to use, not the user's to look at. ColumnCache keys
    // on the plain column name, so a prolink_library table with an analyze_path
    // column gets the mapping for free -- no columncache change needed.
    //
    // The two artwork columns are ours as well: they say where a cover lives and
    // what to ask for it by, which is bookkeeping rather than track information.
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            column == fieldIndex(QStringLiteral("artwork_path")) ||
            column == fieldIndex(QStringLiteral("artwork_id")) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}
