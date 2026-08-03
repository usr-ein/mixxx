#include "widget/deck/wdeckbrowser.h"

#include <QSqlDatabase>
#include <QDateTime>
#include <QHBoxLayout>
#include <QSqlQuery>
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
#include "library/queryutil.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "track/track.h"
#include "track/trackref.h"
#include "moc_wdeckbrowser.cpp"
#include "util/logger.h"
#include "widget/deck/deckdelegates.h"
#include "widget/deck/decklistview.h"
#include "widget/deck/deckmenumodel.h"
#include "widget/deck/wdeckdiagnostics.h"
#include "widget/deck/wdeckinfopanel.h"
#include "widget/deck/wdecksearch.h"
#include "widget/deck/wdecksortmenu.h"

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
/// 1024 - 560 for the list beside it (browser-prd.md 8.2).
constexpr int kInfoPanelWidth = 464;

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
    setAttribute(Qt::WA_StyledBackground, true);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    m_pBreadcrumb = new QLabel(this);
    m_pBreadcrumb->setObjectName(QStringLiteral("DeckBreadcrumb"));
    m_pBreadcrumb->setFixedHeight(kBreadcrumbHeight);
    // Rich text, so each segment can be a link back to its level. The href is
    // the level INDEX rather than its name -- two levels can share a name, and
    // matching on text would jump to the wrong one.
    m_pBreadcrumb->setTextFormat(Qt::RichText);
    m_pBreadcrumb->setOpenExternalLinks(false);
    m_pBreadcrumb->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(m_pBreadcrumb, &QLabel::linkActivated, this, &WDeckBrowser::onBreadcrumbClicked);
    pLayout->addWidget(m_pBreadcrumb);

    m_pStack = new QStackedWidget(this);
    pLayout->addWidget(m_pStack, 1);

    m_pMenuView = new DeckListView(m_pStack);
    m_pMenuModel = new DeckMenuModel(this);
    m_pMenuDelegate = new MenuRowDelegate(this);
    m_pMenuView->setModel(m_pMenuModel);
    m_pMenuView->setItemDelegate(m_pMenuDelegate);
    m_pStack->addWidget(m_pMenuView);

    m_pTracksPage = new QWidget(m_pStack);
    auto* pTracksLayout = new QHBoxLayout(m_pTracksPage);
    pTracksLayout->setContentsMargins(0, 0, 0, 0);
    pTracksLayout->setSpacing(0);
    m_pTrackView = new DeckListView(m_pTracksPage);
    m_pTrackDelegate = new TrackRowDelegate(this);
    m_pTrackView->setItemDelegate(m_pTrackDelegate);
    pTracksLayout->addWidget(m_pTrackView, 1);
    m_pInfoPanel = new WDeckInfoPanel(m_pTracksPage);
    m_pInfoPanel->setFixedWidth(kInfoPanelWidth);
    m_pInfoPanel->hide();
    pTracksLayout->addWidget(m_pInfoPanel);
    m_pStack->addWidget(m_pTracksPage);

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

    // The search page. The results reuse the track view rather than a second
    // list, so a hit behaves exactly like any other track row -- long press to
    // load, tap to see more.
    m_pSearchPage = new QWidget(m_pStack);
    m_pSearchPage->setObjectName(QStringLiteral("DeckSearchPage"));
    m_pSearchPage->setAttribute(Qt::WA_StyledBackground, true);
    auto* pSearchLayout = new QVBoxLayout(m_pSearchPage);
    pSearchLayout->setContentsMargins(0, 0, 0, 0);
    pSearchLayout->setSpacing(0);
    m_pSearchQuery = new QLabel(m_pSearchPage);
    m_pSearchQuery->setObjectName(QStringLiteral("DeckSearchQuery"));
    m_pSearchQuery->setFixedHeight(56);
    pSearchLayout->addWidget(m_pSearchQuery);
    m_pSearchResults = new QWidget(m_pSearchPage);
    auto* pResultsLayout = new QVBoxLayout(m_pSearchResults);
    pResultsLayout->setContentsMargins(0, 0, 0, 0);
    pSearchLayout->addWidget(m_pSearchResults, 1);
    m_pKeyboard = new WDeckKeyboard(m_pSearchPage);
    m_pKeyboard->setFixedHeight(300);
    pSearchLayout->addWidget(m_pKeyboard);
    m_pStack->addWidget(m_pSearchPage);

    m_pDiagnostics = new WDeckDiagnostics(m_pStack);
    m_pStack->addWidget(m_pDiagnostics);

    connect(m_pKeyboard, &WDeckKeyboard::keyPressed, this, [this](const QString& c) {
        m_searchText += c;
        runSearch();
    });
    connect(m_pKeyboard, &WDeckKeyboard::backspacePressed, this, [this]() {
        m_searchText.chop(1);
        runSearch();
    });
    connect(m_pKeyboard, &WDeckKeyboard::clearPressed, this, [this]() {
        m_searchText.clear();
        runSearch();
    });
    connect(m_pKeyboard, &WDeckKeyboard::donePressed, this, [this]() {
        // Fold the keyboard away and give the whole screen to the results.
        m_pKeyboard->setVisible(false);
    });

    // The sort menu floats over the lists rather than sitting in the layout:
    // it is an overlay, and reflowing the list under it would move the rows
    // out from under the finger that opened it.
    m_pSortMenu = new WDeckSortMenu(this);
    connect(m_pSortMenu, &WDeckSortMenu::sortChosen, this, &WDeckBrowser::onSortChosen);
    connect(m_pSortMenu, &WDeckSortMenu::defaultChosen, this, &WDeckBrowser::onSortDefault);

    // The deck never plays off removable media; everything goes through here.
    m_pCache = std::make_unique<TrackCache>(this);

    // A DJ looks at a track before loading it, so the dwell is what turns
    // "selected" into "probably wanted" without copying everything scrolled
    // past. 300 ms is short enough to be ready by the time the encoder is
    // pushed and long enough that spinning through a list copies nothing.
    m_prefetchDwell.setSingleShot(true);
    m_prefetchDwell.setInterval(300);
    connect(&m_prefetchDwell, &QTimer::timeout, this, [this]() {
        if (!inTrackList() || !m_pCache) {
            return;
        }
        const int row = m_pTrackView->selectedRow();
        const int locationColumn = m_pTrackModel->fieldIndex(TRACKLOCATIONSTABLE_LOCATION);
        if (row < 0 || locationColumn < 0) {
            return;
        }
        const QString source =
                m_pTrackModel->index(row, locationColumn).data(Qt::DisplayRole).toString();
        m_pCache->prefetch(currentMedium(), source);
    });

    m_pRegistry = std::make_unique<MediaRegistry>(m_pLibrary->dbConnectionPool(), this);
    // A medium that has gone cannot be re-read, so what is cached from it is
    // now the only copy and must not be dropped to reclaim space.
    connect(m_pRegistry.get(),
            &MediaRegistry::mediumVanished,
            this,
            [this](mixxx::deck::MediumInfo medium) {
                if (m_pCache) {
                    m_pCache->markUnreachable(medium.id);
                }
            });
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
        if (m_pSortMenu->isOpen()) {
            m_pSortMenu->moveSelection(steps);
            return;
        }
        (inTrackList() ? m_pTrackView : m_pMenuView)->moveSelection(steps);
    });

    m_pSelect = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "select"));
    connect(m_pSelect.get(), &ControlPushButton::valueChanged, this, [this](double v) {
        if (v <= 0.0) {
            return;
        }
        if (m_pSortMenu->isOpen()) {
            m_pSortMenu->activateSelection();
            return;
        }
        DeckListView* pView = inTrackList() ? m_pTrackView : m_pMenuView;
        if (pView->selectedRow() >= 0) {
            onActivated(pView->selectedRow());
        }
    });

    m_pBack = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "back"));
    connect(m_pBack.get(), &ControlPushButton::valueChanged, this, [this](double v) {
        if (v <= 0.0) {
            return;
        }
        if (m_pSortMenu->isOpen()) {
            // BACK closes the menu without changing anything, rather than
            // popping a level out from under it.
            m_pSortMenu->dismiss();
            return;
        }
        onBack();
    });

    m_pSortMenuControl = std::make_unique<ControlPushButton>(
            ConfigKey("[Browser]", "sort_menu"));
    connect(m_pSortMenuControl.get(),
            &ControlPushButton::valueChanged,
            this,
            [this](double v) {
                if (v <= 0.0) {
                    return;
                }
                if (m_pSortMenu->isOpen()) {
                    m_pSortMenu->dismiss();
                    return;
                }
                // Only over a track list. Anywhere else the button does
                // nothing at all, which is what the dark LED already says.
                if (!inTrackList()) {
                    return;
                }
                m_pSortMenu->move((width() - m_pSortMenu->width()) / 2, 80);
                m_pSortMenu->open(m_sortColumn);
            });
    m_pInfoToggle = std::make_unique<ControlPushButton>(ConfigKey("[Browser]", "info_toggle"));
    connect(m_pInfoToggle.get(), &ControlPushButton::valueChanged, this, [this](double v) {
        if (v <= 0.0 || !inTrackList()) {
            return;
        }
        setInfoLayout(!m_infoLayout);
    });

    // The deck, watched. Everything below reacts to what is playing rather
    // than to the list being redrawn.
    m_pPlayingKey = std::make_unique<ControlProxy>(kDeckGroup, QStringLiteral("key"), this);
    m_pPlayingKey->connectValueChanged(this, &WDeckBrowser::onPlayingKeyChanged);
    // track_loaded as well as key: unloading leaves `key` where it was, so
    // without this the colouring would linger against a track that is gone.
    m_pTrackLoaded = std::make_unique<ControlProxy>(
            kDeckGroup, QStringLiteral("track_loaded"), this);
    m_pTrackLoaded->connectValueChanged(this, &WDeckBrowser::onPlayingKeyChanged);
    m_pRateRange = std::make_unique<ControlProxy>(
            kDeckGroup, QStringLiteral("rateRange"), this);
    m_pRateRange->connectValueChanged(this, &WDeckBrowser::onRateRangeChanged);
    // Our half of "Last played": what THIS deck played, which is the half that
    // matters during a set, since the stick's own history is somebody else's
    // and stops at the last time a real CDJ mounted it.
    m_pPlayPosition = std::make_unique<ControlProxy>(
            kDeckGroup, QStringLiteral("playposition"), this);
    m_pPlayPosition->connectValueChanged(this, &WDeckBrowser::onPlayPositionChanged);

    m_pLevelControl = std::make_unique<ControlObject>(ConfigKey("[Browser]", "level"));
    m_pLevelControl->setReadOnly();
    m_pInTrackList = std::make_unique<ControlObject>(ConfigKey("[Browser]", "in_track_list"));
    m_pInTrackList->setReadOnly();
    m_pSortColumnControl = std::make_unique<ControlObject>(
            ConfigKey("[Browser]", "sort_column"));
    m_pSortColumnControl->setReadOnly();
    m_pSortOrderControl = std::make_unique<ControlObject>(
            ConfigKey("[Browser]", "sort_order"));
    m_pSortOrderControl->setReadOnly();

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
    if (m_stack.isEmpty()) {
        return false;
    }
    // Search results are a track list in every way that matters: the same
    // view, model and delegate, so sorting and the info layout apply there too.
    const Level::Kind kind = m_stack.last().kind;
    return kind == Level::Kind::Tracks || kind == Level::Kind::Search;
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
    case Level::Kind::Search:
        showSearch(level);
        break;
    case Level::Kind::Diagnostics:
        m_pStack->setCurrentWidget(m_pDiagnostics);
        m_pDiagnostics->setFocus();
        break;
    }
    // Sampling follows visibility: a page nobody is looking at has no business
    // reading /proc once a second.
    m_pDiagnostics->setActive(level.kind == Level::Kind::Diagnostics);
    m_pLevelControl->forceSet(m_stack.size() - 1);
    m_pInTrackList->forceSet(inTrackList() ? 1.0 : 0.0);
    updateBreadcrumb();
}

