// This feature reads tracks, playlists and folders from removable Recordbox
// prepared devices (USB drives, etc), by parsing the binary *.PDB files
// stored on each removable device. It does not read the locally stored
// Rekordbox database (Collection).

// It draws heavily from the hard work completed here:

//      https://github.com/Deep-Symmetry/crate-digger

// And uses the C++ Kaitai Struct binary parsing libraries:

//      http://kaitai.io
//      https://github.com/kaitai-io/kaitai_struct
//      https://github.com/kaitai-io/kaitai_struct_cpp_stl_runtime

// The *.PDB C++ files:

//      rekordbox_pdb.h
//      rekordbox_pdb.cpp

// Were generated from the following structure definition file:

//      https://github.com/Deep-Symmetry/crate-digger/blob/master/src/main/kaitai/rekordbox_pdb.ksy

#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QStringListModel>
#include <QtConcurrentRun>
#include <fstream>
#include <memory>

#include "library/baseexternallibraryfeature.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "util/parented_ptr.h"

class TrackCollectionManager;
class BaseExternalPlaylistModel;
class ControlPushButton;

/// What a worker thread produces from one device's `export.pdb`.
///
/// `pStagingRoot` holds the playlist/folder subtree, and is deliberately *not*
/// the device's own TreeItem: that one is owned by the live TreeItemModel, and
/// a model's items may only be mutated on the GUI thread, bracketed by
/// begin/endInsertRows(). So the worker builds under a detached staging item
/// and RekordboxFeature::onTracksFound() splices the result in.
class WLibraryTextBrowser;

struct RekordboxDeviceParseResult {
    /// Label of the device this came from, used to find its row again -- the
    /// device may well have been unmounted while we were parsing.
    QString device;
    /// The device's "all tracks" playlist, or empty if the parse failed.
    QString devicePlaylist;
    /// What the device holds, for the summary shown when the device node itself
    /// is selected. Counted during the parse, where the maps are already to
    /// hand, rather than by querying the table again afterwards.
    int trackCount = 0;
    int artistCount = 0;
    int albumCount = 0;
    /// Shared rather than unique so the result stays copyable, which is what
    /// QFuture wants, and so an unconsumed result still frees its subtree.
    std::shared_ptr<TreeItem> pStagingRoot;
};

class RekordboxPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    RekordboxPlaylistModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

  protected:
    void initSortColumnMapping() override;
};

class RekordboxFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    RekordboxFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~RekordboxFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    /// Rescan the attached devices without touching the library view. Bound to
    /// the `[Rekordbox], refresh` control so controller scripts can pick up a
    /// freshly mounted USB drive.
    void refreshDevices();
    void refreshLibraryModels();
    void onRekordboxDevicesFound();
    void onTracksFound();

  private slots:
    void htmlLinkClicked(const QUrl& link);

  private:
    QString formatRootViewHtml() const;
    /// The summary page for one device: its name and what is on it.
    QString formatDeviceViewHtml(const QString& device) const;
    /// Show that summary, or the root page if the device is unknown.
    void showDeviceView(const QString& device);
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    /// Device label -> its counts, for the summary page. Populated as each
    /// device is parsed and kept until it is unmounted.
    QHash<QString, RekordboxDeviceParseResult> m_deviceSummaries;
    /// The view the summary is drawn into, shared with the root page.
    QPointer<WLibraryTextBrowser> m_pTextBrowser;
    parented_ptr<RekordboxPlaylistModel> m_pRekordboxPlaylistModel;

    QFutureWatcher<QList<TreeItem*>> m_devicesFutureWatcher;
    QFuture<QList<TreeItem*>> m_devicesFuture;
    QFutureWatcher<RekordboxDeviceParseResult> m_tracksFutureWatcher;
    QFuture<RekordboxDeviceParseResult> m_tracksFuture;
    QString m_title;
    std::unique_ptr<ControlPushButton> m_pRefreshControl;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
