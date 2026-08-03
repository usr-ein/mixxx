#pragma once

#include "track/keyutils.h"

namespace mixxx {
namespace deck {
namespace camelot {

/// Position on the Camelot wheel, 1..12, or 0 when the key is unknown.
///
/// Open Key and Camelot are the same wheel with the numbering rotated by seven:
/// Open Key 1 (C major / A minor) is Camelot 8. Minor is Camelot's A ring and
/// major its B ring, which is the opposite way round from how Open Key spells
/// them — hence both conversions rather than one.
inline int number(mixxx::track::io::key::ChromaticKey key) {
    const int openKey = KeyUtils::keyToOpenKeyNumber(key);
    if (openKey < 1 || openKey > 12) {
        return 0;
    }
    return ((openKey + 6) % 12) + 1;
}

/// A sortable index in Camelot order: 1A, 1B, 2A, 2B … 12B.
///
/// **This is why there is a column for it.** SQL has no idea what a Camelot
/// wheel is, and the obvious column to sort on — `key_id` — is Mixxx's
/// ChromaticKey enum, i.e. *chromatic* order. Sorting a track list on that
/// yields 11A, 6A, 1A, which is correct chromatically and nonsense to a DJ. The
/// order has to be computed once, at ingest, and stored.
///
/// Unknown keys sort last, together, rather than being scattered through the
/// list by whatever their enum value happens to be.
inline int order(mixxx::track::io::key::ChromaticKey key) {
    const int camelotNumber = number(key);
    if (camelotNumber == 0) {
        return 1000;
    }
    return camelotNumber * 2 + (KeyUtils::keyIsMajor(key) ? 1 : 0);
}

inline int orderFromKeyId(int keyId) {
    return order(KeyUtils::keyFromNumericValue(keyId));
}

/// Whether *candidate* mixes with *playing*: the same key, its relative
/// major/minor, or one step round the wheel either way. Nothing else.
///
/// Deliberately narrower than KeyUtils::getCompatibleKeys(), which also returns
/// the relative key's neighbours — the diagonal moves. Six keys out of
/// twenty-four is enough of a list turning green that the colour stops meaning
/// anything.
inline bool isCompatible(mixxx::track::io::key::ChromaticKey playing,
        mixxx::track::io::key::ChromaticKey candidate) {
    if (playing == mixxx::track::io::key::INVALID ||
            candidate == mixxx::track::io::key::INVALID) {
        return false;
    }
    if (playing == candidate) {
        return true;
    }
    const int playingNumber = number(playing);
    const int candidateNumber = number(candidate);
    if (playingNumber == 0 || candidateNumber == 0) {
        return false;
    }
    const bool sameRing = KeyUtils::keyIsMajor(playing) == KeyUtils::keyIsMajor(candidate);
    if (playingNumber == candidateNumber) {
        return !sameRing; // The relative major/minor.
    }
    if (!sameRing) {
        return false;
    }
    const int forward = playingNumber == 12 ? 1 : playingNumber + 1;
    const int back = playingNumber == 1 ? 12 : playingNumber - 1;
    return candidateNumber == forward || candidateNumber == back;
}

} // namespace camelot
} // namespace deck
} // namespace mixxx
