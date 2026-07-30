#include "library/prolink/prolinkfeature.h"

#include <QMenu>
#include <QUrl>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/library.h"
#include "library/treeitem.h"
#include "moc_prolinkfeature.cpp"
#include "network/prolink/prolinknetworkservice.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

using mixxx::prolink::ProLinkDevice;
using mixxx::prolink::ProLinkNetworkService;

namespace {
const QString kConfigGroup = QStringLiteral("[ProLink]");
const QString kViewName = QStringLiteral("PROLINKHOME");

/// Index into a device node's payload list.
///
/// A `QList<QString>` payload follows the convention the Rekordbox feature
/// established (`rekordboxfeature.cpp:196`), but with an explicit kind tag
/// rather than that feature's two magic sentinel strings, so adding slot and
/// playlist nodes later does not mean overloading a boolean.
constexpr int kPayloadKind = 0;
constexpr int kPayloadMac = 1;

const QString kKindDevice = QStringLiteral("device");

QVariant deviceNodePayload(const ProLinkDevice& device) {
    return QVariant(QList<QString>{kKindDevice, QString::fromLatin1(device.mac.toHex())});
}
} // namespace

ProLinkFeature::ProLinkFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("prolink")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pNetwork(std::make_unique<ProLinkNetworkService>()),
          m_pRefreshControl(std::make_unique<ControlPushButton>(
                  ConfigKey(kConfigGroup, QStringLiteral("refresh")))),
          m_pDeviceCountControl(std::make_unique<ControlObject>(
                  ConfigKey(kConfigGroup, QStringLiteral("device_count")))) {
    m_pDeviceCountControl->setReadOnly();

    connect(m_pRefreshControl.get(),
            &ControlPushButton::valueChanged,
            this,
            &ProLinkFeature::onRefresh);

    connect(m_pNetwork.get(),
            &ProLinkNetworkService::deviceFound,
            this,
            &ProLinkFeature::onDeviceFound);
    connect(m_pNetwork.get(),
            &ProLinkNetworkService::deviceChanged,
            this,
            &ProLinkFeature::onDeviceChanged);
    connect(m_pNetwork.get(),
            &ProLinkNetworkService::deviceLost,
            this,
            &ProLinkFeature::onDeviceLost);
    connect(m_pNetwork.get(),
            &ProLinkNetworkService::listeningChanged,
            this,
            &ProLinkFeature::onListeningChanged);

    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    m_pNetwork->start();
}

ProLinkFeature::~ProLinkFeature() {
    // Order matters and is the whole point of doing this here rather than
    // letting the members fall out of scope. shutdown() joins the network
    // thread, so afterwards no queued signal can still be in flight towards a
    // half-destroyed feature -- a crash that only reproduces with a CDJ actually
    // on the network, and therefore never in a unit test.
    m_pNetwork->shutdown();
}

QVariant ProLinkFeature::title() {
    return tr("Pro DJ Link");
}

TreeItemModel* ProLinkFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void ProLinkFeature::activate() {
    showStatusPage();
}

void ProLinkFeature::activateChild(const QModelIndex& index) {
    Q_UNUSED(index);
    // Browsing a device is not implemented yet. Showing the status page is a
    // deliberate placeholder: on a CDJ an error and an empty folder look
    // identical, and the same is true here -- a click that appears to do nothing
    // is indistinguishable from a bug.
    showStatusPage();
}

void ProLinkFeature::showStatusPage() {
    emit switchToView(kViewName);
    emit disableSearch();
}

int ProLinkFeature::rowForMac(const QByteArray& mac) const {
    TreeItem* pRoot = m_pSidebarModel->getRootItem();
    const QString wanted = QString::fromLatin1(mac.toHex());
    for (int row = 0; row < pRoot->childRows(); ++row) {
        const QList<QString> payload = pRoot->child(row)->getData().toStringList();
        if (payload.size() > kPayloadMac && payload.at(kPayloadMac) == wanted) {
            return row;
        }
    }
    return -1;
}

void ProLinkFeature::onDeviceFound(const ProLinkDevice& device) {
    if (rowForMac(device.mac) >= 0) {
        onDeviceChanged(device);
        return;
    }

    // Appended rather than inserted at 0, so a user part-way through inspecting
    // player 1 is not yanked sideways when player 2 powers on.
    std::vector<std::unique_ptr<TreeItem>> rows;
    auto pItem = std::make_unique<TreeItem>(device.label(), deviceNodePayload(device));
    pItem->setBold(true);
    rows.push_back(std::move(pItem));
    m_pSidebarModel->insertTreeItemRows(
            std::move(rows), m_pSidebarModel->getRootItem()->childRows());

    m_pDeviceCountControl->forceSet(m_pNetwork->deviceCount());
}

