#include "network/prolink/prolinknetworkservice.h"

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTimer>
#include <exception>

#include "moc_prolinknetworkservice.cpp"
#include "prolink-cxx/src/lib.rs.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNetworkService");

/// How often the library's events are drained and its tables re-read.
///
/// A beat at 145 BPM is 414 ms apart and the phase meter interpolates between
/// them, so this only has to be fast enough that a beat is not *late* — it is
/// not what is being drawn. 50 ms costs one queue drain and two vector reads
/// twenty times a second.
constexpr int kPollIntervalMs = 50;

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
          m_pImpl(std::make_unique<Impl>()) {
}

ProLinkNetworkService::~ProLinkNetworkService() {
    shutdown();
}

void ProLinkNetworkService::start() {
    if (m_pImpl->pSession) {
        return;
    }
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
        emit listeningChanged(false, m_lastError);
        return;
    }

    m_listening = true;
    m_lastError.clear();
    m_announcedNumber = static_cast<int>((*m_pImpl->pSession)->device_number());
    m_announceDetail = m_announcedNumber > 0
            ? tr("announced as player %1").arg(m_announcedNumber)
            : tr("listening without a player number");
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
    (*m_pImpl->pSession)->refresh();
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

    // Artwork comes over the dbserver connection rather than NFS: it is small,
    // and this blocks for one round trip rather than returning an id.
    const QByteArray local = localPath.toUtf8();
    try {
        (*m_pImpl->pSession)
                ->fetch_artwork(static_cast<::std::uint8_t>(number),
                        toRustSlot(slot),
                        artworkId,
                        ::rust::Str(local.constData(), local.size()));
        emit artworkFetched(localPath, QString());
    } catch (const std::exception& error) {
        emit artworkFetched(localPath, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::poll() {
    if (!m_pImpl->pSession) {
        return;
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
            if (found != m_pending.constEnd()) {
                emit fileFetchProgress(found->localPath,
                        static_cast<quint32>(event.done),
                        static_cast<quint32>(event.total));
            }
            break;
        }
        case ::prolink::EventKind::TransferDone: {
            const Pending pending = m_pending.take(event.transfer);
            const QString error = event.ok ? QString() : toQString(event.detail);
            if (!error.isEmpty()) {
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
