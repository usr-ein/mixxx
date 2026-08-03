#include "widget/deck/wdeckbrowser.h"

#include <QSqlDatabase>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "control/controlencoder.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/deck/deckqueries.h"
#include "library/deck/decktrackmodel.h"
#include "library/deck/pdbingest.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_wdeckbrowser.cpp"
#include "util/logger.h"
#include "widget/deck/deckdelegates.h"
#include "widget/deck/decklistview.h"
#include "widget/deck/deckmenumodel.h"

namespace {
const mixxx::Logger kLogger("DeckBrowser");

// Geometry, from browser-prd.md 5..8. Named rather than inline so the budget
// stays checkable: 48 + 496 + 56 = 600.
constexpr int kBreadcrumbHeight = 48;
constexpr int kBezelPadHeight = 56;
constexpr int kSourceRowHeight = 80;
constexpr int kMenuRowHeight = 72;
constexpr int kValueRowHeight = 64;
constexpr int kPlaylistRowHeight = 88;
constexpr int kTrackRowHeight = 72;

const QString kDeckGroup = QStringLiteral("[Channel1]");

/// Categories of a medium, in the order the PRD lists them.
struct CategorySpec {
    const char* title;
    const char* column; ///< Empty for the ones that are not a plain GROUP BY.
};
} // namespace

namespace mixxx {
namespace deck {

WDeckBrowser::WDeckBrowser(QWidget* pParent, Library* pLibrary, UserSettingsPointer pConfig)
        : QWidget(pParent),
          WBaseWidget(this),
          m_pLibrary(pLibrary),
          m_pConfig(std::move(pConfig)) {
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    m_pBreadcrumb = new QLabel(this);
    m_pBreadcrumb->setObjectName(QStringLiteral("DeckBreadcrumb"));
    m_pBreadcrumb->setFixedHeight(kBreadcrumbHeight);
    pLayout->addWidget(m_pBreadcrumb);

    m_pStack = new QStackedWidget(this);
    pLayout->addWidget(m_pStack, 1);

    m_pMenuView = new DeckListView(m_pStack);
    m_pMenuModel = new DeckMenuModel(this);
    m_pMenuDelegate = new MenuRowDelegate(this);
    m_pMenuView->setModel(m_pMenuModel);
    m_pMenuView->setItemDelegate(m_pMenuDelegate);
    m_pStack->addWidget(m_pMenuView);

    m_pTrackView = new DeckListView(m_pStack);
    m_pTrackDelegate = new TrackRowDelegate(this);
    m_pTrackView->setItemDelegate(m_pTrackDelegate);
    m_pStack->addWidget(m_pTrackView);

    // The bezel strip: the panel's last rows sit under a lip, so anything drawn
    // there cannot be touched and barely seen.
    auto* pBezelPad = new QWidget(this);
    pBezelPad->setFixedHeight(kBezelPadHeight);
    pBezelPad->setObjectName(QStringLiteral("DeckBezelPad"));
    pLayout->addWidget(pBezelPad);

    for (DeckListView* pView : {m_pMenuView, m_pTrackView}) {
        connect(pView, &DeckListView::activated, this, &WDeckBrowser::onActivated);
        connect(pView, &DeckListView::reselected, this, &WDeckBrowser::onReselected);
        connect(pView, &DeckListView::backRequested, this, &WDeckBrowser::onBack);
        connect(pView, &DeckListView::selectionMoved, this, &WDeckBrowser::onSelectionMoved);
    }

    // The track model, and the cache behind it. Same shape as the features it
    // replaces: the cache is what lets the table draw covers and sort without a
    // Track object per row.
    const QStringList columns = {
            QStringLiteral("id"),
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            QStringLiteral("label"),
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_KEY_ID,
            LIBRARYTABLE_COLOR,
            LIBRARYTABLE_DATETIMEADDED,
            LIBRARYTABLE_TIMESPLAYED,
            QStringLiteral("analyze_path"),
            QStringLiteral("artwork_path"),
            LIBRARYTABLE_COVERART,
            LIBRARYTABLE_COVERART_SOURCE,
            LIBRARYTABLE_COVERART_TYPE,
            LIBRARYTABLE_COVERART_LOCATION,
            LIBRARYTABLE_COVERART_COLOR,
            LIBRARYTABLE_COVERART_DIGEST};
    const QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM};
    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pLibrary->trackCollectionManager()->internalCollection(),
            kLibraryTable,
            QStringLiteral("id"),
            columns,
            searchColumns,
            false);
    m_pTrackModel = new DeckTrackModel(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    m_pTrackView->setModel(m_pTrackModel);

    connect(this,
            &WDeckBrowser::loadTrackToPlayer,
            m_pLibrary,
            &Library::slotLoadTrackToPlayer);

    // The tables have to exist before anything reads them, and they are
    // temporary -- dropped and recreated each boot, never migrated.
    {
        QSqlDatabase db = database();
        if (db.isOpen()) {
            dropTables(db);
            createTables(db);
        }
    }

    m_pRegistry = std::make_unique<MediaRegistry>(m_pLibrary->dbConnectionPool(), this);
    connect(m_pRegistry.get(),
            &MediaRegistry::mediaChanged,
            this,
            &WDeckBrowser::onMediaChanged);

    // ---- the deck's controls -------------------------------------------
    m_pMove = std::make_unique<ControlEncoder>(ConfigKey("[Browser]", "move"), false);
    connect(m_pMove.get(), &ControlEncoder::valueChanged, this, [this](double v) {
        const int steps = static_cast<int>(v);
        if (steps == 0) {
            return;
        }
        (inTrackList() ? m_pTrackView : m_pMenuView)->moveSelection(steps);
    });

    m_pSelect = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "select"));
    connect(m_pSelect.get(), &ControlPushButton::valueChanged, this, [this](double v) {
        if (v <= 0.0) {
            return;
        }
        DeckListView* pView = inTrackList() ? m_pTrackView : m_pMenuView;
        if (pView->selectedRow() >= 0) {
            onActivated(pView->selectedRow());
        }
    });

    m_pBack = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "back"));
    connect(m_pBack.get(), &ControlPushButton::valueChanged, this, [this](double v) {
        if (v > 0.0) {
            onBack();
        }
    });

    m_pSortMenu = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "sort_menu"));
    m_pInfoToggle = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "info_toggle"));
    connect(m_pInfoToggle.get(), &ControlPushButton::valueChanged, this, [this](double v) {
        if (v <= 0.0 || !inTrackList()) {
            return;
        }
        // Toggling is all this does for now; the info panel itself is not
        // built, so the layout change is the one-line variant of it.
        m_pTrackDelegate->setInfoLayout(!m_pTrackView->property("infoLayout").toBool());
        m_pTrackView->setProperty("infoLayout",
                !m_pTrackView->property("infoLayout").toBool());
        m_pTrackView->viewport()->update();
    });

    m_pLevelControl = std::make_unique<ControlObject>(ConfigKey("[Browser]", "level"));
    m_pLevelControl->setReadOnly();
    m_pInTrackList = std::make_unique<ControlObject>(ConfigKey("[Browser]", "in_track_list"));
    m_pInTrackList->setReadOnly();

    showSources();
    // showSources() builds level 0 but does not draw the chrome around it; the
    // constructor is the one path into it that does not go through
    // rebuildCurrentLevel().
    updateBreadcrumb();
}

