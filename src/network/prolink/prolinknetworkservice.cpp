#include "network/prolink/prolinknetworkservice.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTimer>
#include <exception>

#include "control/controlpushbutton.h"
#include "moc_prolinknetworkservice.cpp"
#include "network/prolink/prolinkbridge.h"
#include "network/prolink/prolinkcontrols.h"
#include "network/prolink/prolinksync.h"
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

/// Whether two serve statuses would draw the same page.
///
/// Field by field rather than a memcmp or an operator==: the struct is the
/// pages' vocabulary, and adding a comparison to it would put a definition of
/// "changed" somewhere nothing else looks.
bool sameServeStatus(const mixxx::prolink::ServeStatus& left,
        const mixxx::prolink::ServeStatus& right) {
    if (left.active != right.active || left.deviceNumber != right.deviceNumber ||
            left.address != right.address || left.interfaceName != right.interfaceName ||
            left.portmapPort != right.portmapPort || left.mountdPort != right.mountdPort ||
            left.nfsdPort != right.nfsdPort || left.dbserverPort != right.dbserverPort ||
            left.media.size() != right.media.size() ||
            left.consumers.size() != right.consumers.size()) {
        return false;
    }
    for (int i = 0; i < left.media.size(); ++i) {
        const mixxx::prolink::ServedSlot& a = left.media.at(i);
        const mixxx::prolink::ServedSlot& b = right.media.at(i);
        if (a.slot != b.slot || a.volumeName != b.volumeName ||
                a.localPath != b.localPath || a.trackCount != b.trackCount ||
                a.phantom != b.phantom) {
            return false;
        }
    }
    for (int i = 0; i < left.consumers.size(); ++i) {
        const mixxx::prolink::ServeConsumer& a = left.consumers.at(i);
        const mixxx::prolink::ServeConsumer& b = right.consumers.at(i);
        if (a.deviceNumber != b.deviceNumber || a.slot != b.slot ||
                a.trackId != b.trackId || a.playing != b.playing) {
            return false;
        }
    }
    return true;
}

