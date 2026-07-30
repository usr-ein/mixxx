#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>

#include "library/baseexternallibraryfeature.h"
#include "library/prolink/prolinkpdbimport.h"
#include "library/treeitemmodel.h"
#include "network/prolink/prolinkdevice.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

#include "library/prolink/prolinkplaylistmodel.h"

class Library;
class BaseTrackCache;
class ControlPushButton;
class ControlObject;
class WLibraryTextBrowser;

namespace mixxx {
namespace prolink {
class ProLinkNetworkService;
}
} // namespace mixxx

/// Lists the Pioneer players on the network in the library sidebar.
///
/// **This is the discovery half only.** Devices appear, go grey when they stop
/// keep-aliving and disappear when they are gone for good; nothing can be
/// browsed or loaded yet. That is deliberate rather than unfinished: it is the
/// smallest thing that can be verified against real hardware, and it establishes
/// the thread boundary, the tree bookkeeping and the shutdown ordering that the
/// browsing work then builds on.
///
/// Everything here runs on the GUI thread. The network layer lives behind
/// `ProLinkNetworkService` on its own thread and reaches us only through queued
/// signals carrying value types.
class ProLinkFeature : public BaseExternalLibraryFeature {
    Q_OBJECT

  public:
    ProLinkFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~ProLinkFeature() override;

    QVariant title() override;
    TreeItemModel* sidebarModel() const override;
    void bindLibraryWidget(WLibrary* pLibraryWidget,
            KeyboardEventFilter* pKeyboard) override;

    static bool isSupported() {
        return true;
    }

  protected:
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;
    void appendTrackIdsFromRightClickIndex(QList<TrackId>* pTrackIds,
            QString* pPlaylist) override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;

  private slots:
    void onDatabaseFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QByteArray& data,
            const QString& error);
    void onDeviceFound(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void onDeviceLost(const QByteArray& mac);
    void onListeningChanged(bool listening, const QString& error);
    void onRefresh(double value);

  private:
    /// Row of the device with this MAC, or -1. Linear, which is fine: a Pro DJ
    /// Link network holds at most a handful of devices.
    int rowForMac(const QByteArray& mac) const;
    void showStatusPage();
    /// Re-render the status page in place.
    ///
    /// Needed because bindLibraryWidget() runs once, while the library widget is
    /// being built -- long before any player has announced itself. Setting the
    /// HTML only there would leave the page permanently reading "No players
    /// found yet", which is exactly the state it is supposed to report the end
    /// of.
    void refreshStatusPage();
    QString statusHtml() const;

    /// State of one (device, slot) pair.
    ///
    /// Keyed on `mac|slot` rather than held on the TreeItem, so a device that
    /// blips offline and returns keeps its parsed library: the tree node may be
    /// rebuilt, this is not. It is also what stops a second click while a fetch
    /// is in flight from starting a second fetch.
    struct Medium {
        enum class State {
            Unknown,  ///< Never opened. We cannot know if a slot has media
                      ///< without announcing, so both are always offered.
            Fetching,
            Ready,
            Failed,
        };
        State state = State::Unknown;
        QString error;
        mixxx::prolink::PdbContents contents;
    };
    static QString mediumKey(const QByteArray& mac, mixxx::prolink::MediaSlot slot);
    /// Add the USB and SD children under a device row.
    void addSlotNodes(int deviceRow, const QByteArray& mac);
    /// Splice a medium's playlists under its slot node.
    void showPlaylists(const QByteArray& mac, mixxx::prolink::MediaSlot slot);
    int rowForSlot(int deviceRow, mixxx::prolink::MediaSlot slot) const;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<ProLinkPlaylistModel> m_pPlaylistModel;
    QSharedPointer<BaseTrackCache> m_trackSource;
    std::unique_ptr<mixxx::prolink::ProLinkNetworkService> m_pNetwork;

    std::unique_ptr<ControlPushButton> m_pRefreshControl;
    /// Read-only, so a skin or a controller mapping can show a link indicator
    /// without any new plumbing.
    std::unique_ptr<ControlObject> m_pDeviceCountControl;

    /// The registered view, so its contents can be replaced as devices come and
    /// go. QPointer because WLibrary owns it and may outlive or predecease us
    /// depending on teardown order.
    QPointer<WLibraryTextBrowser> m_pStatusView;

    QHash<QString, Medium> m_media;

    bool m_listening = false;
    QString m_error;
};