WDeckBrowser::~WDeckBrowser() = default;

void WDeckBrowser::setup(const QDomNode& node, const SkinContext& context) {
    Q_UNUSED(node);
    Q_UNUSED(context);
}

QSqlDatabase WDeckBrowser::database() const {
    return m_pLibrary->trackCollectionManager()->internalCollection()->database();
}

bool WDeckBrowser::inTrackList() const {
    return !m_stack.isEmpty() && m_stack.last().kind == Level::Kind::Tracks;
}

void WDeckBrowser::pushLevel(Level level) {
    if (!m_stack.isEmpty()) {
        // Remember where the level below was left, so BACK returns to the row
        // the DJ was on rather than to the top of a rebuilt list.
        m_stack.last().selectedRow =
                (inTrackList() ? m_pTrackView : m_pMenuView)->selectedRow();
    }
    m_stack.append(std::move(level));
    rebuildCurrentLevel();
}

void WDeckBrowser::popLevel() {
    if (m_stack.size() <= 1) {
        // At level 0, BACK leaves browse mode for the deck. The skin binds
        // visibility to this control, and the script's own BACK handler is what
        // brings it back.
        ControlObject::set(ConfigKey("[Master]", "show_library"), 0.0);
        return;
    }
    m_stack.removeLast();
    rebuildCurrentLevel();
}

