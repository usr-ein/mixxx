#include "library/deck/setrecording.h"

#include <QStringList>
#include <algorithm>
#include <cmath>

namespace mixxx {
namespace deck {
namespace setrecording {

namespace {

QString twoDigits(qint64 n) {
    return QStringLiteral("%1").arg(n, 2, 10, QChar('0'));
}

} // namespace

QString digitsOf(quint32 number) {
    return QStringLiteral("%1").arg(number % 100000000u, 8, 10, QChar('0'));
}

QString audioFileName(const QString& digits) {
    return QStringLiteral("recorded-set-%1.mp3").arg(digits);
}

QString trackListFileName(const QString& digits) {
    return QStringLiteral("recorded-set-track-list-%1.txt").arg(digits);
}

QString offsetText(qint64 ms, bool hours) {
    const qint64 seconds = std::max<qint64>(ms, 0) / 1000;
    if (hours) {
        return QStringLiteral("%1:%2:%3")
                .arg(seconds / 3600)
                .arg(twoDigits(seconds / 60 % 60), twoDigits(seconds % 60));
    }
    return QStringLiteral("%1:%2").arg(twoDigits(seconds / 60), twoDigits(seconds % 60));
}

QString lengthText(qint64 ms) {
    const qint64 seconds = std::max<qint64>(ms, 0) / 1000;
    if (seconds >= 3600) {
        return offsetText(ms, true);
    }
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(twoDigits(seconds % 60));
}

QString sizeText(qint64 bytes) {
    if (bytes >= 1000LL * 1000 * 1000) {
        return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / 1e9, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg((std::max<qint64>(bytes, 0) + 500000) / 1000000);
}

QString trackListText(const QString& digits, qint64 lengthMs, const QList<TrackListLine>& lines) {
    const bool hours = lengthMs >= 3600 * 1000;
    QStringList out;
    out.append(QStringLiteral("Recorded set %1 · %2").arg(digits, lengthText(lengthMs)));
    out.append(QString());
    if (lines.isEmpty()) {
        // Said, rather than an empty file that looks like a failed write.
        out.append(QStringLiteral("Nothing played on the link while it recorded."));
    }
    for (const TrackListLine& line : lines) {
        QString name;
        if (line.title.isEmpty()) {
            name = QStringLiteral("Unknown track");
        } else if (line.artist.isEmpty()) {
            name = line.title;
        } else {
            name = QStringLiteral("%1 - %2").arg(line.artist, line.title);
        }
        out.append(QStringLiteral("%1  %2").arg(offsetText(line.atMs, hours), name));
    }
    return out.join(QChar('\n')) + QChar('\n');
}

double dbfs(float peak) {
    if (!(peak > 0.0f)) {
        return kSilenceDb;
    }
    return std::max(kSilenceDb, 20.0 * std::log10(static_cast<double>(peak)));
}

double meterPosition(double db) {
    if (!(db > kMeterFloorDb)) {
        return 0.0;
    }
    return std::min(1.0, (db - kMeterFloorDb) / -kMeterFloorDb);
}

void SilenceWatch::hear(qint64 nowMs, double peakDb) {
    if (peakDb >= kSilentBelowDb) {
        m_silentSinceMs = -1;
        m_kept = false;
        return;
    }
    if (m_silentSinceMs < 0) {
        m_silentSinceMs = nowMs;
    }
}

bool SilenceWatch::asking(qint64 nowMs) const {
    return m_silentSinceMs >= 0 && !m_kept && nowMs - m_silentSinceMs >= m_askAfterMs;
}

void SilenceWatch::keep() {
    m_kept = true;
}

void LevelMeter::update(qint64 nowMs, double peakDb) {
    const double db = std::max(peakDb, kMeterFloorDb);
    const qint64 elapsedMs = m_lastMs < 0 ? 0 : std::max<qint64>(nowMs - m_lastMs, 0);
    m_lastMs = nowMs;
    const double fall = kFallDbPerSecond * static_cast<double>(elapsedMs) / 1000.0;
    // Up at once, down at its rate.
    m_levelDb = std::max(db, std::max(m_levelDb - fall, kMeterFloorDb));
    if (m_levelDb >= m_holdDb) {
        m_holdDb = m_levelDb;
        m_heldSinceMs = nowMs;
    } else if (nowMs - m_heldSinceMs > kHoldMs) {
        // Held long enough: it follows, at the same rate, never below the bar.
        m_holdDb = std::max(m_holdDb - fall, m_levelDb);
    }
}

} // namespace setrecording
} // namespace deck
} // namespace mixxx
