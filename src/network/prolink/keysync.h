#pragma once

#include "proto/keys.pb.h"

namespace mixxx {
namespace prolink {

/// KEY SYNC, as a state machine and nothing else.
///
/// A CDJ's KEY SYNC pitches this deck until it is in the same key as the deck
/// the room is following. The rules below are what separate that from "track
/// the master's key", which is what it looks like from outside and is not what
/// a DJ wants:
///
///  1. **It can only be engaged against a master.** Another player has to hold
///     tempo master and we have to know what key its track is in. Without both
///     there is nothing to sync *to*, and a button that engages against nothing
///     is a button that silently does nothing.
///  2. **Engaging latches the key.** What is captured is a key, not a deck. The
///     master moving to another player, that player loading something else, or
///     the whole network going away afterwards changes nothing here: this deck
///     is in a key now, and a key does not stop being a key.
///  3. **It never releases itself.** Only the DJ lets go. Dropping the sync
///     because the network moved would re-pitch a playing track under the
///     DJ's hands, which is the one thing this must never do.
///  4. **Re-latching takes a release first.** Off, then on, and the new key is
///     whatever the network is offering at that moment.
///
/// Rules 2 and 3 together are why this holds a key rather than reading one:
/// there is exactly one moment at which the network is consulted, and it is
/// `engage()`.
class KeySync {
  public:
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

    /// Whether pressing the button would engage it (rule 1).
    ///
    /// Says nothing about whether it is *already* engaged: an engaged sync is
    /// released by the same press whether or not the network still offers one.
    static bool canEngage(const Link& link) {
        return link.otherIsMaster && link.masterKey != mixxx::track::io::key::INVALID;
    }

    bool engaged() const {
        return m_engaged;
    }

    /// The latched key, or INVALID while released.
    mixxx::track::io::key::ChromaticKey target() const {
        return m_target;
    }

    /// Engage against *link*, returning whether it took.
    ///
    /// False leaves everything untouched, which is what makes a refusal safe to
    /// answer by simply putting the button back.
    bool engage(const Link& link);

    /// Let go, forgetting the key. What to do with the deck's pitch afterwards
    /// is the caller's, not this class's.
    void release();

  private:
    bool m_engaged = false;
    mixxx::track::io::key::ChromaticKey m_target = mixxx::track::io::key::INVALID;
};

} // namespace prolink
} // namespace mixxx
