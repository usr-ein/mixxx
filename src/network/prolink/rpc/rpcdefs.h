#pragma once

#include <QtGlobal>

/// ONC RPC v2 (RFC 1057) constants, and the three programs a CDJ exposes.
namespace mixxx {
namespace prolink {
namespace rpc {

constexpr quint32 kRpcVersion = 2;

enum class MsgType : quint32 {
    Call = 0,
    Reply = 1,
};

enum class ReplyStat : quint32 {
    Accepted = 0,
    Denied = 1,
};

enum class AcceptStat : quint32 {
    Success = 0,
    ProgUnavail = 1,
    ProgMismatch = 2,
    ProcUnavail = 3,
    GarbageArgs = 4,
    SystemErr = 5,
};

enum class RejectStat : quint32 {
    RpcMismatch = 0,
    AuthError = 1,
};

enum class AuthFlavor : quint32 {
    Null = 0,
    Unix = 1,
    Short = 2,
};

// -- the programs --------------------------------------------------------------

constexpr quint32 kPortmapProgram = 100000;
constexpr quint32 kPortmapVersion = 2;
enum class PortmapProc : quint32 {
    Null = 0,
    Set = 1,
    Unset = 2,
    GetPort = 3,
    Dump = 4,
};

constexpr quint32 kMountProgram = 100005;
constexpr quint32 kMountVersion = 1;
enum class MountProc : quint32 {
    Null = 0,
    Mnt = 1,
    Dump = 2,
    Umnt = 3,
    UmntAll = 4,
    Export = 5,
};

constexpr quint32 kNfsProgram = 100003;
constexpr quint32 kNfsVersion = 2;
enum class NfsProc : quint32 {
    Null = 0,
    GetAttr = 1,
    SetAttr = 2,
    Root = 3, // obsolete
    Lookup = 4,
    ReadLink = 5,
    Read = 6,
    WriteCache = 7, // obsolete
    Write = 8,
    Create = 9,
    Remove = 10,
    Rename = 11,
    Link = 12,
    SymLink = 13,
    MkDir = 14,
    RmDir = 15,
    ReadDir = 16,
    StatFs = 17,
};

constexpr quint32 kIpProtoTcp = 6;
constexpr quint32 kIpProtoUdp = 17;

/// NFSv2 filehandles are exactly 32 opaque bytes (RFC 1094).
constexpr int kFileHandleSize = 32;

/// NFSv2's ceiling on a single READ payload.
constexpr int kNfsMaxData = 8192;

/// NFSv2 error codes we actually act on.
enum class NfsStat : quint32 {
    Ok = 0,
    Perm = 1,
    NoEnt = 2,
    Io = 5,
    NxIo = 6,
    Acces = 13,
    IsDir = 21,
    /// The filehandle no longer refers to anything — what a player returns after
    /// its media has been swapped. Treated as "re-mount and retry once" rather
    /// than as a hard failure.
    Stale = 70,
};

} // namespace rpc
} // namespace prolink
} // namespace mixxx
