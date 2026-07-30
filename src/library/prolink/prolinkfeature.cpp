#include "library/prolink/prolinkfeature.h"

#include <QMenu>
#include <QUrl>

#include <QDir>
#include <QSet>
#include <QSqlDatabase>
#include <QStandardPaths>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/prolink/prolinkdbwriter.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "util/logger.h"
#include "library/queryutil.h"
#include "moc_prolinkfeature.cpp"
#include "network/prolink/prolinknetworkservice.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

using mixxx::prolink::MediaSlot;
using mixxx::prolink::parsePdb;
using mixxx::prolink::ProLinkDevice;
using mixxx::prolink::ProLinkNetworkService;

namespace {
const mixxx::Logger kLogger("ProLinkFeature");
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
/// Slot number for a slot node; playlist id for a playlist node.
constexpr int kPayloadSlot = 2;
constexpr int kPayloadPlaylist = 3;

const QString kKindDevice = QStringLiteral("device");
const QString kKindSlot = QStringLiteral("slot");
const QString kKindPlaylist = QStringLiteral("playlist");

QString macKey(const QByteArray& mac) {
    return QString::fromLatin1(mac.toHex());
}

QVariant deviceNodePayload(const ProLinkDevice& device) {
    return QVariant(QList<QString>{kKindDevice, macKey(device.mac)});
}

QVariant slotNodePayload(const QByteArray& mac, MediaSlot slot) {
    return QVariant(QList<QString>{kKindSlot,
            macKey(mac),
            QString::number(static_cast<int>(slot))});
}

QVariant playlistNodePayload(const QByteArray& mac, MediaSlot slot, quint32 playlistId) {
    return QVariant(QList<QString>{kKindPlaylist,
            macKey(mac),
            QString::number(static_cast<int>(slot)),
            QString::number(playlistId)});
}

/// The two slots a player can have media in. CD is skipped: a CDJ-2000NXS has
/// one, but rekordbox media does not live there and nothing we do applies to it.
const MediaSlot kBrowsableSlots[] = {MediaSlot::Usb, MediaSlot::Sd};

QString slotLabel(MediaSlot slot) {
    return slot == MediaSlot::Usb ? QStringLiteral("USB") : QStringLiteral("SD");
}
} // namespace

ProLinkFeature::ProLinkFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("prolink")),
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
    connect(m_pNetwork.get(),
            &ProLinkNetworkService::databaseFetched,
            this,
            &ProLinkFeature::onDatabaseFetched);

    // The columns the track table can show. analyze_path rides along because
    // ColumnCache keys on the plain column name, so it maps for free.
    QStringList columns = {LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_COLOR,
            REKORDBOX_ANALYZE_PATH,
            // Ours alone; the model reads it by name to find the medium's cover
            // file, since Mixxx would otherwise guess from the audio file's
            // directory and a rekordbox medium keeps its art under PIONEER/.
            QStringLiteral("artwork_path"),
            LIBRARYTABLE_COVERART,
            LIBRARYTABLE_COVERART_SOURCE,
            LIBRARYTABLE_COVERART_TYPE,
            LIBRARYTABLE_COVERART_LOCATION,
            LIBRARYTABLE_COVERART_COLOR,
            LIBRARYTABLE_COVERART_DIGEST};
    QStringList searchColumns = {LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};
    m_trackSource = QSharedPointer<BaseTrackCache>::create(m_pTrackCollection,
            mixxx::prolink::kProLinkLibraryTable,
            QString(LIBRARYTABLE_ID),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pFetcher = make_parented<ProLinkTrackFetcher>(m_pNetwork.get(), this);
    m_pPlaylistModel = make_parented<ProLinkPlaylistModel>(
            this, pLibrary->trackCollectionManager(), m_trackSource);
    m_pPlaylistModel->setFetcher(m_pFetcher);

    // Recreate the temporary tables from empty. They hold a cache of somebody
    // else's USB stick and are worthless once it is unplugged, so carrying them
    // across a restart would only produce rows pointing at media that may not
    // be on the network any more.
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    mixxx::prolink::dropProLinkTables(database);
    mixxx::prolink::createProLinkTables(database);
    transaction.commit();

    // Purge anything under the cache root from the user's *real* library.
    //
    // The cache is boot-scoped -- it is cleared below, so every path in it is
    // about to become invalid -- and rows left behind would show up in Missing
    // Tracks forever, growing by one per ProLink track ever touched. This also
    // cleans up after an earlier build in which the model's getTrack() let the
    // base class add a row for every row merely *scrolled past*.
    // Via the manager: TrackCollection::purgeAllTracks is private to it.
    QDir cacheRoot(cacheRootPath());
    pLibrary->trackCollectionManager()->purgeAllTracks(cacheRoot);
    if (cacheRoot.exists()) {
        cacheRoot.removeRecursively();
    }

    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    m_pNetwork->start();
}

