#include "library/deck/sessionpurge.h"

#include <QDir>
#include <QFile>

#include "library/trackcollectionmanager.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("SessionPurge");

const QString kBootIdPath = QStringLiteral("/proc/sys/kernel/random/boot_id");
/// Beside mixxx.cfg. Not in the config itself: that is only written on a clean
/// exit, and a crash is precisely the case the record has to survive.
const QString kMarkerName = QStringLiteral("trimixxx-boot-id");
} // namespace

namespace mixxx {
namespace deck {

bool purgeSessionTracksIfNewBoot(TrackCollectionManager* pTrackCollectionManager,
        const QString& settingsPath,
        const QStringList& roots) {
    if (!pTrackCollectionManager || settingsPath.isEmpty()) {
        return false;
    }
    QFile bootIdFile(kBootIdPath);
    if (!bootIdFile.open(QIODevice::ReadOnly)) {
        // Not Linux: there is no boot to key on, and purging at every start
        // would throw away a development library for nothing.
        return false;
    }
    const QByteArray bootId = bootIdFile.readAll().trimmed();
    if (bootId.isEmpty()) {
        return false;
    }

    QFile marker(QDir(settingsPath).filePath(kMarkerName));
    QByteArray seen;
    if (marker.open(QIODevice::ReadOnly)) {
        seen = marker.readAll().trimmed();
        marker.close();
    }
    if (seen == bootId) {
        kLogger.info() << "same boot as the last start; keeping the session's analysis";
        return false;
    }

    for (const QString& root : roots) {
        if (root.isEmpty()) {
            continue;
        }
        kLogger.info() << "first start of this boot; forgetting tracks under" << root;
        pTrackCollectionManager->purgeAllTracks(QDir(root));
    }

    // Written after the purge, not before, so a crash part-way through purges
    // again next time rather than never.
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        marker.write(bootId);
        marker.write("\n");
    } else {
        kLogger.warning() << "could not record the boot id at" << marker.fileName()
                          << "-- the next start will purge again";
    }
    return true;
}

} // namespace deck
} // namespace mixxx
