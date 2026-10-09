#include "widget/deck/deckrelease.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>

namespace {

QByteArray contents(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

QString text(const QString& path) {
    return QString::fromUtf8(contents(path));
}

/// Lines with any \r of a file edited on a Mac taken off.
QStringList linesOf(const QString& text) {
    QStringList lines = text.split(QChar('\n'));
    for (QString& line : lines) {
        line.remove(QChar('\r'));
    }
    return lines;
}

/// `[group]` then `key=value` lines, as GLib's key files are, which is what
/// RAUC's system.conf and central status file both are.
QHash<QString, QHash<QString, QString>> groupsOf(const QString& text) {
    QHash<QString, QHash<QString, QString>> groups;
    QString group;
    for (const QString& rawLine : linesOf(text)) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QChar('#'))) {
            continue;
        }
        if (line.startsWith(QChar('[')) && line.endsWith(QChar(']'))) {
            group = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        const int equals = line.indexOf(QChar('='));
        if (equals > 0 && !group.isEmpty()) {
            groups[group].insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
        }
    }
    return groups;
}

QChar slotLetter(const QString& text) {
    return text == QLatin1String("A") || text == QLatin1String("B") ? text.at(0) : QChar();
}

} // namespace

namespace mixxx {
namespace deck {

DeckRelease::Release DeckRelease::parseRelease(const QString& text) {
    Release release;
    for (const QString& line : linesOf(text)) {
        const int equals = line.indexOf(QChar('='));
        if (equals <= 0) {
            continue;
        }
        const QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();
        if (key == QLatin1String("VERSION")) {
            release.version = value;
        } else if (key == QLatin1String("COMMIT")) {
            release.commit = value;
        } else if (key == QLatin1String("HASH")) {
            release.hash = value;
        } else if (key == QLatin1String("BUILT")) {
            release.built = value;
        }
    }
    return release;
}

QChar DeckRelease::slotOnCommandLine(const QString& cmdline) {
    static const QRegularExpression kSlot(QStringLiteral("(?:^|\\s)rauc\\.slot=([AB])(?=\\s|$)"));
    const QRegularExpressionMatch match = kSlot.match(cmdline);
    return match.hasMatch() ? match.captured(1).at(0) : QChar();
}

bool DeckRelease::isTrial(const QByteArray& tryboot) {
    if (tryboot.size() != 4) {
        return false;
    }
    return qFromBigEndian<quint32>(tryboot.constData()) == 1;
}

QChar DeckRelease::committedIn(const QString& autobootTxt) {
    // rpi-tryboot's named_in, which reads it as the firmware does: lines before
    // any section belong to [all], and the last boot_partition there wins.
    bool inAll = true;
    QString partition;
    for (const QString& rawLine : linesOf(autobootTxt)) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QChar('['))) {
            inAll = line.startsWith(QLatin1String("[all]"));
            continue;
        }
        const int equals = line.indexOf(QChar('='));
        if (!inAll || equals <= 0) {
            continue;
        }
        if (line.left(equals).trimmed() == QLatin1String("boot_partition")) {
            partition = line.mid(equals + 1).trimmed();
        }
    }
    if (partition == QLatin1String("2")) {
        return QChar('A');
    }
    if (partition == QLatin1String("3")) {
        return QChar('B');
    }
    return QChar();
}

QHash<QChar, DeckRelease::Install> DeckRelease::parseInstalls(
        const QString& systemConf, const QString& centralStatus) {
    const QHash<QString, QHash<QString, QString>> config = groupsOf(systemConf);
    const QHash<QString, QHash<QString, QString>> status = groupsOf(centralStatus);
    const QString kSlotPrefix = QStringLiteral("slot.");

    QHash<QChar, Install> installs;
    for (auto slot = config.cbegin(); slot != config.cend(); ++slot) {
        const QChar bootname = slotLetter(slot.value().value(QStringLiteral("bootname")));
        if (!slot.key().startsWith(kSlotPrefix) || bootname.isNull()) {
            continue;
        }
        // One install writes the slot and those under it -- its boot
        // partition, [slot.boot.0] parent=rootfs.0 -- and it is whole only if
        // each of them is.
        const QString name = slot.key().mid(kSlotPrefix.size());
        QStringList members{slot.key()};
        for (auto child = config.cbegin(); child != config.cend(); ++child) {
            if (child.value().value(QStringLiteral("parent")) == name) {
                members.append(child.key());
            }
        }
        std::optional<Install> install;
        for (const QString& member : std::as_const(members)) {
            // A record without a status is not an install: `rauc status
            // mark-active` writes when it activated a slot, a slot that came
            // on the card included, and nothing else.
            const QHash<QString, QString> record = status.value(member);
            const QString memberStatus = record.value(QStringLiteral("status"));
            if (memberStatus.isEmpty()) {
                continue;
            }
            if (!install) {
                install = Install{record.value(QStringLiteral("bundle.version")),
                        record.value(QStringLiteral("bundle.build")),
                        record.value(QStringLiteral("installed.timestamp")),
                        memberStatus};
            } else if (install->ok()) {
                install->status = memberStatus;
            }
        }
        if (install) {
            installs.insert(bootname, *install);
        }
    }
    return installs;
}