void ProLinkFeature::onDeviceChanged(const ProLinkDevice& device) {
    const int row = rowForMac(device.mac);
    if (row < 0) {
        onDeviceFound(device);
        return;
    }
    TreeItem* pItem = m_pSidebarModel->getRootItem()->child(row);

    // Offline devices are relabelled and unbolded, never removed here. Their
    // subtree and (later) their cached database will stay put through a blip,
    // because a player can drop off the network for two seconds and come
    // straight back -- see kDeviceTimeoutMs.
    const bool offline = device.isStale();
    pItem->setLabel(offline ? tr("%1 (offline)").arg(device.label()) : device.label());
    pItem->setBold(!offline);
    m_pSidebarModel->triggerRepaint(m_pSidebarModel->index(row, 0));
}

void ProLinkFeature::onDeviceLost(const QByteArray& mac) {
    const int row = rowForMac(mac);
    if (row < 0) {
        return;
    }
    m_pSidebarModel->removeRows(row, 1);
    m_pDeviceCountControl->forceSet(m_pNetwork->deviceCount());
}

void ProLinkFeature::onListeningChanged(bool listening, const QString& error) {
    m_listening = listening;
    m_error = error;
    // No QMessageBox on failure, ever: the library is constructed during
    // startup and a modal dialog there blocks the whole application before there
    // is a window to own it. See the comment at library.cpp:170.
    emit featureIsLoading(this, false);
}

void ProLinkFeature::onRefresh(double value) {
    if (value > 0) {
        m_pNetwork->refresh();
    }
}

void ProLinkFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* pKeyboard) {
    Q_UNUSED(pKeyboard);
    parented_ptr<WLibraryTextBrowser> pEdit =
            make_parented<WLibraryTextBrowser>(pLibraryWidget);
    pEdit->setHtml(statusHtml());
    pEdit->setOpenLinks(false);
    pLibraryWidget->registerView(kViewName, pEdit);
}

QString ProLinkFeature::statusHtml() const {
    QString html;
    html += QStringLiteral("<h2>%1</h2>").arg(tr("Pro DJ Link"));

    if (!m_listening) {
        html += QStringLiteral("<p><b>%1</b></p>")
                        .arg(tr("Not listening on UDP port 50000."));
        if (!m_error.isEmpty()) {
            html += QStringLiteral("<p>%1</p>").arg(m_error.toHtmlEscaped());
        }
        html += QStringLiteral("<p>%1</p>")
                        .arg(tr("Another program may already be using the port — "
                                "rekordbox, or a second copy of Mixxx."));
        return html;
    }

    const QList<ProLinkDevice> devices = m_pNetwork->devices();
    html += QStringLiteral("<p>%1</p>")
                    .arg(tr("Listening on UDP port 50000. Mixxx does not transmit "
                            "anything on the Pro DJ Link network."));

    if (devices.isEmpty()) {
        html += QStringLiteral("<p>%1</p>").arg(tr("No players found yet."));
        // A CDJ tries DHCP roughly three times before self-assigning, so it is
        // ~9 s after power-on before it says anything at all (F8). Without this
        // note an impatient user concludes the feature is broken.
        html += QStringLiteral("<p>%1</p>")
                        .arg(tr("A CDJ takes about 10 seconds after power-on to "
                                "appear, and does not answer ping."));
        return html;
    }

    html += QStringLiteral("<table cellpadding='4'><tr><th align='left'>%1</th>"
                           "<th align='left'>%2</th><th align='left'>%3</th>"
                           "<th align='left'>%4</th></tr>")
                    .arg(tr("Player"), tr("Name"), tr("Address"), tr("Interface"));
    for (const ProLinkDevice& device : devices) {
        html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                        .arg(QString::number(device.deviceNumber),
                                device.name.toHtmlEscaped(),
                                device.address.toString(),
                                device.interfaceName);
    }
    html += QStringLiteral("</table>");
    html += QStringLiteral("<p><i>%1</i></p>")
                    .arg(tr("Browsing a player's media is not implemented yet."));
    return html;
}