void WDeckBrowser::rebuildCurrentLevel() {
    if (m_stack.isEmpty()) {
        showSources();
        return;
    }
    const Level level = m_stack.last();
    switch (level.kind) {
    case Level::Kind::Sources:
        showSources();
        break;
    case Level::Kind::MediumMenu:
        showMediumMenu(level);
        break;
    case Level::Kind::Playlists:
        showPlaylists(level);
        break;
    case Level::Kind::Categories:
    case Level::Kind::Artists:
        showCategory(level);
        break;
    case Level::Kind::ArtistAlbums:
        showArtistAlbums(level);
        break;
    case Level::Kind::Tracks:
        showTracks(level, level.parameter);
        break;
    }
    m_pLevelControl->forceSet(m_stack.size() - 1);
    m_pInTrackList->forceSet(inTrackList() ? 1.0 : 0.0);
    updateBreadcrumb();
}

void WDeckBrowser::updateBreadcrumb() {
    QStringList parts;
    for (const Level& level : m_stack) {
        if (!level.title.isEmpty()) {
            parts.append(level.title);
        }
    }
    m_pBreadcrumb->setText(parts.join(QStringLiteral("  ›  ")));
}

void WDeckBrowser::onMediaChanged() {
    // Only level 0 shows media directly. Deeper levels are left alone unless
    // the medium they belong to has gone, in which case there is nothing to
    // show and the stack unwinds to the root.
    if (m_stack.size() == 1) {
        showSources();
        return;
    }
    if (m_stack.size() > 1 && m_pRegistry->indexOf(m_stack.at(1).medium) < 0) {
        while (m_stack.size() > 1) {
            m_stack.removeLast();
        }
        rebuildCurrentLevel();
    }
}

void WDeckBrowser::showSources() {
    if (m_stack.isEmpty()) {
        Level level;
        level.kind = Level::Kind::Sources;
        level.title = tr("SOURCES");
        m_stack.append(level);
    }

    QList<MenuRow> rows;
    for (const MediumInfo& medium : m_pRegistry->media()) {
        MenuRow row;
        const bool linked = !medium.id.isLocal();
        if (medium.kind == MediumInfo::Kind::Sd) {
            row.mark = linked ? MenuRow::Mark::SdLinked : MenuRow::Mark::Sd;
        } else {
            row.mark = linked ? MenuRow::Mark::UsbLinked : MenuRow::Mark::Usb;
        }
        row.playerNumber = medium.playerNumber;
        row.title = medium.name;
        switch (medium.state) {
        case MediumInfo::State::Reading:
            row.detail = tr("reading…");
            row.dimmed = true;
            break;
        case MediumInfo::State::Failed:
            row.detail = medium.error;
            row.dimmed = true;
            break;
        case MediumInfo::State::Offline:
            row.dimmed = true;
            [[fallthrough]];
        case MediumInfo::State::Ready:
            row.detail = tr("%1 tracks · %2 playlists")
                                 .arg(medium.trackCount)
                                 .arg(medium.playlistCount);
            break;
        }
        row.payload = QVariant::fromValue(medium.id.key());
        rows.append(row);
    }

    MenuRow diagnostics;
    diagnostics.mark = MenuRow::Mark::Diagnostics;
    diagnostics.title = tr("Diagnostics");
    diagnostics.payload = QStringLiteral("#diagnostics");
    rows.append(diagnostics);

    MenuRow power;
    power.mark = MenuRow::Mark::Power;
    power.title = tr("Shut down");
    power.payload = QStringLiteral("#shutdown");
    rows.append(power);

    m_pMenuModel->setRows(rows);
    m_pMenuDelegate->setRowHeight(kSourceRowHeight);
    m_pMenuView->setRowHeight(kSourceRowHeight);
    m_pStack->setCurrentWidget(m_pMenuView);
    m_pMenuView->selectRow(qBound(0, m_stack.first().selectedRow, rows.size() - 1));
}

