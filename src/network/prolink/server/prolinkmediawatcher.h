#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include "network/prolink/prolinkdefs.h"

class QTimer;

namespace mixxx {
namespace prolink {
namespace server {

/// Watches for rekordbox media appearing and disappearing, and assigns slots.
///
/// TriMiXxX has two USB ports and presents them to the network as a USB and an
/// SD, because that is the pair a CDJ expects to see. The first medium takes the
/// USB slot (`/C/`), the second the SD slot (`/B/`).
///
/// **Assignment is sticky.** A medium keeps its slot for as long as it stays
/// mounted, and a newly arrived one takes the lowest slot that is free. The
/// alternative — renumbering from scratch on every change — would move a medium
/// from SD to USB the moment the other stick was pulled, and a deck browsing it
/// would find its filehandles stale and its slot suddenly empty. Mid-set.
///
/// **Polling, not inotify or udev.** The Pi's automounter (`pi_config/dj-usb`)
/// does `mkdir` and only *then* `mount`, so an inotify watch fires on an empty
/// directory before the filesystem is there, and inotify cannot see the mount
/// itself. Testing for `export.pdb` on a timer sees the state that actually
/// matters, and it works identically on a Mac for development. The same
/// reasoning, and the same 2 s period, as `pi-midi-daemon`'s own USB watch.
class ProLinkMediaWatcher : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkMediaWatcher(QObject* parent = nullptr);
    ~ProLinkMediaWatcher() override;

    void start();
    void stop();

    /// Re-scan now rather than waiting for the next tick.
    void poll();

    /// Currently mounted media, by slot.
    QMap<int, QString> media() const {
        return m_bySlot;
    }

  signals:
    /// A rekordbox medium is now available in *slot*. *volumeName* is the
    /// stick's own label, not the mount point's name.
    void mediumMounted(mixxx::prolink::MediaSlot slot,
            const QString& path,
            const QString& volumeName);
    /// The medium in *slot* is gone. The CDJs must be told: a deck holding
    /// filehandles into it will otherwise keep asking for files that are no
    /// longer there.
    void mediumUnmounted(mixxx::prolink::MediaSlot slot);

  private:
    /// Every directory that could hold a mounted rekordbox medium.
    static QStringList candidateVolumes();
    /// The volume's own label, or the mount point's name if we cannot find one.
    static QString volumeLabel(const QString& mountPoint);

    QTimer* m_pTimer = nullptr;
    /// slot -> mount path.
    QMap<int, QString> m_bySlot;
};

} // namespace server
} // namespace prolink
} // namespace mixxx
