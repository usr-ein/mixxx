#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <functional>

#include "network/prolink/prolinkdefs.h"
#include "network/prolink/rpc/rpcclient.h"
#include "network/prolink/rpc/rpcdefs.h"

namespace mixxx {
namespace prolink {
namespace nfs {

/// NFSv2 `fattr` — the 17 words returned with most replies.
///
/// Only `size` and `type` are used today; the rest are decoded because they must
/// be stepped over anyway and skipping blind would hide a misalignment.
struct FileAttributes {
    quint32 type = 0;
    quint32 mode = 0;
    quint32 nlink = 0;
    quint32 uid = 0;
    quint32 gid = 0;
    quint32 size = 0;
    quint32 blockSize = 0;
    quint32 rdev = 0;
    quint32 blocks = 0;
    quint32 fsid = 0;
    quint32 fileId = 0;
    quint32 atimeSec = 0, atimeUsec = 0;
    quint32 mtimeSec = 0, mtimeUsec = 0;
    quint32 ctimeSec = 0, ctimeUsec = 0;

    static constexpr quint32 kTypeRegular = 1;
    static constexpr quint32 kTypeDirectory = 2;

    bool isDirectory() const {
        return type == kTypeDirectory;
    }
};

/// An NFSv2 client for one player, over the RPC layer beneath it.
///
/// Deliberately partial: `MNT`, `LOOKUP`, `READ` and `GETATTR` are all a client
/// needs, and are all a player reliably implements. libcdj's attempt at
/// `READDIR` came back `PROC_UNAVAIL`, so directory enumeration is not
/// something to build on.
///
/// **The port discovery step is not optional.** nfsd sits on the standard 2049
/// and mountd on 48276 on every device we have seen, but those are discovered
/// through the portmapper rather than assumed — the numbers are consistent, not
/// documented, and a client that hardcodes them will break on the first device
/// that differs. (Our own *server* has the reverse problem: a player will not
/// fall back to those ports if our portmapper is silent, F46.)
///
/// Lives on the ProLink network thread. Everything is asynchronous.
class NfsV2Client : public QObject {
    Q_OBJECT

  public:
    template<typename T>
    struct Outcome {
        bool ok = false;
        QString error;
        T value{};
    };

    struct ReadResult {
        FileAttributes attributes;
        QByteArray data;
    };

    NfsV2Client(const QHostAddress& peer,
            const QHostAddress& localAddress,
            QObject* parent = nullptr);
    ~NfsV2Client() override;

    /// Resolve the mountd and nfsd ports, then mount *exportPath* (`/C/` for
    /// USB, `/B/` for SD). The callback receives the export's root filehandle.
    ///
    /// Both ports are looked up before mounting, so a failure is attributable:
    /// "no portmapper" and "portmapper says nfsd is not registered" are
    /// different problems with different causes.
    void mount(const QString& exportPath,
            std::function<void(const Outcome<QByteArray>&)> callback);

    /// Resolve one path component within *directoryHandle*.
    void lookup(const QByteArray& directoryHandle,
            const QString& name,
            std::function<void(const Outcome<QByteArray>&)> callback);

    /// Walk a whole slash-separated path from an already-mounted root.
    ///
    /// A convenience over chained lookups, and the shape almost every caller
    /// wants: paths out of `export.pdb` arrive as complete relative strings.
    void resolvePath(const QByteArray& rootHandle,
            const QString& path,
            std::function<void(const Outcome<QByteArray>&)> callback);

    void getAttributes(const QByteArray& handle,
            std::function<void(const Outcome<FileAttributes>&)> callback);

    /// One READ. *count* must not exceed kNfsMaxData.
    void read(const QByteArray& handle,
            quint32 offset,
            quint32 count,
            std::function<void(const Outcome<ReadResult>&)> callback);

    /// Release a mount. Best-effort: real players do call this after an eject
    /// (C9), so we do too, but nothing depends on the reply.
    void unmount(const QString& exportPath);

    void abortAll();

    quint16 mountdPort() const {
        return m_mountdPort;
    }
    quint16 nfsdPort() const {
        return m_nfsdPort;
    }

  private:
    /// GETPORT for one program, caching the answer.
    void resolvePort(quint32 program,
            quint32 version,
            std::function<void(bool, quint16)> callback);
    void resolvePathStep(const QByteArray& handle,
            const QStringList& components,
            int index,
            std::function<void(const Outcome<QByteArray>&)> callback);

    rpc::RpcClient m_rpc;
    quint16 m_mountdPort = 0;
    quint16 m_nfsdPort = 0;
};

} // namespace nfs
} // namespace prolink
} // namespace mixxx
