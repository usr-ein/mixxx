#include "widget/deck/decklevels.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "moc_decklevels.cpp"
#include "util/fpclassify.h"
#include "util/logger.h"
#include "util/math.h"

namespace {
const mixxx::Logger kLogger("DeckLevels");

/// Beside mixxx.cfg, and not in it: see the class comment.
const QString kFileName = QStringLiteral("trimixxx-levels");
const QString kOutputKey = QStringLiteral("output_db");
const QString kBrightnessKey = QStringLiteral("brightness");

/// Long enough that a turn of the encoder is written once, at its end; short
/// enough that a plug pulled a moment later still finds it on the card.
constexpr int kSaveDelayMs = 1000;

/// The first backlight under *root* that says how bright it goes. A deck has
/// one, so first is all the choosing there is to do.
QString findBacklight(const QString& root, int* pMaxBrightness) {
    const QDir dir(root);
    // Dirs takes the entries in /sys/class/backlight even though they are
    // symlinks: QFileInfo follows a link to a directory.
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& entry : entries) {
        QFile maxFile(QDir(dir.filePath(entry)).filePath(QStringLiteral("max_brightness")));
        if (!maxFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        bool ok = false;
        const int max = QString::fromLatin1(maxFile.readAll()).trimmed().toInt(&ok);
        if (ok && max > 0) {
            *pMaxBrightness = max;
            return dir.filePath(entry);
        }
    }
    return QString();
}
} // namespace

namespace mixxx {
namespace deck {

DeckLevels::DeckLevels(const QString& settingsPath,
        const QString& backlightRoot,
        QObject* pParent)
        : QObject(pParent),
          m_path(QDir(settingsPath).filePath(kFileName)),
          m_pGain(std::make_unique<ControlProxy>(QStringLiteral("[Master]"),
                  QStringLiteral("gain"),
                  this,
                  ControlFlag::NoAssertIfMissing)) {
    // In the body, not the initialiser list: m_maxBrightness is declared after
    // m_backlight, so its own initialiser would run after this and zero it.
    m_backlight = findBacklight(backlightRoot, &m_maxBrightness);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &DeckLevels::save);

    applySaved();
}

DeckLevels::~DeckLevels() {
    // A change still waiting on the timer is written now rather than lost with
    // it.
    if (m_saveTimer.isActive()) {
        save();
    }
}

bool DeckLevels::hasOutput() const {
    return m_pGain->valid();
}

double DeckLevels::outputDb() const {
    const double ratio = m_pGain->get();
    return ratio > db2ratio(kSilenceDb) ? ratio2db(ratio) : kSilenceDb;
}

void DeckLevels::stepOutput(int steps) {
    if (steps == 0 || !hasOutput()) {
        return;
    }
    const double current = outputDb();
    const double target = snapOutputDb(current + steps * kOutputStepDb);
    // A gain left outside the range by something else comes back into it, but
    // never by moving the wrong way: down from +10 dB lands on the top of the
    // range, and up from there stays put.
    if (steps > 0 ? target < current : target > current) {
        return;
    }
    if (std::abs(target - current) < 1e-9) {
        return;
    }
    m_pGain->set(db2ratio(target));
    m_savedOutputDb = target;
    scheduleSave();
}

int DeckLevels::brightness() const {
    const int raw = readRawBrightness();
    if (raw < 0) {
        return -1;
    }
    return static_cast<int>(std::lround(raw * 100.0 / m_maxBrightness));
}

void DeckLevels::stepBrightness(int steps) {
    if (steps == 0) {
        return;
    }
    const int current = brightness();
    if (current < 0) {
        return;
    }
    const int target = snapBrightness(current + steps * kBrightnessStep);
    // Same rule as the output: back into range, never the wrong way. Below the
    // floor already -- dimmed by something else -- a step down does nothing
    // rather than brightening the panel.
    if (steps > 0 ? target <= current : target >= current) {
        return;
    }
    if (writeBrightness(target)) {
        m_savedBrightness = target;
        scheduleSave();
    }
}

double DeckLevels::snapOutputDb(double db) {
    // util_isnan, not std::isnan: under -ffast-math the compiler takes std::isnan
    // to be always false, and a NaN then clamps to the top of the range
    // (fpclassify.h).
    if (util_isnan(db)) {
        return 0.0;
    }
    return std::clamp(std::round(db / kOutputStepDb) * kOutputStepDb, kOutputMinDb, kOutputMaxDb);
}

int DeckLevels::snapBrightness(int percent) {
    const int snapped = static_cast<int>(std::lround(
                                percent / static_cast<double>(kBrightnessStep))) *
            kBrightnessStep;
    return std::clamp(snapped, kBrightnessMin, kBrightnessMax);
}

void DeckLevels::applySaved() {
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Never set from here, so there is nothing to put back.
        return;
    }
    const QStringList lines =
            QString::fromUtf8(file.readAll()).split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const int equals = line.indexOf(QChar('='));
        if (equals <= 0) {
            continue;
        }
        const QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();
        bool ok = false;
        if (key == kOutputKey) {
            // QString reads "nan" and "inf" as numbers; neither is a level.
            const double db = value.toDouble(&ok);
            if (ok && util_isfinite(db)) {
                m_savedOutputDb = snapOutputDb(db);
            }
        } else if (key == kBrightnessKey) {
            const int percent = value.toInt(&ok);
            if (ok) {
                m_savedBrightness = snapBrightness(percent);
            }
        }
    }

    if (m_savedOutputDb && hasOutput()) {
        m_pGain->set(db2ratio(*m_savedOutputDb));
        kLogger.info() << "output trim" << *m_savedOutputDb << "dB, from" << m_path;
    }
    if (m_savedBrightness && hasBacklight()) {
        writeBrightness(*m_savedBrightness);
        kLogger.info() << "brightness" << *m_savedBrightness << "%, from" << m_path;
    }
}

