#pragma once

#include <QString>
#include <QStringList>

class TrackCollectionManager;

namespace mixxx {
namespace deck {

/// Forget, once per boot, everything Mixxx stored about the session's tracks.
///
/// **Why there is anything to forget.** The deck never plays off a stick: it
/// plays a copy in the RAM store, so every track it loads becomes a row in
/// Mixxx's own library, keyed by that copy's path, and Mixxx's analysis of it
/// -- beats, cues, the waveform files -- is saved against that row on the SD
/// card. Those copies are gone after a reboot and the rows are not, so without
/// this the library grows by a row per track ever played, and a copy's name
/// coming round again would hand a new file an old file's beats.
///
/// **Why once per boot rather than at every start.** "Wiped on reboot" is the
/// rule (docs/plain-usb-plan.md D1): a Mixxx that crashes mid-set and comes back
/// within the same boot keeps the analysis of everything already played.
/// The boot is told apart by `/proc/sys/kernel/random/boot_id`, recorded beside
/// the settings; anywhere that file does not exist, nothing is purged.
///
/// *roots* are the directories whose tracks are the session's: the RAM store,
/// the track cache's disk tier, and the mount root of the sticks themselves.
/// Returns whether it purged.
bool purgeSessionTracksIfNewBoot(TrackCollectionManager* pTrackCollectionManager,
        const QString& settingsPath,
        const QStringList& roots);

} // namespace deck
} // namespace mixxx
