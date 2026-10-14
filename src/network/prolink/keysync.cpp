#include "network/prolink/keysync.h"

#include "track/keyutils.h"

namespace mixxx {
namespace prolink {

namespace {

bool isKey(mixxx::track::io::key::ChromaticKey key) {
    return mixxx::track::io::key::ChromaticKey_IsValid(key) &&
            key != mixxx::track::io::key::INVALID;
}

} // namespace

int KeySync::foldShift(int semitones) {
    int folded = semitones % 12;
    if (folded < 0) {
        folded += 12;
    }
    if (folded > kHighestShift) {
        folded -= 12;
    }
    return folded;
}

std::optional<int> KeySync::shiftOntoCamelotNumber(
        mixxx::track::io::key::ChromaticKey ours,
        mixxx::track::io::key::ChromaticKey master) {
    if (!isKey(ours) || !isKey(master)) {
        return std::nullopt;
    }
    const bool major = KeyUtils::keyIsMajor(ours);
    int targetTonic = KeyUtils::keyToTonic(master);
    if (major != KeyUtils::keyIsMajor(master)) {
        // The key relative to the master's, in our mode, which shares its
        // Camelot number: a minor key's relative major is a minor third up,
        // a major key's relative minor a minor third down.
        targetTonic += major ? 3 : -3;
    }
    return foldShift(targetTonic - KeyUtils::keyToTonic(ours));
}

mixxx::track::io::key::ChromaticKey KeySync::keyAtShift(
        mixxx::track::io::key::ChromaticKey ours, int shift) {
    return KeyUtils::scaleKeySteps(ours, shift);
}

int KeySync::press(const Link& link, mixxx::track::io::key::ChromaticKey ours, int shift) {
    if (!link.otherIsMaster) {
        return 0; // Rule 3: nothing to match.
    }
    const std::optional<int> target = shiftOntoCamelotNumber(ours, link.masterKey);
    if (!target || *target == shift) {
        return 0; // Rule 3, or rule 2: already there.
    }
    return *target; // Rule 1.
}

} // namespace prolink
} // namespace mixxx
