#pragma once

namespace mixxx {
namespace prolink {

/// Who decides this deck's tempo, and when the physical fader gets it back.
///
/// # The problem
///
/// While this deck follows a network master, its tempo comes from the wire and
/// the fader under the DJ's hand is not connected to anything. The two then
/// disagree — the master drags the deck from 130 to 140 while the fader still
/// sits at 130 — and the moment the deck takes over as master, obeying the
/// fader would drop the tempo by ten BPM with nobody having touched it.
///
/// So the fader has to **catch up before it takes effect**: it does nothing at
/// all until it crosses the tempo actually playing, and only then does moving
/// it move the deck. This is the same pickup a CDJ does, and it is why on real
/// hardware you sometimes have to run the fader up to meet the tempo before it
/// responds.
///
/// # The rules, in full
///
///  * **Not synced** — the fader is always in control.
///  * **Synced, following a master** — the master's tempo is the tempo. The
///    fader is decoupled, and which side of the playing tempo it happens to sit
///    on is remembered, because that is the direction it will have to come from
///    to catch up.
///  * **Synced, and we are master** — the tempo stays where the master left it
///    until the fader crosses it. Before the crossing, moving the fader changes
///    nothing on the wire. After it, the fader is the tempo and the rest of the
///    network follows it.
///
/// Crossing means exactly that, in whichever direction: a fader left *below*
/// the playing tempo catches when it reaches or passes it going up, and one
/// left *above* catches coming down.
///
/// Pure and stateless apart from the two things it has to remember, so the
/// whole of the behaviour above is exercised by `synctempo_test.cpp` without a
/// deck, a network, or a fader.
class SyncTempo {
  public:
    struct Inputs {
        /// Whether SYNC is engaged on this deck.
        bool syncEnabled = false;
        /// Whether this deck currently holds tempo master.
        bool isMaster = false;
        /// The network master's effective tempo, or 0 when nobody holds it.
        double masterBpm = 0.0;
        /// The loaded track's own tempo, before the fader. 0 with no track.
        double fileBpm = 0.0;
        /// Where the physical fader is, as a percentage: −6.0 is six percent
        /// slow. This is the *hardware* position, which is not the same thing
        /// as the tempo being played.
        double faderPercent = 0.0;
    };

    struct Output {
        /// What the deck should play at. Zero means "no opinion": there is no
        /// track, so nothing should be written and nothing published.
        double effectiveBpm = 0.0;
        /// Whether the fader is connected to the tempo right now. False while
        /// following a master, and false after taking over until the fader has
        /// caught up.
        bool faderInControl = true;
    };

    Output update(const Inputs& inputs);

    /// Forget everything, as when a track is loaded or the deck is emptied.
    void reset();

  private:
    double m_effectiveBpm = 0.0;
    bool m_haveEffective = false;
    bool m_faderInControl = true;
    /// Which side of the playing tempo the fader was left on, and therefore the
    /// direction it has to travel to catch it.
    bool m_faderWasAbove = false;
};

} // namespace prolink
} // namespace mixxx
