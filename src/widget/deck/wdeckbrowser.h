#pragma once

#include <QLabel>
#include <QList>
#include <QWidget>
#include <memory>

#include "control/controlproxy.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/mediumid.h"
#include "preferences/usersettings.h"
#include "skin/legacy/skincontext.h"
#include "track/track_decl.h"
#include "widget/wbasewidget.h"

class ControlEncoder;
class ControlObject;
class ControlPushButton;
class Library;
class BaseTrackCache;
class QStackedWidget;

namespace mixxx {
namespace deck {

class DeckListView;
class DeckMenuModel;
class DeckTrackModel;
class MenuRowDelegate;
class WDeckSortMenu;
class WDeckKeyboard;
class WDeckInfoPanel;
class TrackRowDelegate;

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
    WDeckBrowser(QWidget* pParent, Library* pLibrary, UserSettingsPointer pConfig);
    ~WDeckBrowser() override;

    void setup(const QDomNode& node, const SkinContext& context);

  signals:
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void onMediaChanged();
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
    /// The deck got far enough into the loaded track to call it played.
    void onPlayPositionChanged(double position);
    /// A breadcrumb segment was clicked: pop back to that level.
    void onBreadcrumbClicked(const QString& levelIndex);

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
        };
        Kind kind = Kind::Sources;
        MediumId medium;
        QString title;
        /// Category column for Kind::Categories, artist for Kind::ArtistAlbums,
        /// parent playlist rb_id for Kind::Playlists.
        QString parameter;
        int parentRbId = 0;
        int selectedRow = 0;
    };

    void pushLevel(Level level);
    void popLevel();
    void rebuildCurrentLevel();
    void showSources();
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
    void updateBreadcrumb();
    void loadSelectedTrack();
    /// True while a track list is on screen, which is what SORT and the info
    /// layout are conditional on.
    bool inTrackList() const;
    QSqlDatabase database() const;

    Library* m_pLibrary;
    UserSettingsPointer m_pConfig;
    std::unique_ptr<MediaRegistry> m_pRegistry;

    QLabel* m_pBreadcrumb;
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
    bool m_infoLayout = false;
    QString m_searchText;
    /// The sort is a BROWSER preference, not a property of a list: leaving one
    /// list and opening another applies the same sort to the new one, until
    /// Default is chosen (browser-prd.md 9.3). Empty means Default.
    QString m_sortColumn;
    bool m_sortDescending = false;
    /// Applied to whatever list is on screen, and re-applied whenever another
    /// one opens.
    void applySort();

    /// Watched so the UI reacts to the deck rather than to being redrawn.
    std::unique_ptr<ControlProxy> m_pPlayingKey;
    std::unique_ptr<ControlProxy> m_pTrackLoaded;
    std::unique_ptr<ControlProxy> m_pRateRange;
    std::unique_ptr<ControlProxy> m_pPlayPosition;
    /// The deck_library row currently on the deck, and whether it has already
    /// been logged. Remembered at load rather than matched back from the Track:
    /// two media can hold clones of the same file, so the path does not
    /// identify the row and the medium would have to be guessed.
    int m_loadedTrackRowId = -1;
    bool m_loadedTrackLogged = false;
    void logPlay();

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
};

} // namespace deck
} // namespace mixxx
