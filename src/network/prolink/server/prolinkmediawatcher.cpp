#include "network/prolink/server/prolinkmediawatcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include "moc_prolinkmediawatcher.cpp"
#include "network/prolink/server/prolinkservedmedium.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkMediaWatcher");

/// Same period pi-midi-daemon polls its own USB mounts on, and for the same
/// reason: fast enough that a stick appears before the DJ has finished reaching
/// for the deck, cheap enough to run forever.
constexpr int kPollIntervalMs = 2000;

/// Slots we can advertise, in the order a medium claims them. USB first: it is
/// the slot a DJ expects a stick to appear in.
constexpr mixxx::prolink::MediaSlot kSlotOrder[] = {
        mixxx::prolink::MediaSlot::Usb,
        mixxx::prolink::MediaSlot::Sd,
};

/// Decode the octal escapes `/proc/self/mounts` uses for spaces and the like.
QString unescapeMountPath(const QString& path) {
    QString out;
    for (int i = 0; i < path.size(); ++i) {
        if (path.at(i) == QLatin1Char('\\') && i + 3 < path.size()) {
            bool ok = false;
            const int value = path.mid(i + 1, 3).toInt(&ok, 8);
            if (ok) {
                out.append(QChar(value));
                i += 3;
                continue;
            }
        }
        out.append(path.at(i));
    }
    return out;
}

} // namespace

namespace mixxx {
namespace prolink {
namespace server {

ProLinkMediaWatcher::ProLinkMediaWatcher(QObject* parent)
        : QObject(parent) {
}

ProLinkMediaWatcher::~ProLinkMediaWatcher() = default;

void ProLinkMediaWatcher::start() {
    if (!m_pTimer) {
        m_pTimer = new QTimer(this);
        connect(m_pTimer, &QTimer::timeout, this, &ProLinkMediaWatcher::poll);
    }
    if (!m_pTimer->isActive()) {
        m_pTimer->start(kPollIntervalMs);
    }
    poll();
}

void ProLinkMediaWatcher::stop() {
    if (m_pTimer) {
        m_pTimer->stop();
    }
}

QStringList ProLinkMediaWatcher::candidateVolumes() {
    // The same roots the Rekordbox feature scans, and their **immediate**
    // children only -- which is why the Pi's automounter puts sticks at
    // /media/DJ_USB_1 rather than one level deeper.
    QStringList roots{QStringLiteral("/media"), QStringLiteral("/Volumes")};
    const QString user = QString::fromLocal8Bit(qgetenv("USER"));
    if (!user.isEmpty()) {
        roots.append(QStringLiteral("/media/") + user);
        roots.append(QStringLiteral("/run/media/") + user);
    }

    QStringList volumes;
    for (const QString& root : roots) {
        const QDir dir(root);
        if (!dir.exists()) {
            continue;
        }
        for (const QFileInfo& child :
                dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString path = child.absoluteFilePath();
            if (!volumes.contains(path)) {
                volumes.append(path);
            }
        }
    }
    return volumes;
}

QString ProLinkMediaWatcher::volumeLabel(const QString& mountPoint) {
    // On macOS the mount point *is* the label, so the fallback is already right
    // there. On the Pi it is `DJ_USB_1`, which tells a DJ nothing and is not
    // what their deck shows for the same stick -- so find the real label by
    // going mount point -> device -> /dev/disk/by-label.
    const QString fallback = QFileInfo(mountPoint).fileName();

    QFile mounts(QStringLiteral("/proc/self/mounts"));
    if (!mounts.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return fallback;
    }
    QString device;
    // Loop on readLine() rather than on atEnd(). Files under /proc report a
    // size of zero, and QFile::atEnd() is `pos() >= size()` for anything it
    // does not consider sequential — so it is true before the first read and
    // the loop body never runs. The symptom is not an error: the label simply
    // comes back as the mount point's name, which looks like a stick that has
    // no label rather than like a bug.
    forever {
        const QByteArray line = mounts.readLine();
        if (line.isEmpty()) {
            break;
        }
        const QStringList fields = QString::fromUtf8(line).split(QLatin1Char(' '));
        if (fields.size() >= 2 && unescapeMountPath(fields.at(1)) == mountPoint) {
            device = fields.at(0);
            break;
        }
    }
    if (device.isEmpty()) {
        return fallback;
    }

    // `QDir::Files` does not list these. Every entry in by-label is a symlink to
    // a block device, and to Qt that is neither a file nor a directory — a
    // filter of Files|System comes back empty and the label silently falls back
    // to the mount point's name, which on the Pi is `DJ_USB_1` and tells the DJ
    // nothing. AllEntries|System lists them.
    const QDir byLabel(QStringLiteral("/dev/disk/by-label"));
    for (const QString& name :
            byLabel.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        const QFileInfo link(byLabel.absoluteFilePath(name));
        // symLinkTarget() resolves the `../../sda1` these use into an absolute
        // path, which is the form /proc/self/mounts gives us.
        if (link.symLinkTarget() == device) {
            // udev escapes the label the same way the mount table does.
            return unescapeMountPath(name);
        }
    }
    return fallback;
}

void ProLinkMediaWatcher::poll() {
    QStringList found;
    for (const QString& volume : candidateVolumes()) {
        if (QFile::exists(volume + QLatin1Char('/') + QLatin1String(kPdbPath))) {
            found.append(volume);
        }
    }

    // Gone first, so a stick swapped between polls frees its slot before the
    // replacement asks for one.
    const QList<int> occupied = m_bySlot.keys();
    for (const int slot : occupied) {
        if (!found.contains(m_bySlot.value(slot))) {
            const QString path = m_bySlot.value(slot);
            m_bySlot.remove(slot);
            kLogger.info() << "medium gone from slot" << slot << ":" << path;
            emit mediumUnmounted(static_cast<MediaSlot>(slot));
        }
    }

    for (const QString& volume : found) {
        if (m_bySlot.values().contains(volume)) {
            continue; // already serving it, in whichever slot it took
        }
        MediaSlot slot = MediaSlot::Empty;
        for (const MediaSlot candidate : kSlotOrder) {
            if (!m_bySlot.contains(static_cast<int>(candidate))) {
                slot = candidate;
                break;
            }
        }
        if (slot == MediaSlot::Empty) {
            // A third stick, with nowhere to put it. Worth saying once per poll
            // rather than silently ignoring: the DJ plugged something in and
            // nothing happened.
            kLogger.info() << "no free slot for" << volume
                           << "- a CDJ has only USB and SD";
            continue;
        }
        m_bySlot.insert(static_cast<int>(slot), volume);
        const QString label = volumeLabel(volume);
        kLogger.info() << "medium mounted in slot" << static_cast<int>(slot) << ":"
                       << volume << "(" << label << ")";
        emit mediumMounted(slot, volume, label);
    }
}

} // namespace server
} // namespace prolink
} // namespace mixxx
