#pragma once

namespace mixxx {
namespace prolink {

/// Which of the two things decides this deck's tempo.
///
/// Two devices, exactly one of them tempo master, BEAT SYNC independently on or
/// off on each: eight states. They collapse to this one question, because of a
/// single observation —
///
/// > **A deck's own BEAT SYNC only matters while that deck is not master.**
///
/// The master is the reference. It cannot follow itself, so SYNC on the master
/// is inert: it changes the flag published on the wire and nothing else. See
/// `docs/tempo-sync.md` for the full table and the reasoning behind the cases
/// the protocol does not settle.
///
/// # What is deliberately not here
///
/// **Pickup.** While this deck follows a master the fader under the DJ's hand
/// is connected to nothing, and the two drift apart; the fader then has to come
/// back and meet the tempo before it takes over, or the first touch of it drops
/// the tempo by however far the master had moved.
///
/// That is `soft-takeover` in `TriMixxx.midi.xml`, which is Mixxx's own and is
/// applied after the 14-bit fader value is assembled. Writing a second one here
/// would mean two implementations racing over the same fader.
class SyncTempo {
  public:
    enum class Source {
        /// This deck's own pitch fader, whatever the rest of the network is
        /// doing. Mixxx's soft takeover decides when a moved fader takes
        /// effect.
        Fader,
        /// The network master's tempo, and its phase with it. The fader is
        /// decoupled for as long as this lasts.
        Master,
    };

    struct State {
        /// Whether BEAT SYNC is engaged on this deck.
        bool syncEnabled = false;
        /// Whether this deck holds tempo master.
        bool isMaster = false;
        /// The master's effective tempo, or 0 when nobody holds it or the
        /// holder has no track.
        double masterBpm = 0.0;
    };

    static Source decide(const State& state);
};

} // namespace prolink
} // namespace mixxx
