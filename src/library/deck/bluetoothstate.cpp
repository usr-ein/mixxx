#include "library/deck/bluetoothstate.h"

#include <QCoreApplication>
#include <algorithm>

namespace mixxx {
namespace deck {
namespace bluetooth {

namespace {
QString tr(const char* text) {
    return QCoreApplication::translate("BluetoothReceiver", text);
}

/// A device that says nothing about itself is still "the device": it can be
/// a phone or a Mac (Sam).
QString nameOr(const QString& name) {
    return name.isEmpty() ? tr("the device") : name;
}
} // namespace

Status status(const Facts& facts) {
    switch (facts.radio) {
    case Facts::Radio::Off:
        return Status::Off;
    case Facts::Radio::Starting:
        return Status::Starting;
    case Facts::Radio::Failed:
        return Status::Failed;
    case Facts::Radio::Up:
        break;
    }
    if (!facts.adapter) {
        return Status::NoAdapter;
    }
    // A code waiting outranks everything: it is a question the DJ has to
    // answer, and the device gives up on it after half a minute.
    if (facts.hasPasskey) {
        return Status::Confirming;
    }
    if (!facts.connectedName.isEmpty()) {
        // Streaming is what is heard; a player saying "playing" with no
        // stream is a device that has not started sending yet.
        return facts.streaming ? Status::Playing : Status::Connected;
    }
    if (!facts.connectingName.isEmpty()) {
        return Status::Connecting;
    }
    return facts.discoverable ? Status::Pairing : Status::Waiting;
}

QString statusText(const Facts& facts, const QString& deckName) {
    switch (status(facts)) {
    case Status::Off:
        return tr("Bluetooth off");
    case Status::Starting:
        return tr("Turning Bluetooth on…");
    case Status::NoAdapter:
        return tr("No Bluetooth radio on this deck");
    case Status::Failed:
        return facts.failure.isEmpty()
                ? tr("Bluetooth would not start")
                : tr("Bluetooth would not start: %1").arg(facts.failure);
    case Status::Waiting:
        return facts.devices.isEmpty() ? tr("Waiting for a device · + to pair one")
                                       : tr("Pick a device to play from");
    case Status::Connecting:
        return tr("Connecting to %1…").arg(facts.connectingName);
    case Status::Pairing:
        return tr("Visible as %1: pair from the device").arg(deckName);
    case Status::Confirming:
        return tr("Pair with %1? Same code on both: Confirm")
                .arg(nameOr(facts.pairingName));
    case Status::Connected:
        return tr("Connected to %1").arg(nameOr(facts.connectedName));
    case Status::Playing:
        return tr("Playing from %1").arg(nameOr(facts.connectedName));
    }
    return QString();
}

QString formatTime(qint64 ms) {
    if (ms < 0) {
        return QStringLiteral("-:--");
    }
    const qint64 seconds = ms / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;
    const qint64 rest = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QChar('0'))
                .arg(rest, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(rest, 2, 10, QChar('0'));
}

QString formatPasskey(quint32 passkey) {
    const QString digits = QStringLiteral("%1").arg(passkey % 1000000, 6, 10, QChar('0'));
    return digits.left(3) + QChar(' ') + digits.mid(3);
}

qint64 positionNow(const Facts& facts, qint64 nowMs) {
    qint64 position = facts.positionMs;
    if (facts.playerStatus == QStringLiteral("playing") && nowMs > facts.positionStampMs) {
        position += nowMs - facts.positionStampMs;
    }
    if (facts.track.durationMs > 0) {
        position = qMin(position, facts.track.durationMs);
    }
    return qMax<qint64>(position, 0);
}

void setPosition(Facts* pFacts, qint64 positionMs, qint64 nowMs) {
    pFacts->positionMs = qMax<qint64>(positionMs, 0);
    pFacts->positionStampMs = nowMs;
}

void setStatus(Facts* pFacts, const QString& status, qint64 nowMs) {
    pFacts->positionMs = positionNow(*pFacts, nowMs);
    pFacts->positionStampMs = nowMs;
    pFacts->playerStatus = status;
}

bool isPlaying(const QString& playerStatus) {
    return playerStatus == QStringLiteral("playing") ||
            playerStatus == QStringLiteral("forward-seek") ||
            playerStatus == QStringLiteral("reverse-seek");
}

QString playPauseMethod(const QString& playerStatus) {
    return isPlaying(playerStatus) ? QStringLiteral("Pause") : QStringLiteral("Play");
}

bool mayConnect(const QString& device, const QString& picked, bool paired, bool discoverable) {
    return (!picked.isEmpty() && device == picked) || (discoverable && !paired);
}

QString autoPick(const QList<Device>& devices, const QString& picked) {
    if (!picked.isEmpty()) {
        return QString();
    }
    for (const Device& device : devices) {
        if (device.starred) {
            return device.path;
        }
    }
    return QString();
}

QList<Device> sortedDevices(QList<Device> devices) {
    std::stable_sort(devices.begin(), devices.end(), [](const Device& a, const Device& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return devices;
}

bool screenOffDue(qint64 lastActivityMs, qint64 nowMs) {
    return nowMs - lastActivityMs >= kScreenOffAfterMs;
}

} // namespace bluetooth
} // namespace deck
} // namespace mixxx
