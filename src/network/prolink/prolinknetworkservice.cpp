#include "network/prolink/prolinknetworkservice.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTimer>
#include <exception>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_prolinknetworkservice.cpp"
#include "prolink-cxx/src/lib.rs.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNetworkService");

/// How often the library's events are drained and its tables re-read.
///
/// 33 ms, which is what the phase publisher this replaced used: the bar-phase
/// marker is animated from `[ProLink] master_bar_phase`, so this is the rate
/// that marker moves at. A beat at 145 BPM is 414 ms apart, so events are far
/// less demanding than the marker is.
constexpr int kPollIntervalMs = 33;

mixxx::prolink::MediaSlot toMixxxSlot(::prolink::Slot slot) {
    switch (slot) {
    case ::prolink::Slot::Cd:
        return mixxx::prolink::MediaSlot::Cd;
    case ::prolink::Slot::Sd:
        return mixxx::prolink::MediaSlot::Sd;
    case ::prolink::Slot::Usb:
        return mixxx::prolink::MediaSlot::Usb;
    case ::prolink::Slot::Rekordbox:
        return mixxx::prolink::MediaSlot::Rekordbox;
    default:
        return mixxx::prolink::MediaSlot::Empty;
    }
}

::prolink::Slot toRustSlot(mixxx::prolink::MediaSlot slot) {
    switch (slot) {
    case mixxx::prolink::MediaSlot::Cd:
        return ::prolink::Slot::Cd;
    case mixxx::prolink::MediaSlot::Sd:
        return ::prolink::Slot::Sd;
    case mixxx::prolink::MediaSlot::Rekordbox:
        return ::prolink::Slot::Rekordbox;
    case mixxx::prolink::MediaSlot::Usb:
    default:
        // USB for anything unnamed: it is the slot a deck browses first, and
        // the one a caller means when it has not thought about it.
        return ::prolink::Slot::Usb;
    }
}

mixxx::prolink::DeviceKind toMixxxKind(::prolink::DeviceKind kind) {
    switch (kind) {
    case ::prolink::DeviceKind::Mixer:
        return mixxx::prolink::DeviceKind::Mixer;
    case ::prolink::DeviceKind::Rekordbox:
        return mixxx::prolink::DeviceKind::RekordboxOrCdj3000;
    case ::prolink::DeviceKind::Cdj:
    default:
        return mixxx::prolink::DeviceKind::Cdj;
    }
}

QString toQString(const ::rust::String& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// Whether two serve statuses would draw the same page.
///
/// Field by field rather than a memcmp or an operator==: the struct is the
/// library feature's vocabulary, and adding a comparison to it would put a
/// definition of "changed" somewhere nothing else looks.
bool sameServeStatus(const mixxx::prolink::server::ServeStatus& left,
        const mixxx::prolink::server::ServeStatus& right) {
    if (left.active != right.active || left.deviceNumber != right.deviceNumber ||
            left.address != right.address || left.interfaceName != right.interfaceName ||
            left.portmapPort != right.portmapPort || left.mountdPort != right.mountdPort ||
            left.nfsdPort != right.nfsdPort || left.dbserverPort != right.dbserverPort ||
            left.media.size() != right.media.size() ||
            left.consumers.size() != right.consumers.size()) {
        return false;
    }
    for (int i = 0; i < left.media.size(); ++i) {
        const mixxx::prolink::server::ServedSlot& a = left.media.at(i);
        const mixxx::prolink::server::ServedSlot& b = right.media.at(i);
        if (a.slot != b.slot || a.volumeName != b.volumeName ||
                a.localPath != b.localPath || a.trackCount != b.trackCount) {
            return false;
        }
    }
    for (int i = 0; i < left.consumers.size(); ++i) {
        const mixxx::prolink::server::ServeConsumer& a = left.consumers.at(i);
        const mixxx::prolink::server::ServeConsumer& b = right.consumers.at(i);
        if (a.deviceNumber != b.deviceNumber || a.slot != b.slot ||
                a.trackId != b.trackId || a.playing != b.playing) {
            return false;
        }
    }
    return true;
}

mixxx::prolink::ProLinkDevice toMixxxDevice(const ::prolink::Device& device) {
    mixxx::prolink::ProLinkDevice out;
    out.mac = toQString(device.mac).toLatin1();
    out.address = QHostAddress(toQString(device.address));
    out.name = toQString(device.name);
    out.nameRaw = out.name.toUtf8();
    out.deviceNumber = device.number;
    out.kind = toMixxxKind(device.kind);
    out.online = device.online;
    return out;
}
} // namespace