void WDeckBrowser::showMediumMenu(const Level& level) {
    QSqlDatabase db = database();
    QList<MenuRow> rows;

    const auto add = [&rows](const QString& title, const QString& payload, int count) {
        MenuRow row;
        row.title = title;
        row.payload = payload;
        if (count >= 0) {
            row.detail = QString::number(count);
            row.dimmed = count == 0;
        }
        rows.append(row);
    };

    MenuRow search;
    search.mark = MenuRow::Mark::Search;
    search.title = tr("Search");
    search.payload = QStringLiteral("search");
    rows.append(search);

    add(tr("All tracks"), QStringLiteral("all"), trackCount(db, level.medium));
    add(tr("Playlists"), QStringLiteral("playlists"), playlistCount(db, level.medium));
    add(tr("Genre"), QStringLiteral("genre"), categoryCount(db, level.medium, QStringLiteral("genre")));
    add(tr("Artists"), QStringLiteral("artist"), categoryCount(db, level.medium, QStringLiteral("artist")));
    add(tr("Last played"), QStringLiteral("lastplayed"), -1);
    add(tr("Date added"), QStringLiteral("dateadded"), categoryCount(db, level.medium, QStringLiteral("datetime_added")));
    add(tr("BPM"), QStringLiteral("bpm"), -1);
    add(tr("Key"), QStringLiteral("key"), categoryCount(db, level.medium, QStringLiteral("key")));
    add(tr("Album"), QStringLiteral("album"), categoryCount(db, level.medium, QStringLiteral("album")));
    add(tr("Label"), QStringLiteral("label"), categoryCount(db, level.medium, QStringLiteral("label")));

    m_pMenuModel->setRows(rows);
    m_pMenuDelegate->setRowHeight(kMenuRowHeight);
    m_pMenuView->setRowHeight(kMenuRowHeight);
    m_pStack->setCurrentWidget(m_pMenuView);
    m_pMenuView->selectRow(qBound(0, level.selectedRow, rows.size() - 1));
}

void WDeckBrowser::showPlaylists(const Level& level) {
    QSqlDatabase db = database();
    QList<MenuRow> rows;
    for (const PlaylistEntry& entry : playlistsIn(db, level.medium, level.parentRbId)) {
        MenuRow row;
        row.title = entry.name;
        if (entry.isFolder) {
            row.mark = MenuRow::Mark::Folder;
            row.detail = tr("%1 playlists").arg(entry.childCount);
            row.payload = QStringLiteral("folder:%1").arg(entry.rbId);
        } else {
            row.coverPaths = entry.coverPaths;
            const int minutes = entry.durationSeconds / 60;
            row.detail = tr("%1 tracks · %2 h %3 min")
                                 .arg(entry.trackCount)
                                 .arg(minutes / 60)
                                 .arg(minutes % 60, 2, 10, QChar('0'));
            row.payload = QStringLiteral("playlist:%1").arg(entry.id);
        }
        rows.append(row);
    }
    m_pMenuModel->setRows(rows);
    m_pMenuDelegate->setRowHeight(kPlaylistRowHeight);
    m_pMenuView->setRowHeight(kPlaylistRowHeight);
    m_pStack->setCurrentWidget(m_pMenuView);
    m_pMenuView->selectRow(qBound(0, level.selectedRow, rows.size() - 1));
}

void WDeckBrowser::showCategory(const Level& level) {
    QSqlDatabase db = database();
    QList<CategoryEntry> entries;
    bool withCover = false;
    int anchor = 0;

    if (level.parameter == QStringLiteral("key")) {
        entries = keyCategory(db, level.medium);
    } else if (level.parameter == QStringLiteral("dateadded")) {
        entries = dateCategory(db, level.medium);
    } else if (level.parameter == QStringLiteral("bpm")) {
        // The list opens on what the deck is playing, so the first thing under
        // the selection is what can be mixed into it right now.
        const double playing = ControlObject::get(ConfigKey(kDeckGroup, "bpm"));
        const double rateRange = ControlObject::get(ConfigKey(kDeckGroup, "rateRange"));
        entries = bpm::buckets(db, level.medium, rateRange, playing, &anchor);
    } else {
        withCover = level.parameter == QStringLiteral("album") ||
                level.parameter == QStringLiteral("label");
        entries = textCategory(db, level.medium, level.parameter, withCover);
    }

    QList<MenuRow> rows;
    for (const CategoryEntry& entry : entries) {
        MenuRow row;
        row.title = entry.display;
        row.detail = QString::number(entry.count);
        row.dimmed = entry.count == 0;
        if (withCover && !entry.coverPath.isEmpty()) {
            row.coverPaths = QStringList{entry.coverPath};
        }
        if (entry.keyId >= 0) {
            row.payload = QStringLiteral("key:%1").arg(entry.keyId);
        } else if (entry.bpmHi > 0.0) {
            row.payload = QStringLiteral("bpm:%1:%2").arg(entry.bpmLo).arg(entry.bpmHi);
        } else {
            row.payload = QStringLiteral("value:%1").arg(entry.value);
        }
        rows.append(row);
    }

    m_pMenuModel->setRows(rows);
    const int height = withCover ? kPlaylistRowHeight : kValueRowHeight;
    m_pMenuDelegate->setRowHeight(height);
    m_pMenuView->setRowHeight(height);
    m_pStack->setCurrentWidget(m_pMenuView);
    const int row = level.selectedRow > 0 ? level.selectedRow : anchor;
    m_pMenuView->selectRow(qBound(0, row, rows.size() - 1));
}

