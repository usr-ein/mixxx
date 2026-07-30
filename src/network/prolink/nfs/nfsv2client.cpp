#include "network/prolink/nfs/nfsv2client.h"

#include <QStringList>

#include "moc_nfsv2client.cpp"
#include "network/prolink/rpc/xdrbuffer.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNfs");

/// Turn an NFSv2 status code into something a log line can say.
QString nfsStatusString(quint32 status) {
    using mixxx::prolink::rpc::NfsStat;
    switch (static_cast<NfsStat>(status)) {
    case NfsStat::Ok:
        return QString();
    case NfsStat::Perm:
        return QStringLiteral("NFSERR_PERM");
    case NfsStat::NoEnt:
        return QStringLiteral("NFSERR_NOENT");
    case NfsStat::Io:
        return QStringLiteral("NFSERR_IO");
    case NfsStat::NxIo:
        return QStringLiteral("NFSERR_NXIO");
    case NfsStat::Acces:
        // On a CDJ this usually means the export list did not include us, which
        // is firmware-dependent: the players we have seen export to the whole
        // link-local subnet, but one device in dysentery's capture exported to
        // two specific hosts instead. Announcing first would be the thing to try.
        return QStringLiteral("NFSERR_ACCES");
    case NfsStat::IsDir:
        return QStringLiteral("NFSERR_ISDIR");
    case NfsStat::Stale:
        // The media was swapped. Caller should re-mount and retry once.
        return QStringLiteral("NFSERR_STALE");
    }
    return QStringLiteral("NFS status %1").arg(status);
}
} // namespace

