#pragma once

#include <QByteArray>
#include <QString>

#include "network/prolink/rpc/rpcdefs.h"
#include "network/prolink/rpc/xdrbuffer.h"

namespace mixxx {
namespace prolink {
namespace rpc {

/// `AUTH_UNIX` credentials (RFC 1057 §9.2).
///
/// A CDJ's exports are readable by the whole link-local subnet, so these are not
/// really checked — but both working reference clients send `AUTH_UNIX`, nobody
/// has ever demonstrated `AUTH_NULL` working, and libcdj's one recorded attempt
/// hit `NFSERR_ACCES` while using glibc's `clnt_create()`, which defaults to
/// `AUTH_NULL`. Sending `AUTH_UNIX` is therefore the cheap, known-good choice.
///
/// **The stamp is a per-call nonce, not a magic constant.** Documentation
/// described it as a fixed value; every call in the reference capture carries a
/// different one (C8). It is generated fresh per call.
struct AuthUnix {
    quint32 stamp = 0;
    /// Empty on every call we have observed.
    QString machineName;
    quint32 uid = 0;
    quint32 gid = 0;
    // No supplementary gids, matching what a player sends.

    QByteArray encode() const;
};

/// One RPC call, ready to hand to a socket.
QByteArray buildCall(quint32 xid,
        quint32 program,
        quint32 version,
        quint32 procedure,
        const AuthUnix& credentials,
        const QByteArray& arguments);

/// The decoded head of a reply. `results` is whatever followed the header, left
/// for the per-program decoder.
struct Reply {
    quint32 xid = 0;
    bool accepted = false;
    AcceptStat acceptStat = AcceptStat::SystemErr;
    RejectStat rejectStat = RejectStat::AuthError;
    /// Populated on a version mismatch, which carries the range the server does
    /// support. That is the difference between "wrong version" and "not there at
    /// all" — worth distinguishing in a log, since one is our bug and the other
    /// is a device that cannot do what we are asking.
    quint32 lowVersion = 0;
    quint32 highVersion = 0;
    QByteArray results;

    bool isOk() const {
        return accepted && acceptStat == AcceptStat::Success;
    }
    /// Human-readable failure, for logs. Empty when isOk().
    QString errorString() const;
};

/// Decode a reply header. Returns false if *datagram* is not a well-formed RPC
/// reply at all, as distinct from a well-formed reply reporting an error.
bool parseReply(const QByteArray& datagram, Reply* pReply);

/// A fresh nonce for the AUTH_UNIX stamp.
quint32 randomStamp();

} // namespace rpc
} // namespace prolink
} // namespace mixxx