void WDeckBrowser::showArtistAlbums(const Level& level) {
    QSqlDatabase db = database();
    QList<MenuRow> rows;
    for (const CategoryEntry& entry : albumsForArtist(db, level.medium, level.parameter)) {
        MenuRow row;
        row.title = entry.display;
        row.detail = tr("%1 tracks").arg(entry.count);
        if (!entry.coverPath.isEmpty()) {
            row.coverPaths = QStringList{entry.coverPath};
        }
        row.payload = QStringLiteral("album:%1").arg(entry.value);
        rows.append(row);
    }
    m_pMenuModel->setRows(rows);
    m_pMenuDelegate->setRowHeight(kPlaylistRowHeight);
    m_pMenuView->setRowHeight(kPlaylistRowHeight);
    m_pStack->setCurrentWidget(m_pMenuView);
    m_pMenuView->selectRow(qBound(0, level.selectedRow, rows.size() - 1));
}

void WDeckBrowser::showTracks(const Level& level, const QString& selectSql) {
    Q_UNUSED(level);
    m_pTrackModel->setQuery(selectSql);

    // Column indices, resolved once here rather than per painted row.
    TrackRowDelegate::Columns columns;
    columns.cover = m_pTrackModel->fieldIndex(LIBRARYTABLE_COVERART_LOCATION);
    columns.title = m_pTrackModel->fieldIndex(LIBRARYTABLE_TITLE);
    columns.artist = m_pTrackModel->fieldIndex(LIBRARYTABLE_ARTIST);
    columns.bpm = m_pTrackModel->fieldIndex(LIBRARYTABLE_BPM);
    columns.key = m_pTrackModel->fieldIndex(LIBRARYTABLE_KEY);
    columns.keyId = m_pTrackModel->fieldIndex(LIBRARYTABLE_KEY_ID);
    m_pTrackDelegate->setColumns(columns);
    m_pTrackDelegate->setRowHeight(kTrackRowHeight);
    // What is loaded right now, so a compatible key can be green.
    m_pTrackDelegate->setPlayingKeyId(
            static_cast<int>(ControlObject::get(ConfigKey(kDeckGroup, "key"))));

    m_pTrackView->setRowHeight(kTrackRowHeight);
    m_pStack->setCurrentWidget(m_pTrackView);
    // qBound asserts when its bounds cross, which they do for an empty list
    // (0 .. -1). An empty track list is a real state -- a genre with nothing
    // under it -- not a programming error.
    const int lastRow = m_pTrackModel->rowCount() - 1;
    if (lastRow >= 0) {
        m_pTrackView->selectRow(qBound(0, level.selectedRow, lastRow));
    }
    m_pTrackView->setFocus();
}

void WDeckBrowser::onSelectionMoved(int row) {
    Q_UNUSED(row);
}

void WDeckBrowser::onReselected(int row) {
    if (inTrackList()) {
        // A tap on the already-selected track opens the info layout, which is
        // the "tell me more" gesture. Loading is the long press.
        const bool wasInfo = m_pTrackView->property("infoLayout").toBool();
        m_pTrackView->setProperty("infoLayout", !wasInfo);
        m_pTrackDelegate->setInfoLayout(!wasInfo);
        m_pTrackView->viewport()->update();
        return;
    }
    // Above a track list a single tap goes in: menus are cheap to enter and
    // free to leave, so they do not cost a deliberate gesture.
    onActivated(row);
}

void WDeckBrowser::onBack() {
    popLevel();
}

void WDeckBrowser::loadSelectedTrack() {
    const int row = m_pTrackView->selectedRow();
    if (row < 0) {
        return;
    }
    const QModelIndex index = m_pTrackModel->index(row, 0);
    m_pTrackModel->willLoadTrack(index);
    TrackPointer pTrack = m_pTrackModel->getTrack(index);
    if (!pTrack) {
        kLogger.warning() << "no track at row" << row;
        return;
    }
    emit loadTrackToPlayer(pTrack, kDeckGroup, false);
}

