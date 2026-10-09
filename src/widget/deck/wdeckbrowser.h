#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>
#include <QList>
#include <QSet>
#include <QWidget>
#include <memory>

#include "control/controlproxy.h"
#include "library/deck/deckloader.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/previewwaveformcache.h"
#include "library/deck/mediumid.h"
#include "preferences/usersettings.h"
#include "skin/legacy/skincontext.h"
#include "track/trackid.h"
#include "widget/wbasewidget.h"

class ControlEncoder;
class ControlObject;
class ControlPushButton;
class Library;
class BaseTrackCache;
class QStackedWidget;

namespace mixxx {
namespace deck {

class DeckAutoplay;
class DeckListView;
class DeckServices;
class DeckMenuModel;
class DeckTrackModel;
class MenuRowDelegate;
struct MenuRow;
class WDeckSortMenu;
class WDeckKeyboard;
class WDeckInfoPanel;
class WDeckDiagnostics;
class DeckPage;
class TrackRowDelegate;

/// The `▲ Album` in the breadcrumb: what the list is sorted by, and the control
/// for it.
///
/// A widget of its own rather than a link inside the breadcrumb's rich text,
/// and that is what buys the second gesture: a QLabel anchor knows it was
/// clicked and nothing more, so there was no way to ask whether a press that
/// was still being held was on the indicator or on `SAM1 › Genre`. A widget is
/// asked that by being pressed.
///
/// Tap flips the direction; hold opens the sort menu — the same short/long
/// split the SORT pad has, in the place the sort is displayed.
class DeckSortChip : public QLabel {
    Q_OBJECT

  public:
    explicit DeckSortChip(QWidget* pParent = nullptr);

  signals:
    void tapped();
    void held();

  protected:
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;

  private:
    QTimer m_longPressTimer;
    /// The hold already fired, so the release that ends it is not also a tap.
    bool m_consumed = false;
};

/// The deck's library, as a menu stack.
///
/// Replaces the sidebar-and-table entirely (browser-prd.md 1). One column, one
/// selection, drill in and out: level 0 is the sources, level 1 a medium's
/// categories, and below that whatever that category leads to, ending in a
/// track list.
///
/// **Navigation is a stack, not a tree.** Each level records what it was
/// showing and which row was selected, so BACK restores the level below exactly
/// as it was left rather than rebuilding it from scratch and losing the place.
class WDeckBrowser : public QWidget, public WBaseWidget {
    Q_OBJECT

  public:
    /// *pServices* are the deck's services it browses, loads and shows
    /// through; started, and used and not owned.
    WDeckBrowser(QWidget* pParent,
            Library* pLibrary,
            UserSettingsPointer pConfig,
            DeckServices* pServices);
    ~WDeckBrowser() override;

    void setup(const QDomNode& node, const SkinContext& context);

  public slots:
    /// The deck has nothing on it any more -- ejected, or a track that would
    /// not load. Takes the loaded marker off the row it was on.
    void onDeckEmpty();

  private slots:
    void onMediaChanged();
    /// A medium's rows changed in place -- a folder stick's tags arriving.
    /// The rows are re-read at once; the list on screen is re-selected later,
    /// coalesced, and not while the DJ is turning the encoder.
    void onMediumUpdated(const QString& mediumKey, const QList<quint32>& rbIds);
    /// The coalesced half of onMediumUpdated(): re-read what is on screen.
    void refreshInPlace();
    void onActivated(int row);
    void onReselected(int row);
    void onBack();
    void onSelectionMoved(int row);
    void onSortChosen(const QString& column, bool descending);
    void onSortDefault();
    /// A track was loaded or unloaded: repaint the key column against the new
    /// reference, without rebuilding anything.
    void onPlayingKeyChanged();
    /// The tempo fader's range changed: the BPM buckets are a different size
    /// now, so rebuild that level in place if it is the one on screen.
    void onRateRangeChanged();
    /// A breadcrumb segment was clicked: pop back to that level.
    void onBreadcrumbClicked(const QString& levelIndex);
    /// The sort indicator was tapped: the other way round.
    void onSortFlipped();
    /// The sort indicator was held: the field menu, as the SORT pad raises it.
    void openSortMenu();