DeckRelease::Verdict DeckRelease::verdictOf(const QString& healthLog, QChar slot) {
    // pi_config/trimixxx-health's own lines: "<time> +<uptime>s <message>".
    // These are its words; the tests copy them from the script.
    static const QRegularExpression kLine(QStringLiteral("^\\S+ \\+\\d+s (.*)$"));
    static const QRegularExpression kTrial(QStringLiteral("^trial of slot ([AB]) \\((.*)\\)$"));
    static const QRegularExpression kKept(
            QStringLiteral("^slot ([AB]) can take the next release: committed(?:, ([AB]) marked bad)?$"));
    static const QRegularExpression kNotCommitted(QStringLiteral("^slot ([AB]): RAUC can't commit it"));
    static const QRegularExpression kMarkedBad(
            QStringLiteral("^slot ([AB]) can't take the next release after \\d+ s \\((.*)\\): marked bad"));

    Verdict verdict;
    const QString letter(slot);
    for (const QString& line : linesOf(healthLog)) {
        const QRegularExpressionMatch entry = kLine.match(line);
        if (!entry.hasMatch()) {
            continue;
        }
        const QString message = entry.captured(1);
        QRegularExpressionMatch match;
        if ((match = kTrial.match(message)).hasMatch()) {
            if (match.captured(1) == letter) {
                verdict = Verdict{Verdict::Kind::OnTrial, match.captured(2), QString()};
            }
        } else if ((match = kKept.match(message)).hasMatch()) {
            if (match.captured(1) == letter) {
                verdict.kind = Verdict::Kind::Kept;
                verdict.why.clear();
            } else if (match.captured(2) == letter) {
                // The other slot, after a start the bootloader's watchdog
                // made on this one because the committed slot failed to.
                verdict.kind = Verdict::Kind::Failed;
                verdict.why = QStringLiteral("it failed to start, and the bootloader fell back");
            }
        } else if ((match = kNotCommitted.match(message)).hasMatch()) {
            if (match.captured(1) == letter) {
                verdict.kind = Verdict::Kind::Failed;
                verdict.why = QStringLiteral("RAUC couldn't commit it");
            }
        } else if ((match = kMarkedBad.match(message)).hasMatch()) {
            if (match.captured(1) == letter) {
                verdict.kind = Verdict::Kind::Failed;
                verdict.why = match.captured(2);
            }
        }
    }
    return verdict;
}

DeckRelease::Blocked DeckRelease::blocked() const {
    if (!isReleaseCard()) {
        return Blocked::DevCard;
    }
    if (committed.isNull()) {
        return Blocked::CommittedUnknown;
    }
    if (committed != booted) {
        return trial ? Blocked::TrialPending : Blocked::FellBack;
    }
    if (!otherInstall) {
        // A card comes with its release in A and nothing in B
        // (pi-qemu/release/genimage.cfg), and only RAUC writes either after.
        if (other == QChar('B')) {
            return Blocked::OtherEmpty;
        }
    } else if (!otherInstall->ok()) {
        return Blocked::OtherUnfinished;
    }
    if (otherFailed()) {
        return Blocked::OtherFailed;
    }
    return Blocked::No;
}

bool DeckRelease::otherFailed() const {
    if (otherVerdict.kind != Verdict::Kind::Failed) {
        return false;
    }
    // Unless a release went in since: RAUC's record names another one than
    // the trial that failed. A fall-back's verdict names none, and stands
    // until the next trial of the slot, which an install starts at once.
    return !(otherInstall && !otherVerdict.version.isEmpty() &&
            otherInstall->version != otherVerdict.version);
}

QString DeckRelease::otherVersion() const {
    if (otherInstall) {
        return otherInstall->version;
    }
    // A without a record still holds the release its card came with: any
    // install would have left one. Its last trial is the only thing that named
    // it.
    return other == QChar('A') ? otherVerdict.version : QString();
}

DeckRelease DeckRelease::read(const QString& root) {
    DeckRelease release;
    release.release = parseRelease(text(root + QStringLiteral("/etc/trimixxx-release")));
    release.booted = slotOnCommandLine(text(root + QStringLiteral("/proc/cmdline")));
    if (!release.isReleaseCard()) {
        return release;
    }
    release.other = release.booted == QChar('A') ? QChar('B') : QChar('A');
    release.trial = isTrial(contents(root + QStringLiteral("/proc/device-tree/chosen/bootloader/tryboot")));
    release.committed = committedIn(text(root + QStringLiteral("/var/lib/rauc/autoboot.txt")));
    const QHash<QChar, Install> installs = parseInstalls(
            text(root + QStringLiteral("/etc/rauc/system.conf")),
            text(root + QStringLiteral("/var/lib/rauc/central.raucs")));
    if (installs.contains(release.other)) {
        release.otherInstall = installs.value(release.other);
    }
    // The previous file first: the health check moves the log there at 64 KiB.
    release.otherVerdict = verdictOf(
            text(root + QStringLiteral("/var/lib/rauc/trimixxx-health.log.1")) +
                    text(root + QStringLiteral("/var/lib/rauc/trimixxx-health.log")),
            release.other);
    return release;
}

} // namespace deck
} // namespace mixxx
