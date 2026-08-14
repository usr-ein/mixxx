#pragma once

#include <QLabel>
#include <QTimer>
#include <QList>
#include <QWidget>
#include <memory>

#include "control/controlproxy.h"
#include "library/coverart.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/previewwaveformcache.h"
#include "library/deck/trackcache.h"
#include "library/deck/mediumid.h"
#include "preferences/usersettings.h"
#include "skin/legacy/skincontext.h"
#include "track/track_decl.h"
#include "widget/deck/deckencoder.h"
#include "widget/wbasewidget.h"

class ControlEncoder;
class ControlObject;
class ControlPushButton;
class Library;
class BaseTrackCache;
class QStackedWidget;
class EffectsManager;

namespace mixxx {
namespace deck {

class DeckListView;
class DeckMenuModel;
class DeckTrackModel;
class MenuRowDelegate;
class WDeckSortMenu;
class WDeckKeyboard;
class WDeckInfoPanel;
class WDeckDiagnostics;
class WDeckRack;
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
class WDeckBrowser : public QWidget, public WBaseWidget, public DeckEncoder::Target {
    Q_OBJECT

  public:
    WDeckBrowser(QWidget* pParent,
            Library* pLibrary,
            UserSettingsPointer pConfig,
            EffectsManager* pEffectsManager);
    ~WDeckBrowser() override;

    void setup(const QDomNode& node, const SkinContext& context);

    // DeckEncoder::Target. Which of the browser's own screens is showing --
    // a menu, a track list, the Effects page -- is decided below, not by the
    // router: the router only knows that the library is up.
    void encoderMove(int steps) override;
    void encoderPress() override;
    void encoderResetFocus() override;

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
            Effects,
            Diagnostics,
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

    void finishControlSetup();
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
    /// The page currently on the stack, if it wants the deck's controls.
    mixxx::deck::DeckPage* currentPage() const;
    void updateBreadcrumb();
    void loadSelectedTrack();
    /// Point a Track at the cover its medium carries, so the deck's header
    /// draws it.
    ///
    /// Nothing else does: a rekordbox medium keeps its art under `PIONEER/`,
    /// nowhere near the audio file, and the deck plays from a byte copy in the
    /// cache anyway -- so every way Mixxx has of finding a cover on its own
    /// (embedded tags, an image beside the file) comes back with nothing and
    /// the header falls back to the empty square. The path is in the pdb, and
    /// the browser row already has it.
    ///
    /// Taken by value because the caller may be handing over the pending-cover
    /// members, which this clears.
    void applyCoverArt(const TrackPointer& pTrack,
            QString coverPath,
            QString artworkPath);
    /// Put the medium's cover back if something guesses over it.
    ///
    /// `TrackDAO::getOrAddTrack()` fires `guessTrackCoverInfoConcurrently()` on
    /// a worker for every track it adds to the library for the first time. That
    /// worker looks for an image beside the audio file — which for this deck is
    /// the byte-copy cache, and holds nothing but other tracks — finishes a few
    /// milliseconds after the load has returned, and writes `CoverInfo::NONE`
    /// over the path taken out of the pdb.
    ///
    /// It is a race and it cannot be won by ordering: the guess is already
    /// running when `getOrAddTrack()` returns. So the cover is *defended*
    /// instead, once, and the guess loses the rematch.
    ///
    /// This is the whole of "the artwork appears on the second load and never
    /// the first": the second load finds the track already in the library, so
    /// no guess is fired and nothing overwrites anything.
    void guardCoverArt(const TrackPointer& pTrack, const CoverInfoRelative& cover);
    /// Put the selection back after the model has been re-selected, on the same
    /// track if it is still in the list.
    void restoreSelection(int trackId);
    /// True while a track list is on screen, which is what SORT and the info
    /// layout are conditional on.
    bool inTrackList() const;
    QSqlDatabase database() const;

    Library* m_pLibrary;
    UserSettingsPointer m_pConfig;
    std::unique_ptr<MediaRegistry> m_pRegistry;

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
    /// The effect rack. Its status read-out moved to Diagnostics; what is left
    /// here is the instrument.
    WDeckRack* m_pRack;
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
    std::unique_ptr<ControlProxy> m_pPlayPosition;
    /// The deck_library row currently on the deck, and whether it has already
    /// been logged. Remembered at load rather than matched back from the Track:
    /// two media can hold clones of the same file, so the path does not
    /// identify the row and the medium would have to be guessed.
    int m_loadedTrackRowId = -1;
    bool m_loadedTrackLogged = false;
    void logPlay();
    /// The medium the current list came from, for cache keys. Levels below a
    /// medium all carry it, so this is just the stack's.
    MediumId currentMedium() const;

    std::unique_ptr<TrackCache> m_pCache;
    /// Fires after the selection has sat still long enough to mean something.
    QTimer m_prefetchDwell;
    /// Covers arrive in a burst as a list scrolls; this coalesces the redraws
    /// into one rather than repainting per image.
    QTimer m_coverRedraw;
    /// The cached file the deck is playing, so it can be unpinned when another
    /// takes its place.
    QString m_pinnedPath;
    /// A cover the track on the deck is still waiting for, and the track that
    /// wants it. Only ever set for a remote medium: its images come over the
    /// network one at a time, as they are looked at, so a track can reach the
    /// deck before its own cover does.
    ///
    /// **Weak on purpose.** The deck owns what it is playing; holding a
    /// TrackPointer here would keep the last one alive in GlobalTrackCache
    /// until the next load, waveform and all, for a cover that may never come.
    QString m_pendingCoverPath;
    QString m_pendingArtworkPath;
    TrackWeakPointer m_pendingCoverTrack;
    /// Armed by guardCoverArt() for the track on the deck, and only that one.
    QMetaObject::Connection m_coverGuard;

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
