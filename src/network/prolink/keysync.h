#pragma once

#include <optional>

#include "proto/keys.pb.h"

namespace mixxx {
namespace prolink {

/// KEY SYNC's rules, and nothing else: which shift a press asks for.
///
/// A **shift** is what this deck does to the key of the track on it: a whole
/// number of semitones, from -5 to +6, on top of the track's own key. Pitch
/// shifting keeps the mode, so the twelve shifts reach the twelve keys of the
/// track's own mode, one each. A semitone is seven steps round the Camelot
/// wheel, and seven and twelve share no factor, so -5..+6 lands on every
/// Camelot number exactly once, whatever key the track is in. The tritone is
/// the one key both ways reach, and it is +6.
///
/// The rules (Sam, 2026-10-14):
///
///  1. **A press puts the deck on the master's Camelot number:** the master's
///     key, or the key relative to it in our own mode. The master in A minor
///     (8A) and our track in C major (8B) is already there.
///  2. **A press already there goes back to the original.** The pad that
///     matched is the pad that lets go.
///  3. **A press with nothing to match goes back to the original too:** no
///     other player holds tempo master, or nobody knows its track's key, or
///     our own track's. A lit pad that did nothing when pressed would leave
///     the key list as the only way back.
///  4. **Nothing else moves the shift.** The network is read at the press and
///     only then: the master loading something else, handing over or leaving
///     changes nothing here, because re-pitching a playing track under the
///     DJ's hands is the one thing this must never do.
///
/// Picking a key off the list sets the shift directly and needs no rule here.
class KeySync {
  public:
    static constexpr int kLowestShift = -5;
    static constexpr int kHighestShift = 6;

    /// What the network is offering, this instant.
    struct Link {
        /// A player that is **not us** holds tempo master.
        bool otherIsMaster = false;
        /// That player's track key. INVALID when it has no track loaded, when
        /// its medium has not been read yet, or when rekordbox never wrote a
        /// key for the track — all of which are ordinary and none of which is
        /// an error.
        mixxx::track::io::key::ChromaticKey masterKey = mixxx::track::io::key::INVALID;
    };

    /// *semitones* as the shift that reaches the same key: folded into
    /// -5..+6, since a whole octave changes no key.
    static int foldShift(int semitones);

    /// The shift that puts a track in *ours* on the Camelot number of
    /// *master* (rule 1). Empty when either key is unknown.
    static std::optional<int> shiftOntoCamelotNumber(
            mixxx::track::io::key::ChromaticKey ours,
            mixxx::track::io::key::ChromaticKey master);

    /// The key a track in *ours* plays in with *shift* applied. INVALID when
    /// *ours* is.
    static mixxx::track::io::key::ChromaticKey keyAtShift(
            mixxx::track::io::key::ChromaticKey ours, int shift);

    /// The shift a press asks for (rules 1 to 3), from what the network
    /// offers, the track's own key and the shift in force.
    static int press(const Link& link, mixxx::track::io::key::ChromaticKey ours, int shift);
};

} // namespace prolink
} // namespace mixxx
