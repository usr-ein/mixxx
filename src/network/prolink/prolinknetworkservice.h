#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>

#include "network/prolink/prolinkdevice.h"
#include "network/prolink/prolinkmediaquery.h"
#include "network/prolink/server/prolinkservestatus.h"

class QTimer;

namespace mixxx {
namespace prolink {

/// The single object the rest of Mixxx talks to about Pro DJ Link.
///
/// **The protocol is not implemented here.** It lives in `lib/prolink`, a Rust
/// workspace linked statically into this binary, and this class is the Qt
/// shell around it: it turns the library's polled state and drained events
/// into the signals and slots the library feature already expects, and does
/// nothing else. Everything above it — `src/library/prolink/`, the phase-meter
/// widget, the skin — is unchanged by the swap.
///
/// # Why there is no longer a network thread
///
/// The C++ this replaces owned a `QThread` because it held the sockets, and a
/// blocking NFS read on the GUI thread would have delayed a keep-alive: five
/// missed keep-alives and Mixxx disappears from every deck on the network,
/// mid-set. The Rust library owns its own runtime and its own threads, so the
/// sockets are already off this thread and that reason is gone.
///
/// What is left here is a timer that drains an event queue and reads two
/// vectors. Draining on our own thread is deliberate: a callback from a
/// network thread would make every handler here have to be thread-safe and
/// re-entrant, and a slow one would stall the protocol.
class ProLinkNetworkService : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkNetworkService(QObject* parent = nullptr);
    ~ProLinkNetworkService() override;

    /// Start listening. Safe to call twice.
    void start();

    /// Stop, releasing the device number.
    void shutdown();

    QList<ProLinkDevice> devices() const {
        return m_devices;
    }
    int deviceCount() const {
        return m_devices.size();
    }

    bool isListening() const {
        return m_listening;
    }
    int announcedNumber() const {
        return m_announcedNumber;
    }
    QString announceDetail() const {
        return m_announceDetail;
    }
    QString lastError() const {
        return m_lastError;
    }

    server::ServeStatus serveStatus() const {
        return m_serveStatus;
    }

  public slots:
    /// Drop any held browse connection so the next request reconnects.
    void refresh();

    /// Fetch `export.pdb` from the first player with media in `slot`.
    void pullDatabase(MediaSlot slot = MediaSlot::Usb);

    /// Fetch a player's `export.pdb`.
    ///
    /// Keyed on MAC rather than on a device number, and that is not a
    /// convenience: a number can be reassigned, so a request that outlived the
    /// device it named would otherwise reach a different deck.
    void fetchDatabase(const QByteArray& mac, MediaSlot slot);

    /// Fetch one file from a medium into `localPath`.
    ///
    /// `remotePath` is taken verbatim from `export.pdb`, which stores paths
    /// relative to the medium root with a leading slash.
    ///
    /// `priority` is accepted and ignored. The library serialises transfers
    /// itself, because two pulls from one deck contend for its filehandle
    /// table and it then answers `NFSERR_STALE` to everything.
    void fetchFile(const QByteArray& mac,
            MediaSlot slot,
            const QString& remotePath,
            const QString& localPath,
            bool priority = false);

    /// Fetch a track's artwork into `localPath`.
    void fetchArtwork(const QByteArray& mac,
            MediaSlot slot,
            quint32 artworkId,
            const QString& localPath);

  signals:
    void deviceFound(const mixxx::prolink::ProLinkDevice& device);
    void deviceChanged(const mixxx::prolink::ProLinkDevice& device);
    void deviceLost(const QByteArray& mac);
    void listeningChanged(bool listening, const QString& error);
    void announceChanged(int deviceNumber, const QString& detail);
    void serveStatusChanged(const mixxx::prolink::server::ServeStatus& status);
    void mediaInfoFound(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const mixxx::prolink::MediaInfo& info);
    /// The result of fetchDatabase(). `error` is empty on success.
    ///
    /// Carries the bytes rather than the path they were written to: the
    /// caller parses them and never wants the file, and reading it back would
    /// be a second failure to report for no gain.
    void databaseFetched(const QByteArray& mac,
            mixxx::prolink::MediaSlot slot,
            const QByteArray& data,
            const QString& error);
    void fileFetched(const QString& localPath, const QString& error);
    void artworkFetched(const QString& localPath, const QString& error);
    void fileFetchProgress(const QString& localPath, quint32 done, quint32 total);

  private:
    /// Drain the library's events and re-read its tables. On a timer.
    void poll();

    /// Re-read the device table, emitting found, changed and lost as it moves.
    void syncDevices();

    /// Re-read what each player has in its slots, emitting `mediaInfoFound`.
    void syncMedia(int deviceNumber, MediaSlot slot);

    /// What a transfer was asked for, so its end can be reported as the signal
    /// the caller is waiting on.
    ///
    /// The library reports a transfer id and the path it wrote. Which *kind*
    /// of fetch it was is this class's business, because only the slots above
    /// know which signal was promised.
    struct Pending {
        bool isDatabase = false;
        QByteArray mac;
        MediaSlot slot = MediaSlot::Usb;
        QString localPath;
    };

    /// The device number a MAC currently holds, or 0.
    int numberFor(const QByteArray& mac) const;

    /// Opaque, so the generated `cxx` header does not reach every translation
    /// unit that includes this one.
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;

    QTimer* m_pTimer = nullptr;
    QList<ProLinkDevice> m_devices;
    QHash<quint32, Pending> m_pending;
    bool m_listening = false;
    int m_announcedNumber = 0;
    QString m_announceDetail;
    QString m_lastError;
    server::ServeStatus m_serveStatus;
};

} // namespace prolink
} // namespace mixxx