std::unique_ptr<BaseSqlTableModel> ProLinkFeature::createPlaylistModelForPlaylist(
        const QVariant& data) {
    auto pModel = std::make_unique<ProLinkPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    pModel->setFetcher(m_pFetcher);
    pModel->setPlaylist(data.toString());
    return pModel;
}

void ProLinkFeature::appendTrackIdsFromRightClickIndex(
        QList<TrackId>* pTrackIds, QString* pPlaylist) {
    // Right-click actions (export, add to Auto DJ) need real local files, and
    // nothing has been fetched yet. Left empty rather than half-implemented:
    // handing back ids whose files do not exist would put broken rows wherever
    // they were sent.
    Q_UNUSED(pTrackIds);
    Q_UNUSED(pPlaylist);
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
    if (!index.isValid()) {
        return;
    }
    auto* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (pItem == nullptr) {
        return;
    }
    const QList<QString> payload = pItem->getData().toStringList();
    if (payload.size() <= kPayloadMac) {
        showStatusPage();
        return;
    }
    const QString kind = payload.at(kPayloadKind);
    const QByteArray mac = QByteArray::fromHex(payload.at(kPayloadMac).toLatin1());

    if (kind == kKindSlot && payload.size() > kPayloadSlot) {
        const auto slot = static_cast<MediaSlot>(payload.at(kPayloadSlot).toInt());
        Medium& medium = m_media[mediumKey(mac, slot)];
        switch (medium.state) {
        case Medium::State::Fetching:
            // Already in flight. Returning early is the point of tracking state:
            // a second click would otherwise start a second megabyte transfer
            // over a link the CDJs are themselves playing across.
            break;
        case Medium::State::Ready:
            showPlaylists(mac, slot);
            break;
        case Medium::State::Unknown:
        case Medium::State::Failed:
            // Failed is retried rather than remembered: the usual cause is an
            // empty slot or a deck that was mid-something, and both change.
            medium.state = Medium::State::Fetching;
            medium.error.clear();
            m_pNetwork->fetchDatabase(mac, slot);
            break;
        }
        showStatusPage();
        return;
    }

    if (kind == kKindPlaylist && payload.size() > kPayloadPlaylist) {
        const auto slot = static_cast<MediaSlot>(payload.at(kPayloadSlot).toInt());
        // The name the rows were written under: namespaced by medium, because
        // the playlists table has a UNIQUE name and two sticks can both hold a
        // "Warmup".
        const QString playlistName =
                QStringLiteral("%1|%2")
                        .arg(mixxx::prolink::deviceKeyFor(mac, slot),
                                pItem->getLabel().section(QStringLiteral(" ("), 0, 0));
        m_pPlaylistModel->setMedium(mac,
                slot,
                QDir(cacheRootPath()).filePath(mixxx::prolink::deviceKeyFor(mac, slot)));
        m_pPlaylistModel->setPlaylist(playlistName);
        emit showTrackModel(m_pPlaylistModel);
        return;
    }

    showStatusPage();
}

QString ProLinkFeature::cacheRootPath() {
    // CacheLocation, not the settings directory: this is scratch that the
    // platform already understands to be disposable, and it keeps a
    // multi-gigabyte download area out of the user's Mixxx profile.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("prolink"));
}

QString ProLinkFeature::mediumKey(const QByteArray& mac, MediaSlot slot) {
    return QStringLiteral("%1|%2").arg(macKey(mac)).arg(static_cast<int>(slot));
}

void ProLinkFeature::addSlotNodes(int deviceRow, const QByteArray& mac) {
    TreeItem* pRoot = m_pSidebarModel->getRootItem();
    if (deviceRow < 0 || deviceRow >= pRoot->childRows()) {
        return;
    }
    // Both slots are always offered, even though a player usually has one
    // populated. We cannot know which without announcing ourselves -- slot
    // occupancy is published only in status packets, and those are unicast to
    // announced peers (F20/F21). Offering both and letting the fetch fail is
    // honest; guessing would hide a slot that does have media.
    std::vector<std::unique_ptr<TreeItem>> rows;
    for (const MediaSlot slot : kBrowsableSlots) {
        rows.push_back(std::make_unique<TreeItem>(
                slotLabel(slot), slotNodePayload(mac, slot)));
    }
    m_pSidebarModel->insertTreeItemRows(
            std::move(rows), 0, m_pSidebarModel->index(deviceRow, 0));
}

