#include "network/prolink/rpc/rpcmessage.h"

#include <QRandomGenerator>

namespace mixxx {
namespace prolink {
namespace rpc {

QByteArray AuthUnix::encode() const {
    XdrWriter writer;
    writer.u32(stamp);
    writer.stringAscii(machineName);
    writer.u32(uid);
    writer.u32(gid);
    writer.u32(0); // supplementary gid count
    return writer.data();
}

quint32 randomStamp() {
    return QRandomGenerator::global()->generate();
}

QByteArray buildCall(quint32 xid,
        quint32 program,
        quint32 version,
        quint32 procedure,
        const AuthUnix& credentials,
        const QByteArray& arguments) {
    XdrWriter writer;
    writer.u32(xid);
    writer.u32(static_cast<quint32>(MsgType::Call));
    writer.u32(kRpcVersion);
    writer.u32(program);
    writer.u32(version);
    writer.u32(procedure);
    // Credential, then verifier. The verifier is always AUTH_NULL with an empty
    // body for AUTH_UNIX -- RFC 1057 §9.2, and what a player sends.
    writer.u32(static_cast<quint32>(AuthFlavor::Unix));
    writer.opaqueVar(credentials.encode());
    writer.u32(static_cast<quint32>(AuthFlavor::Null));
    writer.opaqueVar(QByteArray());
    return writer.data() + arguments;
}

bool parseReply(const QByteArray& datagram, Reply* pReply) {
    if (!pReply) {
        return false;
    }
    XdrReader reader(datagram);
    pReply->xid = reader.u32();
    const quint32 messageType = reader.u32();
    if (!reader.isValid() || messageType != static_cast<quint32>(MsgType::Reply)) {
        return false;
    }

    const quint32 replyStat = reader.u32();
    if (!reader.isValid()) {
        return false;
    }

    if (replyStat == static_cast<quint32>(ReplyStat::Denied)) {
        pReply->accepted = false;
        pReply->rejectStat = static_cast<RejectStat>(reader.u32());
        if (pReply->rejectStat == RejectStat::RpcMismatch) {
            pReply->lowVersion = reader.u32();
            pReply->highVersion = reader.u32();
        }
        return reader.isValid();
    }
    if (replyStat != static_cast<quint32>(ReplyStat::Accepted)) {
        return false;
    }

    pReply->accepted = true;
    // The verifier comes back before the status. Skipped rather than checked:
    // it is AUTH_NULL with an empty body in everything we have seen, and a
    // client has nothing to verify it against anyway.
    reader.u32(); // verifier flavor
    reader.opaqueVar(400);
    pReply->acceptStat = static_cast<AcceptStat>(reader.u32());
    if (!reader.isValid()) {
        return false;
    }
    if (pReply->acceptStat == AcceptStat::ProgMismatch) {
        pReply->lowVersion = reader.u32();
        pReply->highVersion = reader.u32();
        return reader.isValid();
    }
    // Whatever remains is the per-program result body.
    pReply->results = datagram.mid(reader.position());
    return reader.isValid();
}

QString Reply::errorString() const {
    if (isOk()) {
        return QString();
    }
    if (!accepted) {
        switch (rejectStat) {
        case RejectStat::RpcMismatch:
            return QStringLiteral("RPC_MISMATCH (server supports v%1..v%2)")
                    .arg(lowVersion)
                    .arg(highVersion);
        case RejectStat::AuthError:
            return QStringLiteral("AUTH_ERROR");
        }
        return QStringLiteral("denied, reason %1").arg(static_cast<quint32>(rejectStat));
    }
    switch (acceptStat) {
    case AcceptStat::Success:
        return QString();
    case AcceptStat::ProgUnavail:
        return QStringLiteral("PROG_UNAVAIL");
    case AcceptStat::ProgMismatch:
        return QStringLiteral("PROG_MISMATCH (server supports v%1..v%2)")
                .arg(lowVersion)
                .arg(highVersion);
    case AcceptStat::ProcUnavail:
        // libcdj hit this on READDIR: a player does not implement every NFSv2
        // procedure, so this is a normal answer rather than a broken server.
        return QStringLiteral("PROC_UNAVAIL");
    case AcceptStat::GarbageArgs:
        return QStringLiteral("GARBAGE_ARGS");
    case AcceptStat::SystemErr:
        return QStringLiteral("SYSTEM_ERR");
    }
    return QStringLiteral("accept status %1").arg(static_cast<quint32>(acceptStat));
}

} // namespace rpc
} // namespace prolink
} // namespace mixxx
