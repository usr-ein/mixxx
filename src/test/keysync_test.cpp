#include "network/prolink/keysync.h"

#include <gtest/gtest.h>

#include <QSet>
#include <QString>

#include "track/keyutils.h"

namespace {

using mixxx::prolink::KeySync;
namespace key = mixxx::track::io::key;

/// Every key there is: C major..B major, C minor..B minor.
QList<key::ChromaticKey> allKeys() {
    QList<key::ChromaticKey> keys;
    for (int value = key::C_MAJOR; value <= key::B_MINOR; ++value) {
        keys.append(static_cast<key::ChromaticKey>(value));
    }
    return keys;
}

/// The Camelot number, read off Mixxx's own Lancelot notation ("8A" -> 8)
/// rather than worked out again: what KeySync computes is checked against
/// what every key readout on the deck shows.
int camelotNumber(key::ChromaticKey key) {
    QString code = KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot);
    code.chop(1);
    return code.toInt();
}

KeySync::Link offering(key::ChromaticKey masterKey) {
    KeySync::Link link;
    link.otherIsMaster = true;
    link.masterKey = masterKey;
    return link;
}

/// Nobody else holds tempo master: we do, or nobody does.
KeySync::Link noMaster() {
    KeySync::Link link;
    link.otherIsMaster = false;
    link.masterKey = key::A_MINOR;
    return link;
}

} // namespace

TEST(KeySync, FoldsAnyShiftIntoMinusFiveToPlusSix) {
    EXPECT_EQ(0, KeySync::foldShift(0));
    EXPECT_EQ(-5, KeySync::foldShift(-5));
    EXPECT_EQ(6, KeySync::foldShift(6));
    EXPECT_EQ(6, KeySync::foldShift(-6));
    EXPECT_EQ(-5, KeySync::foldShift(7));
    EXPECT_EQ(-1, KeySync::foldShift(11));
    EXPECT_EQ(0, KeySync::foldShift(12));
    EXPECT_EQ(0, KeySync::foldShift(-12));
    EXPECT_EQ(5, KeySync::foldShift(-7));
    EXPECT_EQ(6, KeySync::foldShift(18));
}

// The list's premise: from any key, the twelve shifts reach twelve different
// Camelot numbers, all in the track's own mode.
TEST(KeySync, TheTwelveShiftsReachEveryCamelotNumberOnce) {
    for (const key::ChromaticKey ours : allKeys()) {
        QSet<int> numbers;
        for (int shift = KeySync::kLowestShift; shift <= KeySync::kHighestShift; ++shift) {
            const key::ChromaticKey shifted = KeySync::keyAtShift(ours, shift);
            EXPECT_EQ(KeyUtils::keyIsMajor(ours), KeyUtils::keyIsMajor(shifted))
                    << KeyUtils::keyDebugName(ours).toStdString() << " " << shift;
            numbers.insert(camelotNumber(shifted));
        }
        EXPECT_EQ(12, numbers.size()) << KeyUtils::keyDebugName(ours).toStdString();
    }
}

// Rule 1 from every key to every key: 24 x 24, both modes either side, so
// every offset and every relative case.
TEST(KeySync, LandsOnTheMastersCamelotNumberFromEveryKey) {
    for (const key::ChromaticKey ours : allKeys()) {
        for (const key::ChromaticKey master : allKeys()) {
            const std::optional<int> shift = KeySync::shiftOntoCamelotNumber(ours, master);
            ASSERT_TRUE(shift.has_value());
            EXPECT_GE(*shift, KeySync::kLowestShift);
            EXPECT_LE(*shift, KeySync::kHighestShift);
            const key::ChromaticKey shifted = KeySync::keyAtShift(ours, *shift);
            EXPECT_EQ(camelotNumber(master), camelotNumber(shifted))
                    << KeyUtils::keyDebugName(ours).toStdString() << " onto "
                    << KeyUtils::keyDebugName(master).toStdString();
            EXPECT_EQ(KeyUtils::keyIsMajor(ours), KeyUtils::keyIsMajor(shifted));
        }
    }
}

// All twelve offsets, spelled out against one master in each mode: a track
// sitting k semitones below the master's number needs +k.
TEST(KeySync, TakesEachOfTheTwelveOffsets) {
    for (int offset = KeySync::kLowestShift; offset <= KeySync::kHighestShift; ++offset) {
        // The same mode as the master (8A, A minor).
        const key::ChromaticKey minorTrack = KeySync::keyAtShift(key::A_MINOR, -offset);
        EXPECT_EQ(offset, KeySync::shiftOntoCamelotNumber(minorTrack, key::A_MINOR))
                << KeyUtils::keyDebugName(minorTrack).toStdString();
        // The other mode: a major track onto 8A is a track onto C major (8B).
        const key::ChromaticKey majorTrack = KeySync::keyAtShift(key::C_MAJOR, -offset);
        EXPECT_EQ(offset, KeySync::shiftOntoCamelotNumber(majorTrack, key::A_MINOR))
                << KeyUtils::keyDebugName(majorTrack).toStdString();
    }
}