namespace mixxx {
namespace prolink {

/// Everything that would otherwise drag the generated bridge header into every
/// translation unit that includes ours.
struct ProLinkNetworkService::Impl {
    /// Null until `start()`, and again after `shutdown()`.
    ///
    /// A `rust::Box` has no empty state — it is a non-null owning pointer by
    /// construction — so the optionality lives here rather than in the Box.
    std::unique_ptr<::rust::Box<::prolink::Session>> pSession;

    void stop() {
        // Dropping the Box drops the session, which releases the device number
        // and closes the sockets.
        pSession.reset();
    }
};

ProLinkNetworkService::ProLinkNetworkService(QObject* parent)
        : QObject(parent),
          m_pImpl(std::make_unique<Impl>()),
          m_pPullDbControl(std::make_unique<ControlPushButton>(
                  ConfigKey(QStringLiteral("[ProLink]"), QStringLiteral("pull_db")))),
          m_pMasterDeviceControl(std::make_unique<ControlObject>(
                  ConfigKey(QStringLiteral("[ProLink]"), QStringLiteral("master_device")))),
          m_pMasterBpmControl(std::make_unique<ControlObject>(
                  ConfigKey(QStringLiteral("[ProLink]"), QStringLiteral("master_bpm")))),
          m_pMasterBarPhaseControl(std::make_unique<ControlObject>(ConfigKey(
                  QStringLiteral("[ProLink]"), QStringLiteral("master_bar_phase")))) {
    connect(m_pPullDbControl.get(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                if (value > 0) {
                    pullDatabase();
                }
            });

    // Read-only: nothing in Mixxx may tell a CDJ what phase it is at, and a
    // skin binding that could write these would look like it worked.
    //
    // Created in the constructor rather than when the network comes up,
    // because the phase-meter widget resolves them once when the skin loads —
    // and a widget that resolved nothing logs
    // "getControl returning NULL" and then never looks again.
    m_pMasterDeviceControl->setReadOnly();
    m_pMasterBpmControl->setReadOnly();
    m_pMasterBarPhaseControl->setReadOnly();
    m_pMasterBarPhaseControl->forceSet(-1.0);
    m_pMasterBpmControl->forceSet(0.0);
    m_pMasterDeviceControl->forceSet(0.0);
}

void ProLinkNetworkService::publishMaster() {
    if (!m_pImpl->pSession) {
        m_pMasterDeviceControl->forceSet(0.0);
        m_pMasterBpmControl->forceSet(0.0);
        m_pMasterBarPhaseControl->forceSet(-1.0);
        return;
    }
    for (const ::prolink::Player& player : (*m_pImpl->pSession)->players()) {
        if (!player.is_master) {
            continue;
        }
        m_pMasterDeviceControl->forceSet(player.number);
        // The tempo actually playing, with the pitch fader applied. The
        // library reports a negative for "not known", which the widget must
        // not draw as a tempo.
        m_pMasterBpmControl->forceSet(player.effective_bpm);
        m_pMasterBarPhaseControl->forceSet(player.bar_phase);
        return;
    }
    // Nobody holds master. Not the same as a master at phase zero, which is
    // why the phase goes to -1 rather than to 0.
    m_pMasterDeviceControl->forceSet(0.0);
    m_pMasterBpmControl->forceSet(0.0);
    m_pMasterBarPhaseControl->forceSet(-1.0);
}

ProLinkNetworkService::~ProLinkNetworkService() {
    shutdown();
}

void ProLinkNetworkService::start() {
    if (m_pImpl->pSession) {
        return;
    }
    // The library's own log, to stderr, which on the deck is where Mixxx's
    // own already goes. Without it the only evidence of what the protocol is
    // doing is the socket table, read over ssh.
    ::prolink::init_logging(::rust::Str("prolink=info"));

    try {
        ::prolink::Config config = ::prolink::default_config();
        // Announcing is what makes players unicast their status to us, and
        // status is the only place the loaded track, the play state and the
        // tempo master are published. Without it we would see beats and
        // nothing else.
        config.announce = true;
        // The number we held before a restart, so a refresh keeps the identity
        // the decks already know. Zero on a cold start, which means "negotiate
        // for whichever of 1-4 is free" -- and 1-4 is a requirement, not a
        // preference: at any other number a deck accepts our announcement in
        // full and then never offers us as a LINK source or asks us anything.
        config.preferred_number = static_cast<::std::uint8_t>(
                m_preferredNumber >= 1 && m_preferredNumber <= 4 ? m_preferredNumber : 0);
        m_pImpl->pSession = std::make_unique<::rust::Box<::prolink::Session>>(
                ::prolink::open(config));
    } catch (const std::exception& error) {
        m_lastError = QString::fromUtf8(error.what());
        m_listening = false;
        kLogger.warning() << "could not start:" << m_lastError;
        emit listeningChanged(false, m_lastError);
        return;
    }

    m_listening = true;
    m_lastError.clear();
    // Zero for now, and that is not a failure. Claiming a player number means
    // watching the network and then negotiating for it, about five seconds in
    // all, and open() deliberately returns before that so the GUI is not frozen
    // for the duration. poll() announces the number when it arrives.
    m_announcedNumber = 0;
    m_announceDetail = tr("joining the network...");
    kLogger.info() << "started;" << m_announceDetail;

    emit listeningChanged(true, QString());
    emit announceChanged(m_announcedNumber, m_announceDetail);

    if (m_pTimer == nullptr) {
        m_pTimer = new QTimer(this);
        connect(m_pTimer, &QTimer::timeout, this, &ProLinkNetworkService::poll);
    }
    m_pTimer->start(kPollIntervalMs);
}

void ProLinkNetworkService::shutdown() {
    if (m_pTimer != nullptr) {
        m_pTimer->stop();
    }
    const bool wasListening = m_listening;
    m_pImpl->stop();
    m_listening = false;
    m_announcedNumber = 0;
    m_announceDetail.clear();

    // Report what is gone, so nothing above keeps drawing a device that is no
    // longer there.
    const QList<ProLinkDevice> had = m_devices;
    m_devices.clear();
    m_pending.clear();
    m_publishedNumber = 0;
    m_serveStatus = server::ServeStatus();
    emit serveStatusChanged(m_serveStatus);
    for (const ProLinkDevice& device : had) {
        emit deviceLost(device.mac);
    }

    if (wasListening) {
        emit listeningChanged(false, QString());
        emit announceChanged(0, QString());
    }
}

void ProLinkNetworkService::refresh() {
    if (!m_pImpl->pSession) {
        start();
        return;
    }

    // Restart, rather than only dropping the browse connections.
    //
    // The interface is chosen when the session opens, and a Mixxx started
    // before the ethernet was plugged in chose whatever was there — on the
    // deck that was the wireless interface, and the CDJs that appeared
    // afterwards were on a network we were not listening to. Nothing about
    // that resolves itself: no keep-alive can arrive on a socket bound to
    // another interface, however long the user waits.
    //
    // So the only honest thing a refresh can do is bind again. It costs the
    // device number and a second of re-discovery, which is what a user
    // clicking "refresh" is asking for.
    shutdown();
    start();
}

int ProLinkNetworkService::numberFor(const QByteArray& mac) const {
    if (!m_pImpl->pSession) {
        return 0;
    }
    return static_cast<int>(
            (*m_pImpl->pSession)->device_number_of(::rust::Str(mac.constData(), mac.size())));
}

void ProLinkNetworkService::fetchFile(const QByteArray& mac,
        MediaSlot slot,
        const QString& remotePath,
        const QString& localPath,
        bool priority) {
    Q_UNUSED(priority);
    if (!m_pImpl->pSession) {
        emit fileFetched(localPath, tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit fileFetched(localPath, tr("that player is no longer on the network"));
        return;
    }

    const QByteArray remote = remotePath.toUtf8();
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_file(static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           ::rust::Str(remote.constData(), remote.size()),
                                           ::rust::Str(local.constData(), local.size()));
        Pending pending;
        pending.isDatabase = false;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit fileFetched(localPath, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::fetchDatabase(const QByteArray& mac, MediaSlot slot) {
    if (!m_pImpl->pSession) {
        emit databaseFetched(mac, slot, QByteArray(), tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit databaseFetched(
                mac, slot, QByteArray(), tr("that player is no longer on the network"));
        return;
    }

    // One file per (player, slot), so a second pull overwrites rather than
    // accumulating copies of a database that is often several megabytes.
    const QString localPath = QStringLiteral("%1/prolink-%2-%3.pdb")
                                      .arg(QDir::tempPath(),
                                              QString::fromLatin1(mac.toHex()),
                                              QString::number(static_cast<int>(slot)));
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_database(static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           ::rust::Str(local.constData(), local.size()));
        Pending pending;
        pending.isDatabase = true;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit databaseFetched(mac, slot, QByteArray(), QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::pullDatabase(MediaSlot slot) {
    if (!m_pImpl->pSession) {
        return;
    }
    // The first player that says it has something in that slot. Occupancy is
    // published in status packets and nowhere else, which is why this reads
    // the library's view rather than guessing from the device list.
    for (const ::prolink::MediaInfo& info : (*m_pImpl->pSession)->media()) {
        if (!info.has_media || toMixxxSlot(info.slot) != slot) {
            continue;
        }
        for (const ProLinkDevice& device : m_devices) {
            if (device.deviceNumber == static_cast<int>(info.device)) {
                fetchDatabase(device.mac, slot);
                return;
            }
        }
    }
    kLogger.info() << "no player has media in that slot yet";
}

void ProLinkNetworkService::fetchArtwork(const QByteArray& mac,
        MediaSlot slot,
        quint32 artworkId,
        const QString& localPath) {
    if (!m_pImpl->pSession) {
        emit artworkFetched(localPath, tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit artworkFetched(localPath, tr("that player is no longer on the network"));
        return;
    }

    // Artwork comes over the dbserver connection rather than NFS. Asking NFS
    // for it instead churns the deck's filehandle table until it answers
    // NFSERR_STALE to everything, including the track a DJ is loading.
    //
    // Like a file fetch this returns an id and finishes later: the library
    // feature asks for every cover on a medium in one loop -- some six hundred
    // of them -- and a blocking round trip each would freeze the GUI for the
    // length of all six hundred.
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_artwork(static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           artworkId,
                                           ::rust::Str(local.constData(), local.size()));
        Pending pending;
        pending.isArtwork = true;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit artworkFetched(localPath, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::poll() {
    if (!m_pImpl->pSession) {
        return;
    }
    // Startup is asynchronous, so a bind that fails -- another Pro DJ Link
    // program already holding a port is the usual reason -- surfaces here
    // rather than as an exception from open().
    if (m_listening && !(*m_pImpl->pSession)->is_ready()) {
        const QString error = toQString((*m_pImpl->pSession)->last_error());
        if (!error.isEmpty() && error != m_lastError) {
            m_lastError = error;
            m_listening = false;
            kLogger.warning() << "could not start:" << error;
            emit listeningChanged(false, error);
        }
    }

    for (const ::prolink::Event& event : (*m_pImpl->pSession)->drain_events()) {
        if (event.dropped > 0) {
            // The queue overflowed, so the running picture is stale and the
            // table has to be re-read rather than patched. Clearing it makes
            // the diff below re-announce everything.
            kLogger.debug() << "missed" << event.dropped << "events; re-reading";
            m_devices.clear();
        }
        switch (event.kind) {
        case ::prolink::EventKind::MediaInfo:
            syncMedia(static_cast<int>(event.device), toMixxxSlot(event.slot));
            break;
        case ::prolink::EventKind::TransferProgress: {
            const auto found = m_pending.constFind(event.transfer);
            if (found != m_pending.constEnd() && !found->isArtwork) {
                emit fileFetchProgress(found->localPath,
                        static_cast<quint32>(event.done),
                        static_cast<quint32>(event.total));
            }
            break;
        }
        case ::prolink::EventKind::TransferDone: {
            const auto found = m_pending.constFind(event.transfer);
            if (found == m_pending.constEnd()) {
                // Not ours. A transfer the library started for its own reasons,
                // or one left over from a session that has since been
                // restarted — either way there is nobody waiting on a signal
                // for it, and emitting one with an empty path would abort a
                // fetch that is still running under the same empty key.
                break;
            }
            const Pending pending = m_pending.take(event.transfer);
            const QString error = event.ok ? QString() : toQString(event.detail);
            // A missing cover is not worth reporting as the connection's last
            // error: a medium has hundreds of them, a few are always absent,
            // and this string is what the UI shows about the network itself.
            if (!error.isEmpty() && !pending.isArtwork) {
                m_lastError = error;
            }
            if (pending.isDatabase) {
                // The caller parses the bytes and never wants the file, so
                // the temp copy is read back and dropped here rather than
                // becoming something it has to clean up.
                QByteArray data;
                QString reason = error;
                if (reason.isEmpty()) {
                    QFile file(pending.localPath);
                    if (file.open(QIODevice::ReadOnly)) {
                        data = file.readAll();
                        file.close();
                    } else {
                        reason = tr("could not read the database back: %1")
                                         .arg(file.errorString());
                    }
                    QFile::remove(pending.localPath);
                }
                emit databaseFetched(pending.mac, pending.slot, data, reason);
            } else if (pending.isArtwork) {
                emit artworkFetched(pending.localPath, error);
            } else {
                emit fileFetched(pending.localPath, error);
            }
            break;
        }
        default:
            // Beats, player state and tempo master are read from the tables
            // below rather than acted on here.
            break;
        }
    }

    syncDevices();
    publishMaster();
    syncAnnouncement();
    syncServeStatus();
}

void ProLinkNetworkService::syncServeStatus() {
    const ::prolink::ServeStatus fresh = (*m_pImpl->pSession)->serve_status();

    server::ServeStatus status;
    status.active = fresh.active;
    status.deviceNumber = static_cast<int>(fresh.device_number);
    status.deviceName = tr("Mixxx (this machine)");
    status.address = QHostAddress(toQString(fresh.address));
    status.interfaceName = toQString(fresh.interface);
    status.portmapPort = fresh.portmap_port;
    status.mountdPort = fresh.mount_port;
    status.nfsdPort = fresh.nfs_port;
    status.dbserverPort = fresh.dbserver_port;
    for (const ::prolink::ServedSlot& slot : fresh.media) {
        server::ServedSlot served;
        served.slot = toMixxxSlot(slot.slot);
        served.exportPath = toQString(slot.export_path);
        served.volumeName = toQString(slot.volume_name);
        served.localPath = toQString(slot.local_path);
        served.trackCount = static_cast<int>(slot.track_count);
        served.playlistCount = static_cast<int>(slot.playlist_count);
        status.media.append(served);
    }
    for (const ::prolink::ServeConsumer& reader : fresh.consumers) {
        server::ServeConsumer consumer;
        consumer.deviceNumber = static_cast<int>(reader.device_number);
        consumer.slot = toMixxxSlot(reader.slot);
        consumer.trackId = reader.track_id;
        consumer.playing = reader.playing;
        // Named from the device table, which the library fills from
        // keep-alives; a consumer we have not seen one from still counts,
        // because its status packet is what put it here.
        for (const ProLinkDevice& device : m_devices) {
            if (device.deviceNumber == consumer.deviceNumber) {
                consumer.deviceName = device.name;
                consumer.address = device.address;
                break;
            }
        }
        status.consumers.append(consumer);
    }

    // Emitted on a change rather than thirty times a second: the page it feeds
    // rebuilds its whole HTML, and a DJ scrolling it would fight the rebuild.
    if (sameServeStatus(status, m_serveStatus)) {
        return;
    }
    m_serveStatus = status;
    emit serveStatusChanged(m_serveStatus);
}

void ProLinkNetworkService::syncAnnouncement() {
    const int number = static_cast<int>((*m_pImpl->pSession)->device_number());
    if (number == m_publishedNumber) {
        return;
    }
    m_publishedNumber = number;
    m_announcedNumber = number;
    if (number >= 1 && number <= 4) {
        m_preferredNumber = number;
    }
    if (number >= 1 && number <= 4) {
        m_announceDetail = tr("announced as player %1").arg(number);
    } else if (number > 0) {
        // Every player number was defended, so the library settled for one
        // outside the range. Everything passive still works; being browsed does
        // not, and the detail has to say so rather than read like success.
        m_announceDetail = tr("watching as device %1; no player number was free").arg(number);
    } else if ((*m_pImpl->pSession)->is_ready()) {
        m_announceDetail = tr("listening without a player number");
    } else {
        m_announceDetail = tr("joining the network...");
    }
    kLogger.info() << "announcement changed:" << m_announceDetail;
    emit announceChanged(m_announcedNumber, m_announceDetail);
}

void ProLinkNetworkService::syncDevices() {
    QList<ProLinkDevice> fresh;
    for (const ::prolink::Device& device : (*m_pImpl->pSession)->devices()) {
        fresh.append(toMixxxDevice(device));
    }

    // Diffed rather than replaced wholesale, because the library feature
    // listens for found/changed/lost and rebuilding its tree on every poll
    // would collapse the user's selection twenty times a second.
    for (const ProLinkDevice& now : fresh) {
        bool seen = false;
        for (const ProLinkDevice& before : m_devices) {
            if (before.mac != now.mac) {
                continue;
            }
            seen = true;
            if (before.deviceNumber != now.deviceNumber || before.name != now.name ||
                    before.online != now.online || before.address != now.address) {
                emit deviceChanged(now);
            }
            break;
        }
        if (!seen) {
            emit deviceFound(now);
        }
    }
    for (const ProLinkDevice& before : m_devices) {
        bool still = false;
        for (const ProLinkDevice& now : fresh) {
            if (before.mac == now.mac) {
                still = true;
                break;
            }
        }
        if (!still) {
            emit deviceLost(before.mac);
        }
    }

    m_devices = fresh;
}

void ProLinkNetworkService::syncMedia(int deviceNumber, MediaSlot slot) {
    for (const ::prolink::MediaInfo& found : (*m_pImpl->pSession)->media()) {
        if (static_cast<int>(found.device) != deviceNumber ||
                toMixxxSlot(found.slot) != slot) {
            continue;
        }
        MediaInfo info;
        info.name = toQString(found.volume_name);
        info.trackCount = found.track_count;
        info.playlistCount = found.playlist_count;
        for (const ProLinkDevice& device : m_devices) {
            if (device.deviceNumber == deviceNumber) {
                emit mediaInfoFound(device.mac, slot, info);
                return;
            }
        }
        return;
    }
}

} // namespace prolink
} // namespace mixxx