namespace mixxx {
namespace prolink {
namespace nfs {

using rpc::RpcClient;
// XdrReader/XdrWriter are in the enclosing mixxx::prolink namespace.

namespace {
/// Decode the 17-word fattr that follows most successful replies.
FileAttributes readAttributes(XdrReader& reader) {
    FileAttributes attributes;
    attributes.type = reader.u32();
    attributes.mode = reader.u32();
    attributes.nlink = reader.u32();
    attributes.uid = reader.u32();
    attributes.gid = reader.u32();
    attributes.size = reader.u32();
    attributes.blockSize = reader.u32();
    attributes.rdev = reader.u32();
    attributes.blocks = reader.u32();
    attributes.fsid = reader.u32();
    attributes.fileId = reader.u32();
    attributes.atimeSec = reader.u32();
    attributes.atimeUsec = reader.u32();
    attributes.mtimeSec = reader.u32();
    attributes.mtimeUsec = reader.u32();
    attributes.ctimeSec = reader.u32();
    attributes.ctimeUsec = reader.u32();
    return attributes;
}
} // namespace

NfsV2Client::NfsV2Client(const QHostAddress& peer,
        const QHostAddress& localAddress,
        QObject* parent)
        : QObject(parent),
          m_rpc(peer, localAddress, this) {
}

NfsV2Client::~NfsV2Client() = default;

void NfsV2Client::abortAll() {
    m_rpc.abortAll();
}

void NfsV2Client::resolvePort(quint32 program,
        quint32 version,
        std::function<void(bool, quint16)> callback) {
    XdrWriter args;
    args.u32(program);
    args.u32(version);
    args.u32(rpc::kIpProtoUdp);
    args.u32(0);

    m_rpc.call(kPortmapPort,
            rpc::kPortmapProgram,
            rpc::kPortmapVersion,
            static_cast<quint32>(rpc::PortmapProc::GetPort),
            args.data(),
            [callback](const RpcClient::Result& result) {
                if (!result.isOk()) {
                    callback(false, 0);
                    return;
                }
                XdrReader reader(result.reply.results);
                const quint32 port = reader.u32();
                // Port 0 is the portmapper's way of saying "that program is not
                // registered here" -- a valid reply reporting absence, not an
                // error, and worth distinguishing from no reply at all.
                callback(reader.isValid() && port != 0 && port <= 0xffff,
                        static_cast<quint16>(port));
            });
}

void NfsV2Client::mount(const QString& exportPath,
        std::function<void(const Outcome<QByteArray>&)> callback) {
    resolvePort(rpc::kMountProgram,
            rpc::kMountVersion,
            [this, exportPath, callback](bool ok, quint16 port) {
                if (!ok) {
                    Outcome<QByteArray> outcome;
                    outcome.error = QStringLiteral(
                            "no mountd: the portmapper did not answer, or does "
                            "not have program 100005 registered");
                    callback(outcome);
                    return;
                }
                m_mountdPort = port;

                // nfsd's port is resolved now too rather than lazily, so a
                // successful mount() means the whole path is usable. Finding out
                // at the first READ that nfsd is missing would be a worse place
                // to discover it.
                resolvePort(rpc::kNfsProgram,
                        rpc::kNfsVersion,
                        [this, exportPath, callback](bool nfsOk, quint16 nfsPort) {
                            if (!nfsOk) {
                                Outcome<QByteArray> outcome;
                                outcome.error = QStringLiteral(
                                        "no nfsd: program 100003 is not "
                                        "registered with the portmapper");
                                callback(outcome);
                                return;
                            }
                            m_nfsdPort = nfsPort;
                            kLogger.debug() << "mountd" << m_mountdPort << "nfsd"
                                            << m_nfsdPort;

                            XdrWriter args;
                            // UTF-16LE, byte-counted. The whole reason for the
                            // hand-written XDR layer.
                            args.stringUtf16Le(exportPath);
                            m_rpc.call(m_mountdPort,
                                    rpc::kMountProgram,
                                    rpc::kMountVersion,
                                    static_cast<quint32>(rpc::MountProc::Mnt),
                                    args.data(),
                                    [callback, exportPath](
                                            const RpcClient::Result& result) {
                                        Outcome<QByteArray> outcome;
                                        if (!result.isOk()) {
                                            outcome.error =
                                                    QStringLiteral("MNT %1: %2")
                                                            .arg(exportPath,
                                                                    result.errorString());
                                            callback(outcome);
                                            return;
                                        }
                                        XdrReader reader(result.reply.results);
                                        const quint32 status = reader.u32();
                                        if (status != 0) {
                                            outcome.error =
                                                    QStringLiteral("MNT %1: %2")
                                                            .arg(exportPath,
                                                                    nfsStatusString(
                                                                            status));
                                            callback(outcome);
                                            return;
                                        }
                                        outcome.value = reader.opaqueFixed(
                                                rpc::kFileHandleSize);
                                        outcome.ok = reader.isValid() &&
                                                outcome.value.size() ==
                                                        rpc::kFileHandleSize;
                                        if (!outcome.ok) {
                                            outcome.error = QStringLiteral(
                                                    "MNT succeeded but the "
                                                    "filehandle was malformed");
                                        }
                                        callback(outcome);
                                    });
                        });
            });
}

void NfsV2Client::lookup(const QByteArray& directoryHandle,
        const QString& name,
        std::function<void(const Outcome<QByteArray>&)> callback) {
    XdrWriter args;
    args.opaqueFixed(directoryHandle);
    args.stringUtf16Le(name);

    m_rpc.call(m_nfsdPort,
            rpc::kNfsProgram,
            rpc::kNfsVersion,
            static_cast<quint32>(rpc::NfsProc::Lookup),
            args.data(),
            [callback, name](const RpcClient::Result& result) {
                Outcome<QByteArray> outcome;
                if (!result.isOk()) {
                    outcome.error = QStringLiteral("LOOKUP %1: %2")
                                            .arg(name, result.errorString());
                    callback(outcome);
                    return;
                }
                XdrReader reader(result.reply.results);
                const quint32 status = reader.u32();
                if (status != 0) {
                    outcome.error = QStringLiteral("LOOKUP %1: %2")
                                            .arg(name, nfsStatusString(status));
                    callback(outcome);
                    return;
                }
                outcome.value = reader.opaqueFixed(rpc::kFileHandleSize);
                outcome.ok = reader.isValid() &&
                        outcome.value.size() == rpc::kFileHandleSize;
                if (!outcome.ok) {
                    outcome.error = QStringLiteral("LOOKUP %1: malformed reply").arg(name);
                }
                callback(outcome);
            });
}

void NfsV2Client::resolvePath(const QByteArray& rootHandle,
        const QString& path,
        std::function<void(const Outcome<QByteArray>&)> callback) {
    const QStringList components =
            path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.isEmpty()) {
        Outcome<QByteArray> outcome;
        outcome.ok = true;
        outcome.value = rootHandle;
        callback(outcome);
        return;
    }
    resolvePathStep(rootHandle, components, 0, callback);
}

void NfsV2Client::resolvePathStep(const QByteArray& handle,
        const QStringList& components,
        int index,
        std::function<void(const Outcome<QByteArray>&)> callback) {
    lookup(handle,
            components.at(index),
            [this, components, index, callback](const Outcome<QByteArray>& step) {
                if (!step.ok) {
                    callback(step);
                    return;
                }
                if (index + 1 >= components.size()) {
                    callback(step);
                    return;
                }
                resolvePathStep(step.value, components, index + 1, callback);
            });
}

void NfsV2Client::getAttributes(const QByteArray& handle,
        std::function<void(const Outcome<FileAttributes>&)> callback) {
    XdrWriter args;
    args.opaqueFixed(handle);

    m_rpc.call(m_nfsdPort,
            rpc::kNfsProgram,
            rpc::kNfsVersion,
            static_cast<quint32>(rpc::NfsProc::GetAttr),
            args.data(),
            [callback](const RpcClient::Result& result) {
                Outcome<FileAttributes> outcome;
                if (!result.isOk()) {
                    outcome.error = QStringLiteral("GETATTR: %1").arg(result.errorString());
                    callback(outcome);
                    return;
                }
                XdrReader reader(result.reply.results);
                const quint32 status = reader.u32();
                if (status != 0) {
                    outcome.error =
                            QStringLiteral("GETATTR: %1").arg(nfsStatusString(status));
                    callback(outcome);
                    return;
                }
                outcome.value = readAttributes(reader);
                outcome.ok = reader.isValid();
                if (!outcome.ok) {
                    outcome.error = QStringLiteral("GETATTR: malformed attributes");
                }
                callback(outcome);
            });
}

void NfsV2Client::read(const QByteArray& handle,
        quint32 offset,
        quint32 count,
        std::function<void(const Outcome<ReadResult>&)> callback) {
    XdrWriter args;
    args.opaqueFixed(handle);
    args.u32(offset);
    args.u32(count);
    args.u32(0); // totalcount, unused by NFSv2 but present in the wire format

    m_rpc.call(m_nfsdPort,
            rpc::kNfsProgram,
            rpc::kNfsVersion,
            static_cast<quint32>(rpc::NfsProc::Read),
            args.data(),
            [callback, offset](const RpcClient::Result& result) {
                Outcome<ReadResult> outcome;
                if (!result.isOk()) {
                    outcome.error = QStringLiteral("READ at %1: %2")
                                            .arg(offset)
                                            .arg(result.errorString());
                    callback(outcome);
                    return;
                }
                XdrReader reader(result.reply.results);
                const quint32 status = reader.u32();
                if (status != 0) {
                    outcome.error = QStringLiteral("READ at %1: %2")
                                            .arg(offset)
                                            .arg(nfsStatusString(status));
                    callback(outcome);
                    return;
                }
                outcome.value.attributes = readAttributes(reader);
                outcome.value.data = reader.opaqueVar(rpc::kNfsMaxData);
                outcome.ok = reader.isValid();
                if (!outcome.ok) {
                    outcome.error = QStringLiteral("READ at %1: malformed reply").arg(offset);
                }
                callback(outcome);
            });
}

void NfsV2Client::unmount(const QString& exportPath) {
    if (m_mountdPort == 0) {
        return;
    }
    XdrWriter args;
    args.stringUtf16Le(exportPath);
    m_rpc.call(m_mountdPort,
            rpc::kMountProgram,
            rpc::kMountVersion,
            static_cast<quint32>(rpc::MountProc::Umnt),
            args.data(),
            [](const RpcClient::Result&) {
                // Best effort. Real players do send UMNT after an eject (C9), so
                // we behave the same way, but nothing of ours depends on it.
            });
}

} // namespace nfs
} // namespace prolink
} // namespace mixxx
