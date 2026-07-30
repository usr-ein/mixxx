#include "network/prolink/server/prolinknfsserver.h"

#include <kaitai/exceptions.h>
#include <kaitai/kaitaistream.h>

#include <QNetworkDatagram>
#include <QUdpSocket>
#include <exception>
#include <sstream>

#include "moc_prolinknfsserver.cpp"
#include "network/prolink/rpc/rpcdefs.h"
#include "network/prolink/rpc/rpcmessage.h"
#include "network/prolink/rpc/xdrbuffer.h"
#include "prolink_rpc.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNfsServer");

using namespace mixxx::prolink;
using namespace mixxx::prolink::server;

/// The ports a real player answers on. Not registered numbers, but three
/// independent observations across three devices agreed (F6), which makes them
/// worth taking when free: a deck that skips the portmapper then still finds us.
constexpr quint16 kPreferredMountdPort = 48276;
constexpr quint16 kPreferredNfsdPort = 2049;

/// Cap on a single READ payload. NFSv2's own ceiling is 8192; a player asks for
/// far less (1280 is typical) and anything larger would fragment on a 1500-byte
/// MTU anyway.
constexpr quint32 kMaxReadCount = 8192;

/// Attributes we synthesise. A rekordbox export is read-only and ownerless as
/// far as a deck is concerned, and nothing on the wire consults these beyond the
/// type and the size — but `size` decides how many READs a client issues and
/// when it stops, so getting *that* wrong truncates or hangs a transfer.
constexpr quint32 kBlockSize = 512;
constexpr quint32 kDirMode = 0040755;
constexpr quint32 kFileMode = 0100644;

QString describeProcedure(quint32 program, quint32 procedure) {
    switch (program) {
    case rpc::kPortmapProgram:
        switch (static_cast<rpc::PortmapProc>(procedure)) {
        case rpc::PortmapProc::Null:
            return QStringLiteral("portmap:NULL");
        case rpc::PortmapProc::GetPort:
            return QStringLiteral("portmap:GETPORT");
        case rpc::PortmapProc::Dump:
            return QStringLiteral("portmap:DUMP");
        default:
            break;
        }
        return QStringLiteral("portmap:%1").arg(procedure);
    case rpc::kMountProgram:
        switch (static_cast<rpc::MountProc>(procedure)) {
        case rpc::MountProc::Null:
            return QStringLiteral("mountd:NULL");
        case rpc::MountProc::Mnt:
            return QStringLiteral("mountd:MNT");
        case rpc::MountProc::Umnt:
            return QStringLiteral("mountd:UMNT");
        case rpc::MountProc::UmntAll:
            return QStringLiteral("mountd:UMNTALL");
        case rpc::MountProc::Export:
            return QStringLiteral("mountd:EXPORT");
        default:
            break;
        }
        return QStringLiteral("mountd:%1").arg(procedure);
    case rpc::kNfsProgram:
        switch (static_cast<rpc::NfsProc>(procedure)) {
        case rpc::NfsProc::Null:
            return QStringLiteral("nfsd:NULL");
        case rpc::NfsProc::GetAttr:
            return QStringLiteral("nfsd:GETATTR");
        case rpc::NfsProc::Lookup:
            return QStringLiteral("nfsd:LOOKUP");
        case rpc::NfsProc::Read:
            return QStringLiteral("nfsd:READ");
        case rpc::NfsProc::ReadDir:
            return QStringLiteral("nfsd:READDIR");
        case rpc::NfsProc::StatFs:
            return QStringLiteral("nfsd:STATFS");
        default:
            break;
        }
        return QStringLiteral("nfsd:%1").arg(procedure);
    default:
        break;
    }
    return QStringLiteral("%1:%2").arg(program).arg(procedure);
}

QByteArray toQt(const std::string& value) {
    return QByteArray(value.data(), static_cast<int>(value.size()));
}

/// Pioneer's UTF-16LE, counted in bytes. Not the UTF-16BE the dbserver layer
/// uses, and not ASCII as standard NFS would have it.
QString decodeUtf16Le(const std::string& raw) {
    QString text;
    text.reserve(static_cast<int>(raw.size()) / 2);
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        const char16_t unit = static_cast<char16_t>(
                static_cast<quint8>(raw[i]) |
                (static_cast<quint8>(raw[i + 1]) << 8));
        if (unit == 0) {
            break;
        }
        text.append(QChar(unit));
    }
    return text;
}