int ProLinkFeature::rowForSlot(int deviceRow, MediaSlot slot) const {
    TreeItem* pRoot = m_pSidebarModel->getRootItem();
    if (deviceRow < 0 || deviceRow >= pRoot->childRows()) {
        return -1;
    }
    TreeItem* pDevice = pRoot->child(deviceRow);
    const QString wanted = QString::number(static_cast<int>(slot));
    for (int row = 0; row < pDevice->childRows(); ++row) {
        const QList<QString> payload = pDevice->child(row)->getData().toStringList();
        if (payload.size() > kPayloadSlot && payload.at(kPayloadSlot) == wanted) {
            return row;
        }
    }
    return -1;
}

void ProLinkFeature::onDatabaseFetched(const QByteArray& mac,
        MediaSlot slot,
        const QByteArray& data,
        const QString& error) {
    Medium& medium = m_media[mediumKey(mac, slot)];
    if (!error.isEmpty()) {
        medium.state = Medium::State::Failed;
        medium.error = error;
        refreshStatusPage();
        return;
    }

    // Parsing a megabyte takes a few milliseconds, so it stays on the GUI
    // thread rather than buying a QtConcurrent round trip and the lifetime
    // questions that come with one.
    medium.contents = parsePdb(data);
    if (!medium.contents.ok) {
        medium.state = Medium::State::Failed;
        medium.error = medium.contents.error;
        refreshStatusPage();
        return;
    }
    medium.state = Medium::State::Ready;
    medium.error.clear();

    // Where the medium's files will live once fetched. The rows carry the
    // location now so the table can be browsed before anything is downloaded;
    // the path is the player's own, concatenated verbatim onto a local root, so
    // the two trees never have to be reconciled.
    const QString localRoot =
            QDir(cacheRootPath()).filePath(mixxx::prolink::deviceKeyFor(mac, slot));
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    mixxx::prolink::writeMedium(database,
            medium.contents,
            mixxx::prolink::deviceKeyFor(mac, slot),
            localRoot);
    transaction.commit();
    m_trackSource->buildIndex();

    showPlaylists(mac, slot);
    prefetchArtwork(mac, slot);
    refreshStatusPage();
}

void ProLinkFeature::prefetchArtwork(const QByteArray& mac, MediaSlot slot) {
    const Medium& medium = m_media.value(mediumKey(mac, slot));
    if (medium.state != Medium::State::Ready) {
        return;
    }
    const QString localRoot =
            QDir(cacheRootPath()).filePath(mixxx::prolink::deviceKeyFor(mac, slot));

    // Distinct paths only: an album's tracks all share one image, so a 651-track
    // medium has a few hundred covers at most, and fetching per track would ask
    // for the same file a dozen times.
    QSet<QString> wanted;
    for (const mixxx::prolink::PdbTrack& track : medium.contents.tracks) {
        if (!track.artworkPath.isEmpty()) {
            wanted.insert(track.artworkPath);
        }
    }
    if (wanted.isEmpty()) {
        return;
    }

    // Queued, not awaited. Covers are tens of kilobytes each and the NFS client
    // serialises them behind one another anyway; they appear as they land. This
    // deliberately shares the link with the CDJs' own playback, so it is worth
    // knowing it is bounded: a few hundred small files, once per medium, never
    // repeated because fetchOptional skips anything already on disk.
    for (const QString& remote : wanted) {
        m_pFetcher->fetchOptional(mac, slot, remote, localRoot + remote);
    }
    kLogger.info() << "queued" << wanted.size() << "cover images for"
                   << mediumKey(mac, slot);
}

