#include "library/deck/screenpower.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include "util/logger.h"

namespace mixxx {
namespace deck {
namespace screenpower {

namespace {
const Logger kLogger("ScreenPower");

/// What off() did, to be undone: "brightness N" or "dpms".
QString markerPath() {
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return QDir(runtime.isEmpty() ? QDir::tempPath() : runtime)
            .filePath(QStringLiteral("trimixxx-screen-off"));
}

QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromLatin1(file.readAll()).trimmed();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(data) == data.size();
}

void xset(const QStringList& args) {
    QProcess::execute(QStringLiteral("xset"), args);
}

/// Back on: a brightness to put back, or DPMS to wake and disarm. The X
/// session starts with DPMS off (pi_config/xinitrc: no blanking, ever), and
/// `xset dpms force` arms it.
void undo(const QString& marker) {
    const QString backlight = backlightIn();
    if (marker.startsWith(QStringLiteral("brightness ")) && !backlight.isEmpty()) {
        const QString value = marker.mid(11);
        if (!writeFile(QDir(backlight).filePath(QStringLiteral("brightness")), value.toLatin1())) {
            kLogger.warning() << "could not put the backlight back to" << value;
        }
    } else if (marker == QStringLiteral("dpms")) {
        xset({QStringLiteral("dpms"), QStringLiteral("force"), QStringLiteral("on")});
        xset({QStringLiteral("-dpms")});
    }
}
} // namespace

QString backlightIn(const QString& root) {
    const QDir dir(root);
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& entry : entries) {
        if (readFile(QDir(dir.filePath(entry)).filePath(QStringLiteral("max_brightness"))).toInt() > 0) {
            return dir.filePath(entry);
        }
    }
    return QString();
}

void off() {
    if (QFile::exists(markerPath())) {
        return; // off already
    }
    const QString backlight = backlightIn();
    if (!backlight.isEmpty()) {
        const QString brightness = readFile(QDir(backlight).filePath(QStringLiteral("brightness")));
        // Noted before the light goes: a crash in between leaves it lit.
        if (brightness.toInt() <= 0 ||
                !writeFile(markerPath(), QByteArray("brightness ") + brightness.toLatin1()) ||
                !writeFile(QDir(backlight).filePath(QStringLiteral("brightness")), "0")) {
            kLogger.warning() << "could not turn the backlight off through" << backlight;
        }
        return;
    }
    if (writeFile(markerPath(), "dpms")) {
        xset({QStringLiteral("dpms"), QStringLiteral("force"), QStringLiteral("off")});
    }
}

void on() {
    const QString marker = readFile(markerPath());
    if (marker.isEmpty()) {
        return;
    }
    undo(marker);
    QFile::remove(markerPath());
}

void recover() {
    const QString marker = readFile(markerPath());
    if (marker.isEmpty()) {
        return;
    }
    kLogger.warning() << "the screen was left off (a crash?): lit again";
    undo(marker);
    QFile::remove(markerPath());
}

} // namespace screenpower
} // namespace deck
} // namespace mixxx
