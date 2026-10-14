#include "network/prolink/prolinkkeysync.h"

#include <cmath>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "mixer/playerinfo.h"
#include "moc_prolinkkeysync.cpp"
#include "network/prolink/prolinkcontrols.h"
#include "track/keyutils.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("ProLinkKeySync");

} // namespace

namespace mixxx {
namespace prolink {

ProLinkKeySync::ProLinkKeySync(const QString& deckGroup,
        ProLinkControls* pControls,
        QObject* pParent)
        : QObject(pParent),
          m_deckGroup(deckGroup),
          m_pControls(pControls) {
    VERIFY_OR_DEBUG_ASSERT(m_pControls) {
        kLogger.warning() << "the [ProLink] controls were never created;"
                          << "KEY SYNC will do nothing";
        return;
    }

    const auto deck = [this](const char* item) {
        return std::make_unique<ControlProxy>(m_deckGroup,
                QString::fromLatin1(item),
                this,
                ControlFlag::NoWarnIfMissing);
    };
    // `pitch_adjust`, the offset, and not `pitch`, the total. With keylock off
    // `pitch` includes what the tempo fader does to the key, so writing a
    // shift there stored "steps minus the fader's part", and going back by
    // writing 0 there left a hidden offset that cancelled the fader's part --
    // a deck a semitone flat once the fader came back to centre, with KEY SYNC
    // dark. The shift is an offset on top of whatever the fader does (owner
    // decision 8), which is exactly what `pitch_adjust` is.
    m_pDeckPitchAdjust = deck("pitch_adjust");
    m_pDeckFileKey = deck("file_key");
    m_pDeckKeylock = deck("keylock");

    connect(m_pControls->keySync(),
            &ControlPushButton::valueChanged,
            this,
            &ProLinkKeySync::onPressed);
    connect(m_pControls->keyShift(),
            &ControlObject::valueChanged,
            this,
            &ProLinkKeySync::onShiftRequested);

    // A shift the controls already hold, from an earlier instance, is held
    // again: they are made before this object and outlive it, and a keylock
    // toggle must still put the shift back.
    if (shift() != 0) {
        applyShift(shift(), "resumed");
    }

    // **A new track plays in its own key** (Sam, 2026-10-14), and so does an
    // empty deck. After the load, not during it: PlayerInfo announces the new
    // track once BaseTrackPlayer's load handler is done with the deck, on this
    // thread, for every load.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            [this](const QString& group, TrackPointer, TrackPointer) {
                if (group == m_deckGroup) {
                    applyShift(0, "new track");
                }
            });
    // The track's key decides whether a press would do anything, and it can
    // arrive after the load: analysed on the deck, or read late.
    m_pDeckFileKey->connectValueChanged(
            this,
            [this](double) {
                publishAvailable();
            },
            Qt::QueuedConnection);
    // **Keylock wipes the shift both ways** at this deck's settings
    // (keylockMode 0, keyunlockMode 0): KeyControl zeroes pitch_adjust when it
    // is turned on and when it is turned off. The shift is put back straight
    // after, on whichever thread toggled it, so not a buffer is played without
    // it. KeyControl connected first, at the deck's construction, so this runs
    // after its reset.
    m_pDeckKeylock->connectValueChanged(
            this,
            [this](double) {
                const int steps = m_heldSteps.load();
                if (steps != 0) {
                    m_pDeckPitchAdjust->set(steps);
                }
            },
            Qt::DirectConnection);
    publishAvailable();
}

ProLinkKeySync::~ProLinkKeySync() = default;

void ProLinkKeySync::setLink(bool otherIsMaster,
        mixxx::track::io::key::ChromaticKey masterKey) {
    m_link.otherIsMaster = otherIsMaster;
    m_link.masterKey = masterKey;
    publishAvailable();
    // And nothing else. The network is read when the pad is pressed, and only
    // then (KeySync rule 4).
}

void ProLinkKeySync::onPressed(double value) {
    if (value <= 0.0) {
        return;
    }
    const int wanted = KeySync::press(m_link, fileKey(), shift());
    if (wanted == shift()) {
        // Nothing to match and nothing to go back from. Keylock is left as it
        // is: a press that changed nothing must not change that either.
        return;
    }
    applyShift(wanted, "pressed");
}

void ProLinkKeySync::onShiftRequested(double value) {
    applyShift(static_cast<int>(std::lround(value)), "picked");
}

void ProLinkKeySync::applyShift(int requested, const char* cause) {
    if (!m_pControls || !m_pDeckPitchAdjust) {
        return;
    }
    const int steps = KeySync::foldShift(requested);
    const int was = m_heldSteps.load();
    if (steps != was) {
        // Said once per change: what the deck plays in is worth finding in a
        // log after a set, and it changes a few times an hour at most. The key
        // is named for a shift only -- 0 is the track's own key, and at a load
        // `file_key` can still be the last track's -- and only when known.
        const QString key = steps != 0
                ? KeyUtils::keyToString(KeySync::keyAtShift(fileKey(), steps),
                          KeyUtils::KeyNotation::Lancelot)
                : QString();
        kLogger.info() << cause << "- shift" << was << "->" << steps
                       << qUtf8Printable(key);
    }
    // The owner's set: heard by everything watching it except this object.
    m_pControls->keyShift()->set(steps);
    // Owner decision 12: a shift turns master tempo on. A key chosen is a key;
    // with keylock off the deck's key would move with every tempo change from
    // the moment it was set. KeyControl zeroes pitch_adjust as keylock turns
    // on, so it is set after.
    if (steps != 0 && m_pDeckKeylock && m_pDeckKeylock->get() <= 0.0) {
        m_pDeckKeylock->set(1.0);
    }
    m_heldSteps.store(steps);
    // No cents term, unlike KeyControl::syncKey: the master's key is its track
    // key as rekordbox analysed it (owner decision 7), and nothing on the wire
    // says how that CDJ is detuned, so there is nothing finer than a semitone
    // to aim at.
    m_pDeckPitchAdjust->set(steps);
    publishAvailable();
}

void ProLinkKeySync::publishAvailable() {
    if (!m_pControls) {
        return;
    }
    const bool wouldMove = KeySync::press(m_link, fileKey(), shift()) != shift();
    m_pControls->keySyncAvailable()->forceSet(wouldMove ? 1.0 : 0.0);
}

int ProLinkKeySync::shift() const {
    return m_pControls ? static_cast<int>(std::lround(m_pControls->keyShift()->get())) : 0;
}

mixxx::track::io::key::ChromaticKey ProLinkKeySync::fileKey() const {
    return m_pDeckFileKey ? KeyUtils::keyFromNumericValue(m_pDeckFileKey->get())
                          : mixxx::track::io::key::INVALID;
}

} // namespace prolink
} // namespace mixxx