void ProLinkFeature::showPlaylists(const QByteArray& mac, MediaSlot slot) {
    const int deviceRow = rowForMac(mac);
    const int slotRow = rowForSlot(deviceRow, slot);
    if (slotRow < 0) {
        return;
    }
    const Medium& medium = m_media.value(mediumKey(mac, slot));
    if (medium.state != Medium::State::Ready) {
        return;
    }

    const QModelIndex deviceIndex = m_pSidebarModel->index(deviceRow, 0);
    const QModelIndex slotIndex = m_pSidebarModel->index(slotRow, 0, deviceIndex);
    TreeItem* pSlot = m_pSidebarModel->getRootItem()->child(deviceRow)->child(slotRow);
    if (pSlot->childRows() > 0) {
        return; // already populated
    }

    // NOTE(prolink): no clearLastRightClickedIndex() here, because that lives on
    // BaseExternalLibraryFeature and this is still a plain LibraryFeature. When
    // the track table arrives and the base class changes, this call becomes
    // mandatory before *every* structural change: that base holds a raw
    // QModelIndex whose internalPointer() Qt cannot fix up when rows move
    // (baseexternallibraryfeature.h:57-58).

    std::vector<std::unique_ptr<TreeItem>> rows;
    // "All tracks" first, then the curated playlists. A medium with no playlist
    // at all is normal -- a stick can be a flat pile of files -- and without
    // this entry it would look empty when it is not.
    rows.push_back(std::make_unique<TreeItem>(
            tr("All tracks (%1)").arg(medium.contents.tracks.size()),
            playlistNodePayload(mac, slot, 0)));
    for (const auto& playlist : medium.contents.playlists) {
        if (playlist.isFolder) {
            continue; // folders are a later increment
        }
        rows.push_back(std::make_unique<TreeItem>(
                QStringLiteral("%1 (%2)").arg(playlist.name).arg(playlist.trackIds.size()),
                playlistNodePayload(mac, slot, playlist.id)));
    }
    m_pSidebarModel->insertTreeItemRows(std::move(rows), 0, slotIndex);
}

void ProLinkFeature::refreshStatusPage() {
    if (m_pStatusView) {
        m_pStatusView->setHtml(statusHtml());
    }
}

void ProLinkFeature::showStatusPage() {
    refreshStatusPage();
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
    const int row = m_pSidebarModel->getRootItem()->childRows();
    m_pSidebarModel->insertTreeItemRows(std::move(rows), row);
    addSlotNodes(row, device.mac);

    m_pDeviceCountControl->forceSet(m_pNetwork->deviceCount());
    refreshStatusPage();
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
    // device.online, not device.isStale(): the timer inside a copy measures the
    // age of the copy, not of the device. This call site happened to be correct
    // only because the signal is emitted at the instant of the transition.
    const bool offline = !device.online;
    pItem->setLabel(offline ? tr("%1 (offline)").arg(device.label()) : device.label());
    pItem->setBold(!offline);
    m_pSidebarModel->triggerRepaint(m_pSidebarModel->index(row, 0));
    refreshStatusPage();
}

void ProLinkFeature::onDeviceLost(const QByteArray& mac) {
    const int row = rowForMac(mac);
    if (row < 0) {
        return;
    }
    m_pSidebarModel->removeRows(row, 1);
    m_pDeviceCountControl->forceSet(m_pNetwork->deviceCount());
    refreshStatusPage();
}

void ProLinkFeature::onListeningChanged(bool listening, const QString& error) {
    m_listening = listening;
    m_error = error;
    refreshStatusPage();
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
    m_pStatusView = pEdit;
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

    // What each slot we have touched is doing. Without this the sidebar is the
    // only feedback, and "expanded a slot and nothing happened" covers a fetch
    // in flight, an empty slot and a real failure alike.
    if (!m_media.isEmpty()) {
        html += QStringLiteral("<h3>%1</h3><table cellpadding='4'>").arg(tr("Media"));
        for (auto it = m_media.constBegin(); it != m_media.constEnd(); ++it) {
            QString state;
            switch (it.value().state) {
            case Medium::State::Unknown:
                state = tr("not opened");
                break;
            case Medium::State::Fetching:
                state = tr("fetching…");
                break;
            case Medium::State::Ready:
                state = tr("%1 tracks, %2 playlists")
                                .arg(it.value().contents.tracks.size())
                                .arg(it.value().contents.playlistCount());
                break;
            case Medium::State::Failed:
                state = it.value().error.toHtmlEscaped();
                break;
            }
            html += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>")
                            .arg(it.key(), state);
        }
        html += QStringLiteral("</table>");
    }

    html += QStringLiteral("<p><i>%1</i></p>")
                    .arg(tr("Expand a player and open USB or SD to fetch its "
                            "database. Loading tracks is not implemented yet."));
    return html;
}
