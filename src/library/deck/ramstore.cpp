#include "library/deck/ramstore.h"

#include <QDir>
#include <QStandardPaths>
#include <QStorageInfo>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("RamStore");

/// Candidates, best first. Both are tmpfs on the deck; they differ in how the
/// kernel sized them, which is the entire reason this list has two entries.
///
/// `/tmp` is 1.9 GB here and `/run` is 760 MB, because systemd sizes `/run` at
/// a fraction of RAM and `/tmp` at half of it. Neither number is in anyone's
/// control, so they are measured below rather than chosen.
const char* const kCandidates[] = {
        "/tmp/trimixxx",
        "/run/trimixxx",
};

/// A filesystem smaller than this is not worth using for audio: one lossless
/// track is 60 MB, and a store that cannot hold two is a store that thrashes.
constexpr qint64 kMinimumUseful = 256LL * 1024 * 1024;

/// How much of the store to actually spend. The rest is headroom, because
/// filling a tmpfs is not like filling a disk -- it is taking memory away from
/// the process that is playing the music.
constexpr double kUsableFraction = 0.6;

struct Chosen {
    QString root;
    qint64 budget = 0;
};

Chosen chooseOnce() {
    Chosen best;
    qint64 bestFree = 0;

    for (const char* candidate : kCandidates) {
        const QString path = QString::fromLatin1(candidate);
        if (!QDir().mkpath(path)) {
            continue;
        }
        const QStorageInfo info(path);
        if (!info.isValid() || !info.isReady()) {
            continue;
        }
        const qint64 free = info.bytesAvailable();
        // A RAM-backed filesystem, ideally. Not a hard requirement -- a dev box
        // has neither of these as tmpfs and should still work -- but the deck
        // does, and the whole point is not to write to the card.
        const bool isRam = info.fileSystemType() == QByteArrayLiteral("tmpfs") ||
                info.fileSystemType() == QByteArrayLiteral("ramfs");
        if (free > bestFree && (isRam || best.root.isEmpty())) {
            bestFree = free;
            best.root = path;
        }
    }

    if (best.root.isEmpty()) {
        // Neither exists, which means this is not the deck. The temp directory
        // is still better than the user's home, and on any sane machine it is
        // at least as fast.
        best.root = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                            .filePath(QStringLiteral("trimixxx"));
        QDir().mkpath(best.root);
        bestFree = QStorageInfo(best.root).bytesAvailable();
    }

    best.budget = static_cast<qint64>(static_cast<double>(bestFree) * kUsableFraction);
    const QStorageInfo info(best.root);
    kLogger.info() << "RAM store" << best.root << "on" << info.fileSystemType() << "--"
                   << (bestFree / (1024 * 1024)) << "MB free, budget"
                   << (best.budget / (1024 * 1024)) << "MB";
    if (bestFree < kMinimumUseful) {
        // Said loudly, because everything downstream will look like a different
        // fault: tracks that will not cache, a stick pulled that stops the
        // music, a consuming player cut off mid-track.
        kLogger.warning() << "the RAM store is very small; caching will barely work";
    }
    return best;
}

const Chosen& chosen() {
    // Measured once. The free space moves as the deck runs -- that is what the
    // budget is for -- and a budget that moved with it would let two callers
    // each believe they had room for the same bytes.
    static const Chosen value = chooseOnce();
    return value;
}
} // namespace

namespace mixxx {
namespace deck {

QString RamStore::path(const QString& name) {
    const QString directory = QDir(chosen().root).filePath(name);
    QDir().mkpath(directory);
    return directory;
}

qint64 RamStore::budget(double share) {
    return static_cast<qint64>(static_cast<double>(chosen().budget) * qBound(0.0, share, 1.0));
}

qint64 RamStore::available() {
    return QStorageInfo(chosen().root).bytesAvailable();
}

QString RamStore::root() {
    return chosen().root;
}

} // namespace deck
} // namespace mixxx
