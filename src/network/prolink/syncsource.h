#pragma once

#include <vector>

namespace mixxx {
namespace prolink {

/// One other device on the network, as SYNC needs to see it.
///
/// Plain data rather than the Rust bridge's Player, so the choice below can be
/// tested without a network.
struct SyncPeer {
    int number = 0;
    /// Whether any status packet has been heard; the status fields below mean
    /// nothing otherwise.
    bool hasStatus = false;
    /// How long ago the last status arrived, ms; negative for never.
    double statusAgeMs = -1.0;
    bool isMaster = false;
    /// The device it is handing master to, or 0.
    int yieldingTo = 0;
    bool isSynced = false;
    /// Status says the playhead moves on its own and is heard: playing,
    /// looping, or the emergency loop of a pulled medium. Not auditioning its
    /// cue (CUE held): moving, but not a deck anybody mixes against.
    bool playing = false;
    /// Status says it is auditioning its cue: CUE held, the playhead moving
    /// until it is let go.
    bool auditioning = false;
    /// The tempo from its last beat packet, pitch applied; <= 0 for none.
    double beatBpm = -1.0;
    /// How long ago that beat packet arrived, ms; negative for never.
    double beatAgeMs = -1.0;
    /// The tempo its status states, pitch applied; <= 0 for none.
    double statusBpm = -1.0;
    /// Where it is in its bar from its beats, 0..1; negative for none.
    double barPhase = -1.0;
    /// Where its status puts it in its bar, to the beat below, 0..1; negative
    /// for none. Beats stop with the platter and status does not, so this is
    /// where a deck that is not playing stands.
    double barPosition = -1.0;
};

/// What a lit SYNC on this deck follows, this poll.
struct SyncSource {
    enum class Kind {
        /// Nothing to follow: the fader leads.
        None,
        /// The tempo master.
        Master,
        /// No master on the network; a playing deck stands in (owner decision
        /// 1).
        Fallback,
    };
    Kind kind = Kind::None;
    int device = 0;
    /// The tempo to follow; <= 0 when there is a source but no tempo to take
    /// from it, which means hold the tempo already playing.
    double bpm = 0.0;
    /// Its phase, 0..1 in the bar; negative for none.
    double barPhase = -1.0;
    /// Whether barPhase is a phase to correct against: from beats, fresh.
    bool phaseLive = false;
    /// Its next beat is due and has not arrived, so barPhase is standing still
    /// at the end of the beat while ours moves on.
    bool beatOverdue = false;
    /// The master is there and not playing: paused, cued, auditioning its cue,
    /// or with nothing loaded. SYNC lets go of it (owner decision 15).
    bool masterStopped = false;
};

/// A status packet older than this describes a deck that may have gone. A deck
/// sends one every ~200 ms.
constexpr double kStatusFreshMs = 1000.0;

/// Player numbers. A CDJ-3000 rig goes up to six; only 1-4 can be browsed,
/// and so only 1-4 is a player number this deck can hold. A mixer is 33 and
/// rekordbox 17 and up.
constexpr int kFirstPlayer = 1;
constexpr int kLastPlayer = 6;
constexpr int kLastBrowsablePlayer = 4;

inline bool isPlayerNumber(int number) {
    return number >= kFirstPlayer && number <= kLastPlayer;
}

/// A number this deck can hold and be seen as a player at.
inline bool isOurPlayerNumber(int number) {
    return number >= kFirstPlayer && number <= kLastBrowsablePlayer;
}

/// Whether *peer*'s status is recent enough to describe it as it is now.
inline bool isHeard(const SyncPeer& peer) {
    return peer.hasStatus && peer.statusAgeMs >= 0.0 && peer.statusAgeMs < kStatusFreshMs;
}

/// Another player, still being heard.
inline bool isHeardPlayer(const SyncPeer& peer, int ours) {
    return isPlayerNumber(peer.number) && peer.number != ours && isHeard(peer);
}

/// Another player, still being heard, that claims tempo master.
inline bool isHeardMasterClaim(const SyncPeer& peer, int ours) {
    return peer.isMaster && isHeardPlayer(peer, ours);
}

/// Choose what a lit SYNC follows.
///
/// *ours* is this deck's player number, 0 before one is held.
///
///  * **The tempo master always wins** -- on fresh status, and not while it is
///    handing master to someone else (its successor is about to be master).
///    A master that is playing is followed: tempo, and phase when its beats
///    are fresh. A master that is not playing is reported as stopped; the
///    caller releases SYNC.
///  * **With no master**, the lowest-numbered deck that is playing and has a
///    tempo -- but never a deck that is itself synced and numbered above us:
///    two synced decks with no master would otherwise follow each other, each
///    copying the other's tempo, and neither could ever take master.
///  * Only players 1-6, and never ourselves.
SyncSource chooseSyncSource(const std::vector<SyncPeer>& peers, int ours);

/// The deck the phase meter draws above ours, this poll.
struct MeterDeck {
    /// Its player number; 0 for nobody to draw.
    int device = 0;
    bool isMaster = false;
    /// Where it is in its bar, 0..1; negative for nobody. Not 0, which is a
    /// deck on its downbeat.
    double barPhase = -1.0;
    /// Drawn from its beats as they arrive, so it moves. False for a deck held
    /// where its status puts it, to the beat.
    bool live = false;
};

/// Choose the deck the phase meter draws above ours: the deck this one is
/// mixing against, which is not always what SYNC follows.
///
///  * **The master first, playing or not**, then any other deck playing, then
///    any deck with a place on its grid at all -- one being cued up is still
///    the deck the next mix lines up against. Ties go to the lowest number.
///  * **Drawn from its beats while they arrive**, from its status otherwise:
///    beats stop with the platter, status keeps coming, and a paused deck the
///    DJ winds back moves a beat at a time. Never from beats while its status
///    says it is stopped, nor once they have stopped: the phase then stands
///    at the end of the last beat.
///  * **Only decks still heard.** One that has gone keeps its last status
///    until it is forgotten some 30 s later. With no status from anyone --
///    we are not announcing -- a deck is drawn while its beats arrive.
///  * Only players 1-6, and never ourselves: our own beats come back to us.
MeterDeck chooseMeterDeck(const std::vector<SyncPeer>& peers, int ours);

} // namespace prolink
} // namespace mixxx
