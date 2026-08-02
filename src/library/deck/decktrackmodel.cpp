#include "library/deck/decktrackmodel.h"

#include <QSqlQuery>

#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/deck/pdbingest.h"
#include "library/queryutil.h"
#include "library/trackcollectionmanager.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("DeckTrackModel");
} // namespace

namespace mixxx {
namespace deck {

DeckTrackModel::DeckTrackModel(QObject* pParent,
        TrackCollectionManager* pTrackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(pParent,
                  pTrackCollectionManager,
                  "mixxx.db.model.deck",
                  kPlaylistsTable,
                  kPlaylistTracksTable,
                  trackSource),
          m_trackSource(std::move(trackSource)) {
}

void DeckTrackModel::setQuery(const QString& selectSql) {
    const QString previousView = m_currentView;
    const QString viewName =
            QStringLiteral("deck_view_%1_%2")
                    .arg(reinterpret_cast<quintptr>(this))
                    .arg(++m_viewSerial);

    // The column ORDER matters, not just the set: BaseSqlTableModel takes the
    // first as the id column, and the empty `preview` is what the base class
    // expects to find where a playlist's preview column would be.
    const QString queryString =
            QStringLiteral(
                    "CREATE TEMPORARY VIEW %1 AS "
                    "SELECT track_id, position, '' AS %2 FROM (%3)")
                    .arg(viewName, LIBRARYTABLE_PREVIEW, selectSql);

    QSqlQuery query(m_database);
    query.prepare(queryString);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query) << "could not create the view for this track list";
        return;
    }
    m_currentView = viewName;

    const QStringList columns = {
            QStringLiteral("track_id"),
            QStringLiteral("position"),
            LIBRARYTABLE_PREVIEW};
    setTable(viewName, columns.first(), columns, m_trackSource);

    // Ascending position *is* the browser's "Default" (browser-prd.md 9.2):
    // whatever order the query produced, which for a playlist is the DJ's own.
    // There is no column id meaning "unsorted", so the order has to be carried
    // by the data rather than by a sort.
    setDefaultSort(fieldIndex(QStringLiteral("position")), Qt::AscendingOrder);
    setSearch(QString());

    // Only once the model has moved on. Dropping the view it is still selecting
    // from is how this turns into an empty list for reasons that look like a
    // query bug.
    if (!previousView.isEmpty()) {
        QSqlQuery drop(m_database);
        drop.prepare(QStringLiteral("DROP VIEW IF EXISTS %1").arg(previousView));
        if (!drop.exec()) {
            LOG_FAILED_QUERY(drop);
        }
    }
}

} // namespace deck
} // namespace mixxx

// Mixxx keeps mocs_compilation.cpp empty on purpose and checks that it is, in
// src/util/moc_included_test.cpp -- so every Q_OBJECT class includes its own
// moc here rather than letting CMake lump them together. Forgetting it fails
// the build with "mocs_compilation.cpp not empty", which names neither this
// file nor the class.
#include "moc_decktrackmodel.cpp"
