#include "library/deck/volumelabel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace {
/// Where pi_config/dj-usb leaves the label it already read at mount time.
const QString kSidecarDir = QStringLiteral("/run/dj-usb");
const QString kByLabelDir = QStringLiteral("/dev/disk/by-label");

/// What to call a stick that has no label. The slot is all we know, and
/// "DJ_USB_1" is a mount point rather than a name a DJ would recognise.
QString fallbackName(const QString& slot) {
    if (slot.startsWith(QStringLiteral("DJ_USB_"))) {
        return QStringLiteral("USB ") + slot.mid(7);
    }
    return slot;
}
} // namespace

namespace mixxx {
namespace deck {

QString deviceForMountPoint(const QString& mountPoint) {
    // /proc/self/mountinfo rather than /proc/mounts: the former survives a
    // mount point containing spaces (they arrive octal-escaped) and gives the
    // fields positionally rather than by guessing at the separator.
    //
    // Fields, per Documentation/filesystems/proc.rst:
    //   0 id  1 parent  2 major:minor  3 root  4 MOUNT POINT  5 options
    //   ... optional fields ... "-"  fstype  SOURCE  super-options
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QStringList fields = stream.readLine().split(QChar(' '));
        if (fields.size() < 10) {
            continue;
        }
        QString target = fields.at(4);
        // The kernel escapes space, tab, newline and backslash as octal.
        target.replace(QStringLiteral("\\040"), QStringLiteral(" "));
        if (target != mountPoint) {
            continue;
        }
        const int separator = fields.indexOf(QStringLiteral("-"));
        if (separator < 0 || separator + 2 >= fields.size()) {
            continue;
        }
        return fields.at(separator + 2);
    }
    return QString();
}

QString volumeLabelFor(const QString& mountPoint) {
    const QString slot = QFileInfo(mountPoint).fileName();

    // 1. The sidecar, if dj-usb wrote one.
    QFile sidecar(QDir(kSidecarDir).filePath(slot + QStringLiteral(".label")));
    if (sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString label = QString::fromUtf8(sidecar.readAll()).trimmed();
        if (!label.isEmpty()) {
            return label;
        }
        // An empty sidecar is a positive answer, not a missing one: dj-usb ran
        // and blkid found no label. Fall through to the slot name rather than
        // to by-label, which cannot know better.
        return fallbackName(slot);
    }

    // 2. Resolve the device and look for it among the by-label symlinks.
    const QString device = deviceForMountPoint(mountPoint);
    if (!device.isEmpty()) {
        const QFileInfo deviceInfo(device);
        const QDir byLabel(kByLabelDir);
        const QFileInfoList links = byLabel.entryInfoList(
                QDir::NoDotAndDotDot | QDir::System | QDir::Files | QDir::Dirs);
        for (const QFileInfo& link : links) {
            // canonicalFilePath resolves ../../sda1 to /dev/sda1.
            if (link.canonicalFilePath() == deviceInfo.canonicalFilePath()) {
                return link.fileName();
            }
        }
    }

    return fallbackName(slot);
}

} // namespace deck
} // namespace mixxx