  private:
    /// What a level is showing. The payload is what rebuilding it needs.
    struct Level {
        enum class Kind {
            Sources,
            MediumMenu,
            Playlists,
            Categories, ///< A value list: genres, albums, labels, keys, dates, BPM.
            Artists,
            ArtistAlbums,
            Tracks,
            Search,
            Diagnostics,
            /// Autoplay's own first level: stop it, or pick a drive.
            Autoplay,
        };
        Kind kind = Kind::Sources;
        MediumId medium;
        QString title;
        /// Category column for Kind::Categories, artist for Kind::ArtistAlbums,
        /// parent playlist rb_id for Kind::Playlists.
        QString parameter;
        int parentRbId = 0;
        int selectedRow = 0;
        /// Below Autoplay: a drive's genres, and a genre's tracks, which start
        /// autoplay rather than load. The same levels as the medium menu's
        /// Genre otherwise, so they look and sort the same.
        bool autoplay = false;
        /// The genre, for an autoplay track list: what autoplay is started on.
        /// Empty is the "—" row, the tracks with no genre.
        QString genre;
    };

    void pushLevel(Level level);
    void popLevel();
    void rebuildCurrentLevel();
    void showSources();
    /// One medium as a source row: its mark, name and state, and its counts.
    MenuRow sourceRow(const MediumInfo& medium) const;
    void showAutoplay(const Level& level);
    void showMediumMenu(const Level& level);
    void showPlaylists(const Level& level);
    void showCategory(const Level& level);
    void showArtistAlbums(const Level& level);
    void showTracks(const Level& level, const QString& selectSql);
    void showSearch(const Level& level);
    void runSearch();
    void updateInfoPanel();
    void setInfoLayout(bool on);
    /// Resolve the track delegate's column indices for the current model.
    void refreshTrackColumns();
    /// The page currently on the stack, if it wants the deck's controls.
    mixxx::deck::DeckPage* currentPage() const;
    void updateBreadcrumb();
    /// The DJ's load: the selected row, onto the deck, not playing. Ends
    /// autoplay.
    void loadSelectedTrack();
    /// Everything a load needs off the list's row *row*, read in one go.
    DeckLoader::LoadableRow readModelRow(int row) const;
    /// The selected track of an autoplay track list starts autoplay.
    void startAutoplay();
    /// Autoplay went on or off: the rows that say so are redrawn.
    void onAutoplayChanged();
    /// Put the selection back after the model has been re-selected, on the same
    /// track if it is still in the list.
    void restoreSelection(int trackId);
    /// True while a track list is on screen, which is what SORT and the info
    /// layout are conditional on.
    bool inTrackList() const;
    QSqlDatabase database() const;

    Library* m_pLibrary;
    UserSettingsPointer m_pConfig;
    /// This deck's accent (deckaccent.h), for the breadcrumb's rich text.
    const QColor m_accent;
    MediaRegistry* const m_pRegistry;
    DeckLoader* const m_pLoader;
    DeckAutoplay* const m_pAutoplay;

    QLabel* m_pBreadcrumb;
    DeckSortChip* m_pSortChip;
    QStackedWidget* m_pStack;
    DeckListView* m_pMenuView;
    DeckListView* m_pTrackView;
    DeckMenuModel* m_pMenuModel;
    DeckTrackModel* m_pTrackModel;
    MenuRowDelegate* m_pMenuDelegate;
    TrackRowDelegate* m_pTrackDelegate;
    QSharedPointer<BaseTrackCache> m_trackSource;

    QList<Level> m_stack;