// Sam's example: the master in A minor (8A), ours in C major (8B). Already
// there, both ways round.
TEST(KeySync, TheRelativeKeyIsAlreadyThere) {
    EXPECT_EQ(0, KeySync::shiftOntoCamelotNumber(key::C_MAJOR, key::A_MINOR));
    EXPECT_EQ(0, KeySync::shiftOntoCamelotNumber(key::A_MINOR, key::C_MAJOR));
    // 9A (E minor) under a G major master (9B).
    EXPECT_EQ(0, KeySync::shiftOntoCamelotNumber(key::E_MINOR, key::G_MAJOR));
}

// The tritone is the one key both ways reach; the list ends at +6, so +6.
TEST(KeySync, TheTritoneIsPlusSix) {
    EXPECT_EQ(6, KeySync::shiftOntoCamelotNumber(key::C_MAJOR, key::F_SHARP_MAJOR));
    EXPECT_EQ(6, KeySync::shiftOntoCamelotNumber(key::F_SHARP_MAJOR, key::C_MAJOR));
    EXPECT_EQ(6, KeySync::shiftOntoCamelotNumber(key::A_MINOR, key::E_FLAT_MINOR));
    // Across modes: E flat minor (2A) is relative to F sharp major (2B).
    EXPECT_EQ(6, KeySync::shiftOntoCamelotNumber(key::C_MAJOR, key::E_FLAT_MINOR));
    // One short of it goes down rather than up.
    EXPECT_EQ(-5, KeySync::shiftOntoCamelotNumber(key::C_MAJOR, key::G_MAJOR));
}

TEST(KeySync, NoShiftWithoutBothKeys) {
    EXPECT_FALSE(KeySync::shiftOntoCamelotNumber(key::INVALID, key::A_MINOR).has_value());
    EXPECT_FALSE(KeySync::shiftOntoCamelotNumber(key::A_MINOR, key::INVALID).has_value());
    EXPECT_EQ(key::INVALID, KeySync::keyAtShift(key::INVALID, 3));
}

// --- What a press does ------------------------------------------------------

// Rule 1. E minor (9A) under an A minor (8A) master: up a fourth.
TEST(KeySync, APressMatchesTheMaster) {
    EXPECT_EQ(5, KeySync::press(offering(key::A_MINOR), key::E_MINOR, 0));
}

// Rule 2.
TEST(KeySync, APressAlreadyThereGoesBackToTheOriginal) {
    EXPECT_EQ(0, KeySync::press(offering(key::A_MINOR), key::E_MINOR, 5));
}

// From a key picked off the list, a press still matches the master: only
// being on the master's number makes it go back.
TEST(KeySync, APressFromAPickedKeyMatchesTheMaster) {
    EXPECT_EQ(5, KeySync::press(offering(key::A_MINOR), key::E_MINOR, 2));
}

// The master changed track since the last press: a press takes the new key.
TEST(KeySync, APressTakesWhateverTheMasterIsInNow) {
    // Shifted +5 onto A minor; the master is now in D minor (7A).
    EXPECT_EQ(-2, KeySync::press(offering(key::D_MINOR), key::E_MINOR, 5));
}

// Rule 3: with nothing to match, back to the original, and nothing more.
TEST(KeySync, APressWithNothingToMatchGoesBackToTheOriginal) {
    EXPECT_EQ(0, KeySync::press(noMaster(), key::E_MINOR, 3));
    EXPECT_EQ(0, KeySync::press(noMaster(), key::E_MINOR, 0));
    EXPECT_EQ(0, KeySync::press(offering(key::INVALID), key::E_MINOR, 3));
    // Our own key unknown: no shift lands anywhere in particular.
    EXPECT_EQ(0, KeySync::press(offering(key::A_MINOR), key::INVALID, 3));
    EXPECT_EQ(0, KeySync::press(offering(key::A_MINOR), key::INVALID, 0));
}

// A track whose own key is already on the master's number has nowhere to go.
TEST(KeySync, APressOnTheMastersNumberAlreadyChangesNothing) {
    EXPECT_EQ(0, KeySync::press(offering(key::A_MINOR), key::C_MAJOR, 0));
}