mixxx::prolink::ProLinkDevice toMixxxDevice(const ::prolink::Device& device) {
    mixxx::prolink::ProLinkDevice out;
    out.mac = mixxx::prolink::toQString(device.mac).toLatin1();
    out.address = QHostAddress(mixxx::prolink::toQString(device.address));
    out.name = mixxx::prolink::toQString(device.name);
    out.deviceNumber = device.number;
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

ProLinkNetworkService::ProLinkNetworkService(const QString& deckGroup,
        ProLinkControls* pControls,
        QObject* parent)
        : QObject(parent),
          m_pImpl(std::make_unique<Impl>()),
          m_pSync(std::make_unique<ProLinkSync>(deckGroup, pControls)) {
    connect(m_pSync.get(),
            &ProLinkSync::masterTrackChanged,
            this,
            &ProLinkNetworkService::masterTrackChanged);
    // Null only in a test, which ProLinkSync warns about.
    if (!pControls) {
        return;
    }
    connect(pControls->pullDatabase(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                if (value > 0) {
                    pullDatabase();
                }
            });
}

void ProLinkNetworkService::setLoadedTrack(int sourcePlayer,
        MediaSlot slot,
        quint32 rekordboxId) {
    m_pSync->setLoadedTrack(sourcePlayer, slot, rekordboxId);
}

/*static*/ ProLinkNetworkService* ProLinkNetworkService::s_pListening = nullptr;

ProLinkNetworkService::~ProLinkNetworkService() {
    // Signals blocked for the whole teardown. shutdown() reports every device
    // as lost and the serve status as gone, which is right when the network is
    // being stopped and wrong when the object is being destroyed: whoever is
    // listening is either already dead or, worse, dying -- a listener that is
    // itself mid-destruction will happily run the handler against members it
    // has already destroyed.
    //
    // This is the general form of the fix; MediaRegistry also disconnects
    // itself explicitly, because relying on one object to remember what
    // another one's destructor emits is how this went unnoticed for a day.
    blockSignals(true);
    shutdown();
}

void ProLinkNetworkService::start() {
    if (m_pImpl->pSession) {
        return;
    }
    // One session per process, and this is not a style rule.
    //
    // Two of these on one machine both bind UDP 50000-50002, both run a
    // portmapper on 111, and both enter the claim chain -- so they compete with
    // each other for a player number, and the one that loses becomes a passive
    // observer that cannot serve or be browsed. That is exactly what happened
    // here once: the browser's registry created one and the old sidebar
    // feature, since removed, created another, and the deck spent a session
    // announcing "no player number was free" against itself.
    //
    // Refused rather than allowed, because the failure is otherwise invisible:
    // everything logs success and the network simply does not work.
    if (s_pListening != nullptr && s_pListening != this) {
        m_lastError = tr("another Pro DJ Link session is already running");
        kLogger.warning() << "refusing to start a second session;"
                          << "the first one holds the sockets and the player number";
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
        m_pImpl->pSession = std::make_unique<::rust::Box<::prolink::Session>>(
                ::prolink::open(config));
    } catch (const std::exception& error) {
        m_lastError = QString::fromUtf8(error.what());
        m_listening = false;
        kLogger.warning() << "could not start:" << m_lastError;
        return;
    }
    m_pSync->setSession(&**m_pImpl->pSession);

    s_pListening = this;
    m_listening = true;
    m_lastError.clear();
    // Zero for now, and that is not a failure. Claiming a player number means
    // watching the network and then negotiating for it, about five seconds in
    // all, and open() deliberately returns before that so the GUI is not frozen
    // for the duration. poll() announces the number when it arrives.
    m_announcedNumber = 0;
    m_announceDetail = tr("joining the network...");
    kLogger.info() << "started;" << m_announceDetail;

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
    m_pSync->setSession(nullptr);
    m_pImpl->stop();
    if (s_pListening == this) {
        s_pListening = nullptr;
    }
    m_pSync->sessionStopped();
    m_listening = false;
    m_announcedNumber = 0;
    m_announceDetail.clear();

    // Report what is gone, so nothing above keeps drawing a device that is no
    // longer there.
    const QList<ProLinkDevice> had = m_devices;
    m_devices.clear();
    m_pending.clear();
    m_publishedNumber = 0;
    m_serveStatus = ServeStatus();
    emit serveStatusChanged(m_serveStatus);
    for (const ProLinkDevice& device : had) {
        emit deviceLost(device.mac);
    }

    if (wasListening) {
        emit announceChanged(0, QString());
    }
}

int ProLinkNetworkService::numberFor(const QByteArray& mac) const {
    if (!m_pImpl->pSession) {
        return 0;
    }
    return static_cast<int>(
            (*m_pImpl->pSession)->device_number_of(::rust::Str(mac.constData(), mac.size())));
}

void ProLinkNetworkService::startTransfer(
        const Pending& pending, const std::function<quint32(int number)>& start) {
    if (!m_pImpl->pSession) {
        emitFailed(pending, tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(pending.mac);
    if (number == 0) {
        emitFailed(pending, tr("that player is no longer on the network"));
        return;
    }
    try {
        m_pending.insert(start(number), pending);
    } catch (const std::exception& error) {
        emitFailed(pending, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::emitFailed(const Pending& pending, const QString& error) {
    switch (pending.kind) {
    case Pending::Kind::File:
        emit fileFetched(pending.localPath, error);
        break;
    case Pending::Kind::Database:
        emit databaseFetched(pending.mac, pending.slot, QByteArray(), error);
        break;
    case Pending::Kind::Artwork:
        emit artworkFetched(pending.localPath, error);
        break;
    case Pending::Kind::Preview:
        emit previewFetched(pending.mac, pending.slot, pending.trackId, QByteArray(), error);
        break;
    }
}

void ProLinkNetworkService::fetchFile(const QByteArray& mac,
        MediaSlot slot,
        const QString& remotePath,
        const QString& localPath) {
    Pending pending;
    pending.kind = Pending::Kind::File;
    pending.mac = mac;
    pending.slot = slot;
    pending.localPath = localPath;
    startTransfer(pending, [&](int number) {
        const QByteArray remote = remotePath.toUtf8();
        const QByteArray local = localPath.toUtf8();
        return (*m_pImpl->pSession)
                ->fetch_file(static_cast<::std::uint8_t>(number),
                        toRustSlot(slot),
                        ::rust::Str(remote.constData(), remote.size()),
                        ::rust::Str(local.constData(), local.size()));
    });
}

void ProLinkNetworkService::fetchFileStreaming(const QByteArray& mac,
        MediaSlot slot,
        const QString& remotePath,
        const QString& localPath,
        quint32 headBytes) {
    Pending pending;
    pending.kind = Pending::Kind::File;
    pending.mac = mac;
    pending.slot = slot;
    pending.localPath = localPath;
    startTransfer(pending, [&](int number) {
        const QByteArray remote = remotePath.toUtf8();
        const QByteArray local = localPath.toUtf8();
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_file_streaming(
                                           static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           ::rust::Str(remote.constData(), remote.size()),
                                           ::rust::Str(local.constData(), local.size()),
                                           headBytes);
        kLogger.info() << "streaming" << remotePath << "from player" << number
                       << "with a" << headBytes << "byte head";
        return id;
    });
}

void ProLinkNetworkService::fetchDatabase(const QByteArray& mac, MediaSlot slot) {
    Pending pending;
    pending.kind = Pending::Kind::Database;
    pending.mac = mac;
    pending.slot = slot;
    // One file per (player, slot), so a second pull overwrites rather than
    // accumulating copies of a database that is often several megabytes.
    pending.localPath = QStringLiteral("%1/prolink-%2-%3.pdb")
                                .arg(QDir::tempPath(),
                                        QString::fromLatin1(mac.toHex()),
                                        QString::number(static_cast<int>(slot)));
    startTransfer(pending, [&](int number) {
        const QByteArray local = pending.localPath.toUtf8();
        return (*m_pImpl->pSession)
                ->fetch_database(static_cast<::std::uint8_t>(number),
                        toRustSlot(slot),
                        ::rust::Str(local.constData(), local.size()));
    });
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

void ProLinkNetworkService::fetchWaveformPreview(
        const QByteArray& mac, MediaSlot slot, quint32 trackId) {
    Pending pending;
    pending.kind = Pending::Kind::Preview;
    pending.mac = mac;
    pending.slot = slot;
    pending.trackId = trackId;
    // Over dbserver, on the connection artwork already uses. 900 bytes, and no
    // file at either end -- the bytes are taken off the session when the
    // transfer finishes.
    startTransfer(pending, [&](int number) {
        return (*m_pImpl->pSession)
                ->fetch_waveform_preview(static_cast<::std::uint8_t>(number),
                        toRustSlot(slot),
                        trackId);
    });
}

void ProLinkNetworkService::fetchArtwork(const QByteArray& mac,
        MediaSlot slot,
        quint32 artworkId,
        const QString& localPath) {
    Pending pending;
    pending.kind = Pending::Kind::Artwork;
    pending.mac = mac;
    pending.slot = slot;
    pending.localPath = localPath;
    // Artwork comes over the dbserver connection rather than NFS. Asking NFS
    // for it instead churns the deck's filehandle table until it answers
    // NFSERR_STALE to everything, including the track a DJ is loading.
    //
    // Like a file fetch this returns an id and finishes later: the browser
    // asks for the cover of every row it draws, and a blocking round trip
    // each would freeze the GUI for the length of all of them.
    startTransfer(pending, [&](int number) {
        const QByteArray local = localPath.toUtf8();
        return (*m_pImpl->pSession)
                ->fetch_artwork(static_cast<::std::uint8_t>(number),
                        toRustSlot(slot),
                        artworkId,
                        ::rust::Str(local.constData(), local.size()));
    });
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
        }
    }

    // Before the events: a slot description is reported against its device's
    // MAC, from this table, and a deck that joined since the last poll -- or
    // during a stall -- must already be in it. The description is not sent
    // twice.
    syncDevices();
    for (const ::prolink::Event& event : (*m_pImpl->pSession)->drain_events()) {
        if (event.dropped > 0) {
            // This thread stopped draining the queue for seconds. Nothing is
            // lost for good: devices and players are re-read from their tables
            // every poll, a transfer's progress is overtaken by its end, and
            // the session never discards the two events nothing repeats, a
            // slot description and a transfer's end.
            kLogger.warning() << "stalled; missed" << event.dropped << "events";
        }
        switch (event.kind) {
        case ::prolink::EventKind::MediaInfo:
            syncMedia(static_cast<int>(event.device), toMixxxSlot(event.slot));
            break;
        case ::prolink::EventKind::TransferProgress: {
            const auto found = m_pending.constFind(event.transfer);
            if (found != m_pending.constEnd() && found->kind != Pending::Kind::Artwork &&
                    found->kind != Pending::Kind::Preview) {
                emit fileFetchProgress(found->localPath,
                        static_cast<quint64>(event.done),
                        static_cast<quint64>(event.total),
                        static_cast<quint64>(event.offset),
                        static_cast<quint64>(event.len));
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
            if (!error.isEmpty() && pending.kind != Pending::Kind::Artwork &&
                    pending.kind != Pending::Kind::Preview) {
                m_lastError = error;
            }
            switch (pending.kind) {
            case Pending::Kind::Database: {
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
                break;
            }
            case Pending::Kind::Preview: {
                // Taken rather than read back: the bytes never touched a
                // filesystem, which is the point of fetching a 900-byte blob
                // over dbserver instead of a 157 kB file over NFS.
                QByteArray blob;
                if (error.isEmpty()) {
                    const auto bytes =
                            (*m_pImpl->pSession)->take_waveform_preview(event.transfer);
                    blob = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<qsizetype>(bytes.size()));
                }
                emit previewFetched(
                        pending.mac, pending.slot, pending.trackId, blob, error);
                break;
            }
            case Pending::Kind::Artwork:
                emit artworkFetched(pending.localPath, error);
                break;
            case Pending::Kind::File:
                emit fileFetched(pending.localPath, error);
                break;
            }
            break;
        }
        default:
            // Beats, player state and tempo master are read from the tables
            // below rather than acted on here.
            break;
        }
    }

    m_pSync->update();
    syncAnnouncement();
    syncServeStatus();
}

void ProLinkNetworkService::syncServeStatus() {
    const ::prolink::ServeStatus fresh = (*m_pImpl->pSession)->serve_status();

    ServeStatus status;
    status.active = fresh.active;
    status.deviceNumber = static_cast<int>(fresh.device_number);
    status.address = QHostAddress(toQString(fresh.address));
    status.interfaceName = toQString(fresh.interface);
    status.portmapPort = fresh.portmap_port;
    status.mountdPort = fresh.mount_port;
    status.nfsdPort = fresh.nfs_port;
    status.dbserverPort = fresh.dbserver_port;
    for (const ::prolink::ServedSlot& slot : fresh.media) {
        ServedSlot served;
        served.slot = toMixxxSlot(slot.slot);
        served.exportPath = toQString(slot.export_path);
        served.volumeName = toQString(slot.volume_name);
        served.localPath = toQString(slot.local_path);
        served.trackCount = static_cast<int>(slot.track_count);
        served.playlistCount = static_cast<int>(slot.playlist_count);
        served.phantom = slot.phantom;
        status.media.append(served);
    }
    for (const ::prolink::ServeConsumer& reader : fresh.consumers) {
        ServeConsumer consumer;
        consumer.deviceNumber = static_cast<int>(reader.device_number);
        consumer.slot = toMixxxSlot(reader.slot);
        consumer.trackId = reader.track_id;
        consumer.playing = reader.playing;
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
    if (mixxx::prolink::isOurPlayerNumber(number)) {
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

    // Diffed rather than replaced wholesale, because the media registry acts
    // on found, changed and lost: a device lost takes its media and their rows
    // with it, which thirty times a second would mean reading every remote
    // medium again thirty times a second.
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
