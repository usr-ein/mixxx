#include "network/prolink/server/prolinkserver.h"

#include <QElapsedTimer>

#include "moc_prolinkserver.cpp"
#include "network/prolink/server/prolinkdbserverd.h"
#include "network/prolink/server/prolinkmediawatcher.h"
#include "network/prolink/server/prolinknfsserver.h"
#include "network/prolink/server/prolinkservedmedium.h"
#include "network/prolink/server/prolinkstatusserver.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkServer");
} // namespace

namespace mixxx {
namespace prolink {
namespace server {

ProLinkServer::ProLinkServer(QUdpSocket* pStatusSocket, QObject* parent)
        : QObject(parent), m_pStatusSocket(pStatusSocket) {
    // Built here rather than in start(), so statusServer() is valid from the
    // moment this object exists. It was created in start() at first, and the
    // caller — reasonably — wired the socket's read path to statusServer()
    // before calling start(); it got a null pointer, every media query went
    // unanswered, and the decks never listed us. Nothing failed and nothing was
    // logged, because "no query arrived" and "the query went nowhere" look
    // identical from here.
    m_pStatus = new ProLinkStatusServer(m_pStatusSocket, this);
    connect(m_pStatus,
            &ProLinkStatusServer::consumersChanged,
            this,
            &ProLinkServer::statusChanged);
}

ProLinkServer::~ProLinkServer() = default;

bool ProLinkServer::start() {
    if (m_pNfs) {
        return true;
    }
    m_pNfs = new ProLinkNfsServer(this);
    m_pDb = new ProLinkDbServer(this);
    m_pWatcher = new ProLinkMediaWatcher(this);

    connect(m_pWatcher,
            &ProLinkMediaWatcher::mediumMounted,
            this,
            &ProLinkServer::onMediumMounted);
    connect(m_pWatcher,
            &ProLinkMediaWatcher::mediumUnmounted,
            this,
            &ProLinkServer::onMediumUnmounted);

    const bool nfsUp = m_pNfs->start();
    m_pDb->start();
    m_pStatus->start();
    m_pWatcher->start();
    return nfsUp;
}

void ProLinkServer::stop() {
    if (m_pWatcher) {
        m_pWatcher->stop();
    }
    if (m_pStatus) {
        m_pStatus->stop();
    }
    if (m_pDb) {
        m_pDb->stop();
    }
    if (m_pNfs) {
        m_pNfs->stop();
    }
}

void ProLinkServer::setDeviceNumber(int number) {
    m_deviceNumber = number;
    if (m_pStatus) {
        m_pStatus->setDeviceNumber(number);
    }
    if (m_pDb) {
        m_pDb->setDeviceNumber(number);
    }
}

void ProLinkServer::setPeers(const QList<QHostAddress>& peers) {
    if (m_pStatus) {
        m_pStatus->setPeers(peers);
    }
}

void ProLinkServer::onMediumMounted(
        MediaSlot slot, const QString& path, const QString& volumeName) {
    // Reading export.pdb blocks this thread. It is a megabyte off a USB stick
    // and tens of milliseconds of Kaitai walk -- comfortably inside the 2 s
    // keep-alive period, and the alternative (hopping to a worker and back)
    // would buy nothing but a window in which the medium is half-registered.
    QElapsedTimer timer;
    timer.start();
    ProLinkServedMedium medium = ProLinkServedMedium::fromVolume(slot, path);
    if (!medium.isValid()) {
        kLogger.info() << path << "is not a usable rekordbox medium; not serving it";
        return;
    }

    // NFS before dbserver, and dbserver before the status flag, because that is
    // the order a deck uses them in: it mounts, it browses, and it will only do
    // either once status says the slot holds something.
    m_pNfs->addExport(slot, path);
    m_pDb->setMedium(slot, medium);
    m_media.insert(static_cast<int>(slot), medium);

    SlotAdvertisement advertisement;
    advertisement.occupied = true;
    // The stick's own label rather than the mount point's name, so a deck shows
    // the DJ what is written on the stick.
    advertisement.volumeName = volumeName;
    advertisement.trackCount = static_cast<quint32>(medium.trackCount());
    advertisement.playlistCount = static_cast<quint32>(medium.playlistCount());
    advertisement.settings = medium.settings();
    m_pStatus->setSlot(slot, advertisement);

    kLogger.info() << "serving" << volumeName << "in slot" << static_cast<int>(slot)
                   << "as" << exportPathForSlot(slot) << "-" << medium.trackCount()
                   << "tracks," << medium.playlistCount() << "playlists, indexed in"
                   << timer.elapsed() << "ms";
    emit statusChanged();
}

void ProLinkServer::onMediumUnmounted(MediaSlot slot) {
    // Reverse order: stop advertising it first, so a deck that reads our next
    // status packet learns the slot is empty before it can be told a filehandle
    // into it has gone stale. The two race either way, but this way round the
    // deck's own UI is what updates first.
    if (m_pStatus) {
        m_pStatus->clearSlot(slot);
    }
    if (m_pDb) {
        m_pDb->removeMedium(slot);
    }
    if (m_pNfs) {
        m_pNfs->removeExport(slot);
    }
    m_media.remove(static_cast<int>(slot));
    kLogger.info() << "slot" << static_cast<int>(slot) << "is now empty";
    emit statusChanged();
}

void ProLinkServer::setIdentity(const QHostAddress& address, const QString& interfaceName) {
    m_address = address;
    m_interfaceName = interfaceName;
}

ServeStatus ProLinkServer::status() const {
    ServeStatus out;
    out.deviceNumber = m_deviceNumber;
    out.active = m_deviceNumber > 0;
    out.deviceName = QString::fromLatin1(kVirtualCdjName);
    out.address = m_address;
    out.interfaceName = m_interfaceName;
    if (!m_pNfs) {
        return out;
    }

    out.portmapPort = m_pNfs->portmapAvailable() ? kPortmapPort : 0;
    out.mountdPort = m_pNfs->mountdPort();
    out.nfsdPort = m_pNfs->nfsdPort();
    out.dbserverPort = m_pDb ? m_pDb->port() : 0;

    for (auto it = m_media.constBegin(); it != m_media.constEnd(); ++it) {
        const ProLinkServedMedium& medium = it.value();
        ServedSlot served;
        served.slot = medium.slot();
        served.exportPath = QString::fromLatin1(exportPathForSlot(medium.slot()));
        served.volumeName = medium.volumeName();
        served.localPath = medium.rootPath();
        served.trackCount = medium.trackCount();
        served.playlistCount = medium.playlistCount();
        out.media.append(served);
    }

    if (m_pStatus) {
        out.mediaQueriesAnswered = m_pStatus->mediaQueriesAnswered();
        // Name each consumer's track. The status server sees an id and a slot
        // and nothing else; the libraries live here, so this is the only place
        // that can turn `track 0xc8` into something a DJ recognises.
        for (ServeConsumer consumer : m_pStatus->consumers()) {
            const auto medium = m_media.constFind(static_cast<int>(consumer.slot));
            if (medium != m_media.constEnd()) {
                if (const PdbTrack* pTrack = medium->track(consumer.trackId)) {
                    consumer.title = pTrack->title;
                    consumer.artist = pTrack->artist;
                }
            }
            out.consumers.append(consumer);
        }
    }

    const QHash<QString, int> rpc = m_pNfs->statistics();
    out.mountCalls = rpc.value(QStringLiteral("mountd:MNT"));
    out.readCalls = rpc.value(QStringLiteral("nfsd:READ"));
    if (m_pDb) {
        out.dbserverClients = m_pDb->statistics().value(QStringLiteral("0x0000"));
    }
    return out;
}

} // namespace server
} // namespace prolink
} // namespace mixxx