/// The 17 consecutive 32-bit words of an NFSv2 `fattr`.
void appendAttributes(XdrWriter* pWriter, const ProLinkVfs::Node& node) {
    const quint32 size = node.isDirectory
            ? 0u
            : static_cast<quint32>(qMin<qint64>(node.size, 0xFFFFFFFFll));
    const quint32 mtime = static_cast<quint32>(node.modifiedSecs);
    pWriter->u32(node.isDirectory ? 2u : 1u); // NFDIR / NFREG
    pWriter->u32(node.isDirectory ? kDirMode : kFileMode);
    pWriter->u32(node.isDirectory ? 2u : 1u); // nlink
    pWriter->u32(0);                          // uid
    pWriter->u32(0);                          // gid
    pWriter->u32(size);
    pWriter->u32(kBlockSize);
    pWriter->u32(0); // rdev
    pWriter->u32((size + kBlockSize - 1) / kBlockSize);
    pWriter->u32(1); // fsid
    pWriter->u32(node.fileId);
    for (int i = 0; i < 3; ++i) { // atime, mtime, ctime
        pWriter->u32(mtime);
        pWriter->u32(0);
    }
}

QByteArray nfsError(rpc::NfsStat status) {
    XdrWriter writer;
    writer.u32(static_cast<quint32>(status));
    return writer.data();
}

/// Bind *pSocket* to *preferred*, falling back to an ephemeral port.
///
/// The fallback matters because the well-known ports may be held by a real
/// `rpcbind`/`nfsd` on the same host. Losing the preferred number only costs us
/// the deck that skips the portmapper; failing to bind at all would cost
/// everything.
quint16 bindPreferred(QUdpSocket* pSocket, quint16 preferred) {
    if (pSocket->bind(QHostAddress::AnyIPv4, preferred)) {
        return preferred;
    }
    if (pSocket->bind(QHostAddress::AnyIPv4, 0)) {
        kLogger.info() << "port" << preferred << "was taken; using"
                       << pSocket->localPort() << "instead";
        return pSocket->localPort();
    }
    return 0;
}

} // namespace