    WDeckSortMenu* m_pSortMenu;
    /// The strip kept clear for the panel's top lip: 36 px, or 0 on a flush
    /// panel (deckbezel.h). The sort menu opens just under the breadcrumb, so
    /// it needs to know.
    int m_topBezelPadHeight = 0;
    /// The search screen: a query line, the results in the track view, and the
    /// keyboard under them.
    QWidget* m_pSearchPage;
    QLabel* m_pSearchQuery;
    WDeckKeyboard* m_pKeyboard;
    QWidget* m_pSearchResults;
    /// The track list and its info panel side by side. The panel is hidden in
    /// the default layout, which is why the two need a container of their own
    /// rather than sitting in the stack directly.
    QWidget* m_pTracksPage;
    WDeckInfoPanel* m_pInfoPanel;
    WDeckDiagnostics* m_pDiagnostics;
    bool m_infoLayout = false;
    /// Preview waveforms for the info panel, read off the GUI thread.
    std::unique_ptr<PreviewWaveformCache> m_pPreviews;
    /// Which track the panel is currently showing, so a preview arriving late
    /// is only drawn if it is still the one being looked at.
    MediumId m_previewMedium;
    quint32 m_previewTrackId = 0;
    QString m_searchText;
    /// The sort is a BROWSER preference, not a property of a list: leaving one
    /// list and opening another applies the same sort to the new one, until
    /// Default is chosen (browser-prd.md 9.3). Empty means Default.
    QString m_sortColumn;
    bool m_sortDescending = false;
    /// Applied to whatever list is on screen, and re-applied whenever another
    /// one opens.
    ///
    /// *keepSelection* puts the selection back on the track it was on, which is
    /// what a DJ wants from every sort except a reversal — see the body.
    void applySort(bool keepSelection = true);

    /// Watched so the UI reacts to the deck rather than to being redrawn.
    std::unique_ptr<ControlProxy> m_pPlayingKey;
    std::unique_ptr<ControlProxy> m_pTrackLoaded;
    std::unique_ptr<ControlProxy> m_pRateRange;
    /// The medium the current list came from, for cache keys. Levels below a
    /// medium all carry it, so this is just the stack's.
    MediumId currentMedium() const;

    /// Covers arrive in a burst as a list scrolls; this coalesces the redraws
    /// into one rather than repainting per image.
    QTimer m_coverRedraw;

    /// The medium whose rows changed in place since the last refresh, and the
    /// timer that coalesces those changes into one re-read of the screen.
    QString m_staleMediumKey;
    QTimer m_inPlaceRefresh;
    /// Rows written behind the track cache's back -- tags, a BPM -- re-read
    /// into it. It keeps a copy of every row it has shown and refreshes none
    /// of them by itself, so a re-select alone shows the old values.
    void refreshCachedRows(const QSet<TrackId>& rows);
    /// When the selection last moved. A refresh re-reads the list under the
    /// selection, so it waits until the DJ has stopped scrolling.
    QElapsedTimer m_lastSelectionMove;

    // The deck's controls. Rotate, push, back, and the two SORT meanings.
    std::unique_ptr<ControlEncoder> m_pMove;
    std::unique_ptr<ControlPushButton> m_pSelect;
    std::unique_ptr<ControlPushButton> m_pBack;
    std::unique_ptr<ControlPushButton> m_pSortMenuControl;
    std::unique_ptr<ControlPushButton> m_pInfoToggle;
    /// Read-only, so the mapping can light the SORT pad only where it does
    /// something.
    std::unique_ptr<ControlObject> m_pLevelControl;
    std::unique_ptr<ControlObject> m_pInTrackList;
    /// The sort in force, as numbers, so the mapping can light the SORT pad
    /// without knowing anything about column names: an index into
    /// WDeckSortMenu::fields() (0 = Default) and 0/1 for the direction.
    std::unique_ptr<ControlObject> m_pSortColumnControl;
    std::unique_ptr<ControlObject> m_pSortOrderControl;
};

} // namespace deck
} // namespace mixxx