void WDeckBrowser::updateBreadcrumb() {
    QStringList parts;
    for (int i = 0; i < m_stack.size(); ++i) {
        // The root is a house, not the word SOURCES: it is the one segment
        // that is always there, and an icon says "start again" in less width
        // than any word does. The glyph is the skin's Nerd Font, so it costs no
        // asset.
        const QString label = i == 0
                ? QStringLiteral("\uf015")
                : m_stack.at(i).title.toHtmlEscaped();
        if (label.isEmpty()) {
            continue;
        }
        // Padded, because a link the height of a line of text is a poor target
        // for a fingertip in a 48 px bar.
        parts.append(QStringLiteral(
                "<a style='color:#88ff00; text-decoration:none;' href='%1'>"
                "&nbsp;%2&nbsp;</a>")
                             .arg(i)
                             .arg(label));
    }
    QString text = parts.join(QStringLiteral("<span style='color:#557700'> › </span>"));
    if (inTrackList() && !m_sortColumn.isEmpty()) {
        // The arrow says the direction and the name says the field, so the
        // breadcrumb answers "what am I looking at, and in what order".
        for (const WDeckSortMenu::Field& field : WDeckSortMenu::fields()) {
            if (field.column == m_sortColumn) {
                text += QStringLiteral(
                        "<span style='color:#aaaaaa'>&nbsp;&nbsp;&nbsp;&nbsp;"
                        "%1 %2</span>")
                                .arg(m_sortDescending ? QStringLiteral("▼")
                                                      : QStringLiteral("▲"),
                                        field.title.toHtmlEscaped());
                break;
            }
        }
    }
    m_pBreadcrumb->setText(text);
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
        row.slot = medium.slot;
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

    if (rows.isEmpty()) {
        // Nothing plugged in and no players. Saying so beats a menu with two
        // rows in it and no explanation for why.
        MenuRow empty;
        empty.title = tr("Insert a USB stick, or link a player");
        empty.dimmed = true;
        rows.append(empty);
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
    // Reclaim the track view if search had it.
    if (m_pTrackView->parentWidget() != m_pTracksPage) {
        auto* pLayout = qobject_cast<QHBoxLayout*>(m_pTracksPage->layout());
        if (pLayout) {
            pLayout->insertWidget(0, m_pTrackView, 1);
        }
    }
    m_pTrackModel->setQuery(selectSql);

    refreshTrackColumns();

    applySort();
    m_pTrackView->setRowHeight(kTrackRowHeight);
    m_pStack->setCurrentWidget(m_pTracksPage);
    // qBound asserts when its bounds cross, which they do for an empty list
    // (0 .. -1). An empty track list is a real state -- a genre with nothing
    // under it -- not a programming error.
    const int lastRow = m_pTrackModel->rowCount() - 1;
    if (lastRow >= 0) {
        m_pTrackView->selectRow(qBound(0, level.selectedRow, lastRow));
    }
    updateInfoPanel();
    m_pTrackView->setFocus();
}

void WDeckBrowser::showSearch(const Level& level) {
    Q_UNUSED(level);
    m_pKeyboard->setVisible(true);
    // No room for the panel beside a keyboard; the layout toggle still applies
    // to the rows themselves.
    m_pInfoPanel->hide();
    // The track view is re-parented into the search page and back, so the
    // results are literally the same widget, model and delegate as a track
    // list -- there is no second code path to keep in step.
    m_pSearchResults->layout()->addWidget(m_pTrackView);
    m_pTrackView->show();
    m_pStack->setCurrentWidget(m_pSearchPage);
    runSearch();
}

void WDeckBrowser::refreshTrackColumns() {
    // Resolved once per model change rather than per painted row: the delegate
    // runs for every visible row on every detent, and a name lookup in there
    // turns a scroll into hundreds of string compares a frame.
    //
    // **Every path that changes the model must call this**, search included --
    // a stale index set draws a list of blank rows, which looks like a query
    // that returned nothing.
    TrackRowDelegate::Columns columns;
    columns.cover = m_pTrackModel->fieldIndex(LIBRARYTABLE_COVERART_LOCATION);
    columns.title = m_pTrackModel->fieldIndex(LIBRARYTABLE_TITLE);
    columns.artist = m_pTrackModel->fieldIndex(LIBRARYTABLE_ARTIST);
    columns.bpm = m_pTrackModel->fieldIndex(LIBRARYTABLE_BPM);
    columns.key = m_pTrackModel->fieldIndex(LIBRARYTABLE_KEY);
    columns.keyId = m_pTrackModel->fieldIndex(LIBRARYTABLE_KEY_ID);
    columns.color = m_pTrackModel->fieldIndex(LIBRARYTABLE_COLOR);
    columns.trackId = m_pTrackModel->fieldIndex(QStringLiteral("track_id"));
    m_pTrackDelegate->setColumns(columns);
    m_pTrackDelegate->setLoadedTrackId(m_loadedTrackRowId);
    m_pTrackDelegate->setRowHeight(kTrackRowHeight);
    // What is loaded right now, so a compatible key can be green.
    m_pTrackDelegate->setPlayingKeyId(
            static_cast<int>(ControlObject::get(ConfigKey(kDeckGroup, "key"))));
}

void WDeckBrowser::runSearch() {
    if (m_stack.isEmpty()) {
        return;
    }
    const Level& level = m_stack.last();
    QSqlDatabase db = database();
    m_pTrackModel->setQuery(query::search(db, level.medium, m_searchText));
    refreshTrackColumns();
    applySort();
    m_pSearchQuery->setText(m_searchText.isEmpty()
                    ? tr("SEARCH")
                    : QStringLiteral("%1_        %2 hits")
                              .arg(m_searchText)
                              .arg(m_pTrackModel->rowCount()));
    if (m_pTrackModel->rowCount() > 0) {
        m_pTrackView->selectRow(0);
    }
}

void WDeckBrowser::onSelectionMoved(int row) {
    Q_UNUSED(row);
    updateInfoPanel();
    if (inTrackList()) {
        m_prefetchDwell.start();
    }
}

MediumId WDeckBrowser::currentMedium() const {
    return m_stack.isEmpty() ? MediumId() : m_stack.last().medium;
}

void WDeckBrowser::applySort() {
    // Publish it for the mapping first, so the LED is right even on the paths
    // below that return early.
    int fieldIndex = 0;
    const QList<WDeckSortMenu::Field>& fields = WDeckSortMenu::fields();
    for (int i = 0; i < fields.size(); ++i) {
        if (fields.at(i).column == m_sortColumn && !m_sortColumn.isEmpty()) {
            fieldIndex = i;
            break;
        }
    }
    m_pSortColumnControl->forceSet(fieldIndex);
    m_pSortOrderControl->forceSet(m_sortDescending ? 1.0 : 0.0);

    if (m_sortColumn.isEmpty()) {
        // Not "sort by some natural column": there is no column id meaning
        // unsorted, and only the model can put itself back.
        m_pTrackModel->clearSorting();
        m_pTrackDelegate->setSecondaryColumn(-1);
        return;
    }
    const int column = m_pTrackModel->fieldIndex(m_sortColumn);
    if (column < 0) {
        // The medium has no such field -- no labels, no dates. Fall back to
        // Default for THIS list without forgetting the preference, so the next
        // medium that does have it still sorts.
        m_pTrackModel->clearSorting();
        m_pTrackDelegate->setSecondaryColumn(-1);
        return;
    }
    m_pTrackModel->setSort(column, m_sortDescending ? Qt::DescendingOrder : Qt::AscendingOrder);
    m_pTrackModel->select();
    // The info layout shows the sorted-by value beside each title -- but not
    // always the sorted-by COLUMN. Key sorts on camelot_order, an integer
    // nobody wants to read, and shows the key text instead.
    QString displayColumn = m_sortColumn;
    for (const WDeckSortMenu::Field& field : WDeckSortMenu::fields()) {
        if (field.column == m_sortColumn && !field.displayColumn.isEmpty()) {
            displayColumn = field.displayColumn;
            break;
        }
    }
    const int displayIndex = m_pTrackModel->fieldIndex(displayColumn);
    m_pTrackDelegate->setSecondaryColumn(displayIndex >= 0 ? displayIndex : column);
}

void WDeckBrowser::onBreadcrumbClicked(const QString& levelIndex) {
    bool ok = false;
    const int target = levelIndex.toInt(&ok);
    if (!ok || target < 0 || target >= m_stack.size() - 1) {
        return; // The last segment is where we already are.
    }
    while (m_stack.size() > target + 1) {
        m_stack.removeLast();
    }
    rebuildCurrentLevel();
}

void WDeckBrowser::setInfoLayout(bool on) {
    m_infoLayout = on;
    m_pTrackDelegate->setInfoLayout(on);
    m_pInfoPanel->setVisible(on);
    if (on) {
        updateInfoPanel();
    }
    m_pTrackView->viewport()->update();
}

void WDeckBrowser::updateInfoPanel() {
    if (!m_infoLayout) {
        return;
    }
    const int row = m_pTrackView->selectedRow();
    if (row < 0) {
        m_pInfoPanel->clear();
        return;
    }
    const auto value = [this, row](const QString& column) {
        const int index = m_pTrackModel->fieldIndex(column);
        if (index < 0) {
            return QString();
        }
        return m_pTrackModel->index(row, index).data(Qt::DisplayRole).toString().trimmed();
    };

    // Whatever the list is sorted by is already beside every title on the left,
    // so it is dropped here rather than printed twice.
    QString sortedDisplay = m_sortColumn;
    for (const WDeckSortMenu::Field& field : WDeckSortMenu::fields()) {
        if (field.column == m_sortColumn && !field.displayColumn.isEmpty()) {
            sortedDisplay = field.displayColumn;
            break;
        }
    }

    QList<QPair<QString, QString>> fields;
    const auto add = [&](const QString& label, const QString& column) {
        if (column == sortedDisplay) {
            return;
        }
        const QString text = value(column);
        // Empty fields are omitted entirely: six populated rows read better
        // than eleven with five em-dashes in them.
        if (text.isEmpty() || text == QStringLiteral("0")) {
            return;
        }
        fields.append({label, text});
    };

    add(tr("Artist"), LIBRARYTABLE_ARTIST);
    add(tr("Album"), LIBRARYTABLE_ALBUM);
    add(tr("Year"), LIBRARYTABLE_YEAR);
    add(tr("Duration"), LIBRARYTABLE_DURATION);
    add(tr("Genre"), LIBRARYTABLE_GENRE);
    add(tr("Label"), QStringLiteral("label"));
    add(tr("Key"), LIBRARYTABLE_KEY);
    add(tr("Rating"), LIBRARYTABLE_RATING);
    add(tr("Added"), LIBRARYTABLE_DATETIMEADDED);
    // The play count rekordbox recorded, which is a different thing from
    // "Last played" -- that is a whole list of its own now, built from the
    // medium's history playlists merged with this deck's own log.
    add(tr("Plays"), LIBRARYTABLE_TIMESPLAYED);
    add(tr("Comment"), LIBRARYTABLE_COMMENT);

    m_pInfoPanel->setTrack(value(LIBRARYTABLE_COVERART_LOCATION), fields);
}

void WDeckBrowser::onPlayingKeyChanged() {
    // The marker moves with the loaded track, so it rides the same signal.
    m_pTrackDelegate->setLoadedTrackId(m_loadedTrackRowId);
    // Repaint, do not rebuild. The list has not changed -- only what the keys
    // in it should be compared against -- and rebuilding would lose the
    // selection and the scroll position mid-browse.
    m_pTrackDelegate->setPlayingKeyId(
            static_cast<int>(ControlObject::get(ConfigKey(kDeckGroup, "key"))));
    m_pTrackView->viewport()->update();
}

void WDeckBrowser::logPlay() {
    if (m_loadedTrackRowId < 0 || m_loadedTrackLogged) {
        return;
    }
    m_loadedTrackLogged = true;

    QSqlDatabase db = database();
    if (!db.isOpen()) {
        return;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT INTO deck_play_log (track_id, played_at) "
            "VALUES (:track_id, :played_at)"));
    query.bindValue(QStringLiteral(":track_id"), m_loadedTrackRowId);
    // Seconds since the epoch. Only ever compared against other rows in this
    // same table, and the table does not survive a boot, so the absolute value
    // never has to mean anything.
    query.bindValue(QStringLiteral(":played_at"),
            static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
}

void WDeckBrowser::onPlayPositionChanged(double position) {
    // Half way through is "played". Mixxx's own threshold for its play counter
    // is configurable and this is not it: what Last played answers is "did I
    // already drop this tonight", and a track a DJ pulled out after eight bars
    // is one they did not.
    if (position >= 0.5) {
        logPlay();
    }
}

void WDeckBrowser::onRateRangeChanged() {
    // A bucket is one fader's reach, so changing the range changes every
    // boundary. Rebuild in place rather than waiting for the level to be
    // re-entered.
    if (!m_stack.isEmpty() && m_stack.last().kind == Level::Kind::Categories &&
            m_stack.last().parameter == QStringLiteral("bpm")) {
        rebuildCurrentLevel();
    }
}

void WDeckBrowser::onSortChosen(const QString& column, bool descending) {
    m_sortColumn = column;
    m_sortDescending = descending;
    applySort();
    updateBreadcrumb();
    m_pTrackView->setFocus();
}

void WDeckBrowser::onSortDefault() {
    m_sortColumn.clear();
    m_sortDescending = false;
    applySort();
    updateBreadcrumb();
    m_pTrackView->setFocus();
}

void WDeckBrowser::onReselected(int row) {
    if (inTrackList()) {
        // A tap on the already-selected track opens the info layout, which is
        // the "tell me more" gesture. Loading is the long press.
        setInfoLayout(!m_infoLayout);
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

    // Play the COPY, never the medium. This is the whole point of the cache:
    // the file under the playhead has to be one that survives the stick being
    // pulled out of the deck.
    const int locationColumn = m_pTrackModel->fieldIndex(TRACKLOCATIONSTABLE_LOCATION);
    const QString source = locationColumn >= 0
            ? m_pTrackModel->index(row, locationColumn).data(Qt::DisplayRole).toString()
            : QString();
    QString playPath = source;
    if (m_pCache && !source.isEmpty()) {
        // Usually already there from the dwell prefetch; this is the slow path.
        const QString cached = m_pCache->ensureLocal(currentMedium(), source);
        if (!cached.isEmpty()) {
            playPath = cached;
            if (!m_pinnedPath.isEmpty()) {
                m_pCache->unpin(m_pinnedPath);
            }
            m_pCache->pin(cached);
            m_pinnedPath = cached;
        } else {
            // Could not copy it -- a remote medium whose audio has not been
            // fetched, or a read error. Falling through to the original path
            // keeps a local stick playable; a remote one simply will not load,
            // which is the state the streaming work exists to fix.
            kLogger.warning() << "not cached, playing from source:" << source;
        }
    }

    TrackPointer pTrack = playPath == source
            ? m_pTrackModel->getTrack(index)
            : m_pLibrary->trackCollectionManager()->getOrAddTrack(
                      TrackRef::fromFilePath(playPath));
    if (!pTrack) {
        kLogger.warning() << "no track at row" << row;
        return;
    }
    if (playPath != source) {
        // The cached file has no tags worth reading -- it is a byte copy of
        // somebody else's file -- so the metadata comes from the pdb, as it
        // does for the row itself.
        const auto field = [this, row](const QString& column) {
            const int c = m_pTrackModel->fieldIndex(column);
            return c < 0 ? QString()
                         : m_pTrackModel->index(row, c).data(Qt::DisplayRole).toString();
        };
        pTrack->setArtist(field(LIBRARYTABLE_ARTIST));
        pTrack->setTitle(field(LIBRARYTABLE_TITLE));
        pTrack->setAlbum(field(LIBRARYTABLE_ALBUM));
        // Genre is deliberately not set: Track has no plain setter for it (only
        // setGenreFromTrackDAO, which is the DAO's business), and the browser
        // reads genre off the pdb row anyway.
    }
    // Which deck_library row this is, kept for the play log. Taken here because
    // this is the only moment it is unambiguous: two sticks can hold clones of
    // the same file, so afterwards the path alone does not say which medium it
    // came from.
    const int idColumn = m_pTrackModel->fieldIndex(QStringLiteral("track_id"));
    m_loadedTrackRowId = idColumn >= 0
            ? m_pTrackModel->index(row, idColumn).data(Qt::DisplayRole).toInt()
            : -1;
    m_loadedTrackLogged = false;
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
            Level level;
            level.kind = Level::Kind::Diagnostics;
            level.title = menuRow.title;
            pushLevel(level);
            return;
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
        } else if (payload == QStringLiteral("search")) {
            level.kind = Level::Kind::Search;
            m_searchText.clear();
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