void WDeckBrowser::onActivated(int row) {
    if (m_stack.isEmpty()) {
        return;
    }
    if (inTrackList()) {
        loadSelectedTrack();
        return;
    }

    const MenuRow& menuRow = m_pMenuModel->rowAt(row);
    if (menuRow.dimmed) {
        return; // Nothing to show; entering would strand the encoder.
    }
    const QString payload = menuRow.payload.toString();
    const Level current = m_stack.last();
    QSqlDatabase db = database();

    switch (current.kind) {
    case Level::Kind::Sources: {
        if (payload == QStringLiteral("#shutdown")) {
            // The same overlay the old POWER button raised; the daemon does the
            // actual poweroff, since Mixxx cannot touch the OS.
            ControlObject::set(ConfigKey("[TriMixxx]", "shutdown_confirm"), 1.0);
            return;
        }
        if (payload == QStringLiteral("#diagnostics")) {
            return; // Not built yet.
        }
        Level level;
        level.kind = Level::Kind::MediumMenu;
        level.medium = MediumId::fromKey(payload);
        level.title = menuRow.title;
        pushLevel(level);
        break;
    }
    case Level::Kind::MediumMenu: {
        Level level;
        level.medium = current.medium;
        level.title = menuRow.title;
        if (payload == QStringLiteral("all")) {
            level.kind = Level::Kind::Tracks;
            level.parameter = query::allTracks(db, current.medium);
        } else if (payload == QStringLiteral("playlists")) {
            level.kind = Level::Kind::Playlists;
            level.parentRbId = 0;
        } else if (payload == QStringLiteral("lastplayed")) {
            level.kind = Level::Kind::Tracks;
            level.parameter = query::lastPlayed(db, current.medium);
        } else if (payload == QStringLiteral("artist")) {
            level.kind = Level::Kind::Artists;
            level.parameter = QStringLiteral("artist");
        } else {
            level.kind = Level::Kind::Categories;
            level.parameter = payload == QStringLiteral("dateadded")
                    ? QStringLiteral("dateadded")
                    : payload;
        }
        pushLevel(level);
        break;
    }
    case Level::Kind::Playlists: {
        Level level;
        level.medium = current.medium;
        level.title = menuRow.title;
        if (payload.startsWith(QStringLiteral("folder:"))) {
            level.kind = Level::Kind::Playlists;
            level.parentRbId = payload.mid(7).toInt();
        } else {
            level.kind = Level::Kind::Tracks;
            level.parameter = query::playlist(payload.mid(9).toInt());
        }
        pushLevel(level);
        break;
    }
    case Level::Kind::Artists: {
        Level level;
        level.kind = Level::Kind::ArtistAlbums;
        level.medium = current.medium;
        level.title = menuRow.title;
        level.parameter = payload.mid(6); // "value:"
        pushLevel(level);
        break;
    }
    case Level::Kind::ArtistAlbums: {
        Level level;
        level.kind = Level::Kind::Tracks;
        level.medium = current.medium;
        level.title = menuRow.title;
        level.parameter = query::byArtistAlbum(
                db, current.medium, current.parameter, payload.mid(6));
        pushLevel(level);
        break;
    }
    case Level::Kind::Categories: {
        Level level;
        level.kind = Level::Kind::Tracks;
        level.medium = current.medium;
        level.title = menuRow.title;
        if (payload.startsWith(QStringLiteral("key:"))) {
            level.parameter = query::byKeyId(db, current.medium, payload.mid(4).toInt());
        } else if (payload.startsWith(QStringLiteral("bpm:"))) {
            const QStringList parts = payload.split(QChar(':'));
            level.parameter = query::byBpmRange(
                    db, current.medium, parts.at(1).toDouble(), parts.at(2).toDouble());
        } else {
            const QString value = payload.mid(6); // "value:"
            const QString column = current.parameter == QStringLiteral("dateadded")
                    ? QStringLiteral("datetime_added")
                    : current.parameter;
            level.parameter = query::byTextColumn(db, current.medium, column, value);
        }
        pushLevel(level);
        break;
    }
    case Level::Kind::Tracks:
        break;
    }
}

} // namespace deck
} // namespace mixxx