void DeckLevels::scheduleSave() {
    m_saveTimer.start();
}

void DeckLevels::save() {
    m_saveTimer.stop();
    QStringList lines;
    if (m_savedOutputDb) {
        // Turning back to unity from below can land on -0.0, which would be
        // written "-0.0". Settled here rather than in snapOutputDb(), where
        // -ffast-math is free to treat the two zeros as one and drop the fix.
        const double db = std::abs(*m_savedOutputDb) < kOutputStepDb / 2 ? 0.0 : *m_savedOutputDb;
        lines.append(QStringLiteral("%1=%2").arg(kOutputKey).arg(db, 0, 'f', 1));
    }
    if (m_savedBrightness) {
        lines.append(QStringLiteral("%1=%2").arg(kBrightnessKey).arg(*m_savedBrightness));
    }
    // Written aside and renamed into place, so a plug pulled mid-write leaves
    // the previous file whole rather than this one half-written.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        kLogger.warning() << "could not save the levels to" << m_path << ":" << file.errorString();
        return;
    }
    file.write((lines.join(QChar('\n')) + QChar('\n')).toUtf8());
    if (!file.commit()) {
        kLogger.warning() << "could not save the levels to" << m_path << ":" << file.errorString();
    }
}

int DeckLevels::readRawBrightness() const {
    if (!hasBacklight()) {
        return -1;
    }
    QFile file(QDir(m_backlight).filePath(QStringLiteral("brightness")));
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    bool ok = false;
    const int raw = QString::fromLatin1(file.readAll()).trimmed().toInt(&ok);
    return ok ? raw : -1;
}

bool DeckLevels::writeBrightness(int percent) {
    if (!hasBacklight()) {
        return false;
    }
    const int raw = static_cast<int>(std::lround(percent / 100.0 * m_maxBrightness));
    // Unbuffered, so a write the driver refuses fails here, where it can be
    // reported, rather than silently on close. ExistingOnly, so this can never
    // create a stray file where a backlight used to be.
    QFile file(QDir(m_backlight).filePath(QStringLiteral("brightness")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::ExistingOnly | QIODevice::Unbuffered) ||
            file.write(QByteArray::number(raw) + '\n') < 0) {
        kLogger.warning() << "could not set the backlight through" << file.fileName() << ":"
                          << file.errorString();
        return false;
    }
    return true;
}

} // namespace deck
} // namespace mixxx