namespace mixxx {
namespace prolink {
namespace server {

ProLinkNfsServer::ProLinkNfsServer(QObject* parent)
        : QObject(parent) {
}

ProLinkNfsServer::~ProLinkNfsServer() = default;

bool ProLinkNfsServer::start() {
    if (m_pNfsd) {
        return m_nfsdPort != 0;
    }
    m_pPortmap = new QUdpSocket(this);
    m_pMountd = new QUdpSocket(this);
    m_pNfsd = new QUdpSocket(this);
    connect(m_pPortmap, &QUdpSocket::readyRead, this, &ProLinkNfsServer::readPortmap);
    connect(m_pMountd, &QUdpSocket::readyRead, this, &ProLinkNfsServer::readMountd);
    connect(m_pNfsd, &QUdpSocket::readyRead, this, &ProLinkNfsServer::readNfsd);

    m_mountdPort = bindPreferred(m_pMountd, kPreferredMountdPort);
    m_nfsdPort = bindPreferred(m_pNfsd, kPreferredNfsdPort);
    if (m_mountdPort == 0 || m_nfsdPort == 0) {
        kLogger.warning() << "could not bind mountd/nfsd; not serving files";
        return false;
    }

    if (m_pPortmap->bind(QHostAddress::AnyIPv4, kPortmapPort)) {
        m_portmapPort = kPortmapPort;
    } else {
        // Not fatal, but it very nearly is: a deck asks here first and retries
        // forever rather than guessing (F46), so without this almost nothing
        // downstream will happen.
        m_portmapPort = 0;
        kLogger.warning() << "could not bind UDP" << kPortmapPort << ":"
                          << m_pPortmap->errorString()
                          << "- players will not find our exports. On Linux set "
                             "net.ipv4.ip_unprivileged_port_start=111.";
    }
    kLogger.info() << "NFS server up: portmap" << m_portmapPort << "mountd"
                   << m_mountdPort << "nfsd" << m_nfsdPort;
    return true;
}

void ProLinkNfsServer::stop() {
    for (QUdpSocket* pSocket : {m_pPortmap, m_pMountd, m_pNfsd}) {
        if (pSocket) {
            pSocket->close();
        }
    }
    m_portmapPort = m_mountdPort = m_nfsdPort = 0;
}

void ProLinkNfsServer::addExport(MediaSlot slot, const QString& directory) {
    const QString exportPath = QString::fromLatin1(exportPathForSlot(slot));
    if (exportPath.isEmpty()) {
        return;
    }
    // `/C/` becomes the subtree `C`, so the two can never disagree.
    const QString prefix = exportPath.mid(1, exportPath.size() - 2);
    m_vfs.mount(prefix, directory);
    m_exports.insert(exportPath, prefix);
}

void ProLinkNfsServer::removeExport(MediaSlot slot) {
    const QString exportPath = QString::fromLatin1(exportPathForSlot(slot));
    const auto found = m_exports.constFind(exportPath);
    if (found == m_exports.constEnd()) {
        return;
    }
    m_vfs.unmount(found.value());
    m_exports.remove(exportPath);
}

QString ProLinkNfsServer::subtreeForExport(const QString& exportPath) const {
    const auto found = m_exports.constFind(exportPath);
    if (found != m_exports.constEnd()) {
        return QStringLiteral("/") + found.value();
    }
    // A deck may mount `/C/EXPORT` where we advertise `/C/` — both spellings
    // have been seen for the USB slot on different hardware (C6). Match on the
    // prefix so the variation costs nothing.
    for (auto it = m_exports.constBegin(); it != m_exports.constEnd(); ++it) {
        if (exportPath.startsWith(it.key())) {
            return QStringLiteral("/") + it.value();
        }
    }
    return QString();
}

void ProLinkNfsServer::readPortmap() {
    drain(m_pPortmap);
}

void ProLinkNfsServer::readMountd() {
    drain(m_pMountd);
}

void ProLinkNfsServer::readNfsd() {
    drain(m_pNfsd);
}

void ProLinkNfsServer::drain(QUdpSocket* pSocket) {
    while (pSocket && pSocket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = pSocket->receiveDatagram();
        const QByteArray reply = handleCall(datagram.data());
        if (reply.isEmpty()) {
            continue;
        }
        pSocket->writeDatagram(reply, datagram.senderAddress(), datagram.senderPort());
    }
}

QByteArray ProLinkNfsServer::handleCall(const QByteArray& datagram) {
    // Parsed by the generated Kaitai reader, which is where the framing, the
    // argument bodies and the length caps are specified. A malformed or
    // hostile datagram costs an exception and nothing else -- notably no
    // allocation, because every length prefix in the schema is bounded.
    std::string buffer(datagram.constData(), static_cast<size_t>(datagram.size()));
    std::istringstream stream(buffer);
    try {
        kaitai::kstream ks(&stream);
        prolink_rpc_t call(&ks);

        const quint32 program = static_cast<quint32>(call.program());
        const quint32 procedure = call.procedure();
        const QString key = describeProcedure(program, procedure);
        m_statistics[key] = m_statistics.value(key, 0) + 1;

        switch (program) {
        case rpc::kPortmapProgram:
            return dispatchPortmap(&call);
        case rpc::kMountProgram:
            return dispatchMount(&call);
        case rpc::kNfsProgram:
            return dispatchNfs(&call);
        default:
            break;
        }
        return rpc::buildReply(
                call.xid(), QByteArray(), rpc::AcceptStat::ProgUnavail);
    } catch (const std::exception& e) {
        // Not worth a warning: our ports see stray traffic, and a reply that
        // wandered in is a call we cannot answer rather than a problem.
        kLogger.debug() << "undecodable RPC call:" << e.what();
        return QByteArray();
    }
}

QByteArray ProLinkNfsServer::dispatchPortmap(prolink_rpc_t* pCall) {
    const quint32 xid = pCall->xid();
    switch (static_cast<rpc::PortmapProc>(pCall->procedure())) {
    case rpc::PortmapProc::Null:
        return rpc::buildReply(xid);
    case rpc::PortmapProc::GetPort: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::getport_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        quint16 port = 0;
        const quint32 wanted = static_cast<quint32>(pArgs->program());
        if (wanted == rpc::kMountProgram && pArgs->program_version() == rpc::kMountVersion) {
            port = m_mountdPort;
        } else if (wanted == rpc::kNfsProgram && pArgs->program_version() == rpc::kNfsVersion) {
            port = m_nfsdPort;
        } else if (wanted == rpc::kPortmapProgram) {
            port = m_portmapPort;
        }
        // Zero is the protocol's own "not registered", so an unknown program
        // needs no special case.
        XdrWriter writer;
        writer.u32(port);
        return rpc::buildReply(xid, writer.data());
    }
    case rpc::PortmapProc::Dump: {
        XdrWriter writer;
        const struct {
            quint32 program;
            quint32 version;
            quint16 port;
        } mappings[] = {
                {rpc::kPortmapProgram, rpc::kPortmapVersion, m_portmapPort},
                {rpc::kMountProgram, rpc::kMountVersion, m_mountdPort},
                {rpc::kNfsProgram, rpc::kNfsVersion, m_nfsdPort},
        };
        for (const auto& mapping : mappings) {
            if (mapping.port == 0) {
                continue;
            }
            writer.boolean(true);
            writer.u32(mapping.program);
            writer.u32(mapping.version);
            writer.u32(rpc::kIpProtoUdp);
            writer.u32(mapping.port);
        }
        writer.boolean(false);
        return rpc::buildReply(xid, writer.data());
    }
    default:
        break;
    }
    return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::ProcUnavail);
}

QByteArray ProLinkNfsServer::dispatchMount(prolink_rpc_t* pCall) {
    const quint32 xid = pCall->xid();
    switch (static_cast<rpc::MountProc>(pCall->procedure())) {
    case rpc::MountProc::Null:
        return rpc::buildReply(xid);
    case rpc::MountProc::Mnt: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::path_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        const QString path = decodeUtf16Le(pArgs->path()->value());
        const QString subtree = subtreeForExport(path);
        XdrWriter writer;
        if (subtree.isEmpty()) {
            // What a real player answers for an empty slot, and what our own
            // client reads as "nothing in there" (F48).
            kLogger.debug() << "MNT" << path << "- no such export";
            writer.u32(static_cast<quint32>(rpc::NfsStat::NoEnt));
            return rpc::buildReply(xid, writer.data());
        }
        kLogger.info() << "MNT" << path << "->" << subtree;
        writer.u32(static_cast<quint32>(rpc::NfsStat::Ok));
        // The handle for the *mapped* subtree, not for the VFS root: that is
        // what keeps two media's filehandles distinct, since a handle is a hash
        // of the path and a deck keeps only its first 12 bytes (F28).
        writer.opaqueFixed(ProLinkVfs::handleFor(subtree));
        return rpc::buildReply(xid, writer.data());
    }
    case rpc::MountProc::Export: {
        XdrWriter writer;
        for (auto it = m_exports.constBegin(); it != m_exports.constEnd(); ++it) {
            writer.boolean(true);
            writer.stringUtf16Le(it.key());
            // The group list: which hosts may mount. A real player names the
            // peers it has discovered; we export to anyone who can reach us,
            // which is an empty list.
            writer.boolean(false);
        }
        writer.boolean(false);
        return rpc::buildReply(xid, writer.data());
    }
    case rpc::MountProc::Umnt:
    case rpc::MountProc::UmntAll:
        // Nothing to release: handles are derived from paths, not allocated per
        // client, so there is no per-mount state to forget.
        return rpc::buildReply(xid);
    default:
        break;
    }
    return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::ProcUnavail);
}

