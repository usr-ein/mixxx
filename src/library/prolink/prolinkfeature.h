#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>

#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "network/prolink/prolinkdevice.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class Library;
class ControlPushButton;
class ControlObject;

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
class ProLinkFeature : public LibraryFeature {
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

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;

  private slots:
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
    QString statusHtml() const;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    std::unique_ptr<mixxx::prolink::ProLinkNetworkService> m_pNetwork;

    std::unique_ptr<ControlPushButton> m_pRefreshControl;
    /// Read-only, so a skin or a controller mapping can show a link indicator
    /// without any new plumbing.
    std::unique_ptr<ControlObject> m_pDeviceCountControl;

    bool m_listening = false;
    QString m_error;
};
