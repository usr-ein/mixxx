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

class ControlObject;
class ControlPushButton;
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

    /// Fetch one file head-first, so it can be played before it has arrived.
    ///
    /// The file appears at `localPath` at its full size immediately, sparse,
    /// and fills in: the head first, then the tail, then the middle. Progress
    /// arrives as `fileFetchProgress`, whose `offset` and `length` say which
    /// range has actually landed — and **the rest of the file is a hole that
    /// reads back as zeros**, so nothing may read a range it has not been told
    /// about. `StreamingFile` is what enforces that.
    ///
    /// The first progress signal carries the size and no bytes, which is the
    /// moment a caller can size its view of the file and start decoding.
    void fetchFileStreaming(const QByteArray& mac,
            MediaSlot slot,
            const QString& remotePath,
            const QString& localPath,
            quint32 headBytes);

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
    /// How a transfer is getting on.
    ///
    /// `done`/`total` are a fraction to show a user. `offset`/`length` are the
    /// range that has just landed on disk, and for a streaming fetch they are
    /// the only safe statement about which bytes exist — `done` counts the head
    /// and the tail together, which are not contiguous. `length` is zero for an
    /// event that carries no new bytes: the opening one that announces `total`,
    /// and the closing summary.
    void fileFetchProgress(const QString& localPath,
            quint64 done,
            quint64 total,
            quint64 offset,
            quint64 length);

  private:
    /// Drain the library's events and re-read its tables. On a timer.
    void poll();

    /// Publish the tempo master's number, tempo and bar phase as controls.
    ///
    /// `[ProLink] master_device`, `master_bpm` and `master_bar_phase`, which is
    /// what the phase-meter widget reads. Read-only, because nothing in Mixxx
    /// may tell a CDJ what phase it is at.
    void publishMaster();

    /// Re-read the device table, emitting found, changed and lost as it moves.
    void syncDevices();

    /// Emit `announceChanged` when the negotiated player number moves.
    ///
    /// The number is not known when `start()` returns: taking one means
    /// watching the network for a couple of seconds and then sending a claim
    /// chain, and that is far too long to hold the GUI thread. So it is polled
    /// for instead, and announced when it settles.
    void syncAnnouncement();

    /// Re-read what we are offering, emitting `serveStatusChanged` on a change.
    void syncServeStatus();

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
        bool isArtwork = false;
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

    /// The one instance holding the sockets, or null. See start().
    static ProLinkNetworkService* s_pListening;

    QTimer* m_pTimer = nullptr;
    /// `[ProLink] pull_db`, so a controller or a skin button can ask for the
    /// database without going through the menu.
    std::unique_ptr<ControlPushButton> m_pPullDbControl;
    std::unique_ptr<ControlObject> m_pMasterDeviceControl;
    std::unique_ptr<ControlObject> m_pMasterBpmControl;
    std::unique_ptr<ControlObject> m_pMasterBarPhaseControl;
    QList<ProLinkDevice> m_devices;
    QHash<quint32, Pending> m_pending;
    bool m_listening = false;
    int m_announcedNumber = 0;
    /// The number `announceChanged` last carried, so it is emitted on a change
    /// rather than thirty times a second.
    int m_publishedNumber = 0;
    /// The number to ask for next time, kept across a shutdown.
    ///
    /// A refresh restarts the session, and restarting with no preference would
    /// take whatever happens to be free -- so Mixxx could come back as a
    /// different player than the decks have in their tables and than the user
    /// just read off the screen. Held here rather than in `m_announcedNumber`
    /// because that one is cleared by `shutdown()`, which is the whole point.
    int m_preferredNumber = 0;
    QString m_announceDetail;
    QString m_lastError;
    server::ServeStatus m_serveStatus;
};

} // namespace prolink
} // namespace mixxx