QByteArray ProLinkNfsServer::dispatchNfs(prolink_rpc_t* pCall) {
    const quint32 xid = pCall->xid();
    switch (static_cast<rpc::NfsProc>(pCall->procedure())) {
    case rpc::NfsProc::Null:
        return rpc::buildReply(xid);

    case rpc::NfsProc::GetAttr: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::fhandle_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        ProLinkVfs::Node node;
        if (!m_vfs.resolve(toQt(pArgs->fhandle()), &node)) {
            return rpc::buildReply(xid, nfsError(rpc::NfsStat::Stale));
        }
        XdrWriter writer;
        writer.u32(static_cast<quint32>(rpc::NfsStat::Ok));
        appendAttributes(&writer, node);
        return rpc::buildReply(xid, writer.data());
    }

    case rpc::NfsProc::Lookup: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::lookup_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        const QByteArray parent = toQt(pArgs->dir_fhandle());
        const QString name = decodeUtf16Le(pArgs->name()->value());
        QByteArray childHandle;
        ProLinkVfs::Node child;
        if (!m_vfs.lookup(parent, name, &childHandle, &child)) {
            // Distinguish "you gave me a handle I never issued" from "that
            // directory has no such child": the first tells a deck to re-mount,
            // the second that the file is gone.
            const bool parentKnown = m_vfs.resolve(parent, nullptr);
            return rpc::buildReply(xid,
                    nfsError(parentKnown ? rpc::NfsStat::NoEnt : rpc::NfsStat::Stale));
        }
        XdrWriter writer;
        writer.u32(static_cast<quint32>(rpc::NfsStat::Ok));
        writer.opaqueFixed(childHandle);
        appendAttributes(&writer, child);
        return rpc::buildReply(xid, writer.data());
    }

    case rpc::NfsProc::Read: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::read_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        const QByteArray handle = toQt(pArgs->fhandle());
        ProLinkVfs::Node node;
        if (!m_vfs.resolve(handle, &node)) {
            return rpc::buildReply(xid, nfsError(rpc::NfsStat::Stale));
        }
        if (node.isDirectory) {
            return rpc::buildReply(xid, nfsError(rpc::NfsStat::IsDir));
        }
        const QByteArray payload = m_vfs.read(
                handle, pArgs->offset(), qMin(pArgs->count(), kMaxReadCount));
        XdrWriter writer;
        writer.u32(static_cast<quint32>(rpc::NfsStat::Ok));
        appendAttributes(&writer, node);
        writer.opaqueVar(payload);
        return rpc::buildReply(xid, writer.data());
    }

    case rpc::NfsProc::ReadDir: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::readdir_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        const QByteArray handle = toQt(pArgs->fhandle());
        ProLinkVfs::Node node;
        if (!m_vfs.resolve(handle, &node)) {
            return rpc::buildReply(xid, nfsError(rpc::NfsStat::Stale));
        }
        if (!node.isDirectory) {
            return rpc::buildReply(xid, nfsError(rpc::NfsStat::NotDir));
        }
        XdrWriter writer;
        writer.u32(static_cast<quint32>(rpc::NfsStat::Ok));
        int index = 0;
        for (const ProLinkVfs::Node& child : m_vfs.readDirectory(handle)) {
            writer.boolean(true);
            writer.u32(child.fileId);
            writer.stringUtf16Le(child.path.section(QLatin1Char('/'), -1));
            // The cookie is opaque to the client and minted by us, so a plain
            // index is as good as anything -- and we answer in one shot.
            writer.u32(static_cast<quint32>(++index));
        }
        writer.boolean(false);
        writer.boolean(true); // eof
        return rpc::buildReply(xid, writer.data());
    }

    case rpc::NfsProc::StatFs: {
        const auto* pArgs = dynamic_cast<prolink_rpc_t::fhandle_args_t*>(pCall->arguments());
        if (!pArgs) {
            return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::GarbageArgs);
        }
        if (!m_vfs.resolve(toQt(pArgs->fhandle()), nullptr)) {
            return rpc::buildReply(xid, nfsError(rpc::NfsStat::Stale));
        }
        XdrWriter writer;
        writer.u32(static_cast<quint32>(rpc::NfsStat::Ok));
        // tsize, bsize, blocks, bfree, bavail. A deck shows free space in its
        // Link Info panel; the medium is read-only to it, so plausible numbers
        // are enough and a real free-space query per call would not be.
        for (const quint32 value : {8192u, 512u, 1000000u, 500000u, 500000u}) {
            writer.u32(value);
        }
        return rpc::buildReply(xid, writer.data());
    }

    default:
        break;
    }
    // Everything else, including every write procedure. A player asking for one
    // gets the same answer libcdj got from a real CDJ for READDIR, which is a
    // normal thing for a client to have to handle.
    return rpc::buildReply(xid, QByteArray(), rpc::AcceptStat::ProcUnavail);
}

} // namespace server
} // namespace prolink
} // namespace mixxx
