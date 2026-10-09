#include "network/prolink/prolinkkeysync.h"

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
    // shift there stored "steps minus the fader's part", and releasing by
    // writing 0 there left a hidden offset that cancelled the fader's part --
    // a deck a semitone flat once the fader came back to centre, with KEY SYNC
    // dark. The shift is an offset on top of whatever the fader does (owner
    // decision 8), which is exactly what `pitch_adjust` is.
    m_pDeckPitchAdjust = deck("pitch_adjust");
    m_pDeckFileKey = deck("file_key");
    m_pDeckKeylock = deck("keylock");

    connect(m_pControls->keySyncEnabled(),
            &ControlPushButton::valueChanged,
            this,
            &ProLinkKeySync::onEnabledChanged);

    // **Resume, don't forget** (rule 3). The controls are made before this
    // object and outlive it, so a latch they already hold is picked up.
    const auto held = KeyUtils::keyFromNumericValue(m_pControls->keySyncTarget()->get());
    if (m_pControls->keySyncEnabled()->get() > 0.0 &&
            held != mixxx::track::io::key::INVALID) {
        m_state.resume(held);
        applyToDeck();
    } else if (m_pControls->keySyncEnabled()->get() > 0.0) {
        // Lit with nothing held: a state nothing can explain. Put the button
        // back rather than leave a lit KEY SYNC that does nothing.
        m_pControls->keySyncEnabled()->set(0.0);
    }

    // **After the load, not during it.** `file_key` is set inside
    // BaseTrackPlayer's load handler, and the same handler then resets the
    // pitch (SpeedAutoReset): re-applying when the key changed was undone a few
    // lines later, and a track in the same key as the last never changed it at
    // all. PlayerInfo announces the new track after those resets, on this
    // thread, for every load.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            [this](const QString& group, TrackPointer pNewTrack, TrackPointer) {
                if (group == m_deckGroup && pNewTrack) {
                    reapply();
                }
            });
    // A key that arrives after the load -- analysed on the deck, or read late.
    // Queued, so a change made inside the load handler is seen after its
    // resets too.
    m_pDeckFileKey->connectValueChanged(
            this, &ProLinkKeySync::onFileKeyChanged, Qt::QueuedConnection);
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
                if (steps != kNoShift) {
                    m_pDeckPitchAdjust->set(steps);
                }
            },
            Qt::DirectConnection);
}

ProLinkKeySync::~ProLinkKeySync() = default;

void ProLinkKeySync::setLink(bool otherIsMaster,
        mixxx::track::io::key::ChromaticKey masterKey) {
    m_link.otherIsMaster = otherIsMaster;
    m_link.masterKey = masterKey;
    if (m_pControls) {
        m_pControls->keySyncAvailable()->forceSet(KeySync::canEngage(m_link) ? 1.0 : 0.0);
    }
    // And nothing else. Everything about an engaged sync -- whether it stays
    // engaged, and in which key -- was settled when it was engaged.
}

void ProLinkKeySync::onEnabledChanged(double value) {
    const bool wanted = value > 0.0;
    if (wanted == m_state.engaged()) {
        return;
    }
    if (!wanted) {
        m_state.release();
        m_heldSteps.store(kNoShift);
        if (m_pControls) {
            m_pControls->keySyncTarget()->forceSet(0.0);
        }
        // No shift: the key is the track's own, plus whatever the fader does
        // with keylock off. Not whatever pitch_adjust was before the sync:
        // nothing else on this deck moves it.
        if (m_pDeckPitchAdjust) {
            m_pDeckPitchAdjust->set(0.0);
        }
        return;
    }
    if (!m_state.engage(m_link)) {
        // Nothing on the network to sync to. Put the button back rather than
        // leaving it lit against a key we do not have: a lit KEY SYNC that has
        // not moved the deck is a lie the DJ finds out about in the mix.
        //
        // Safe to write from inside this handler -- it comes straight back
        // with 0, which matches the state we are already in and returns above.
        if (m_pControls) {
            m_pControls->keySyncEnabled()->set(0.0);
        }
        return;
    }
    if (m_pControls) {
        m_pControls->keySyncTarget()->forceSet(static_cast<double>(m_state.target()));
    }
    // Owner decision 12: engaging turns master tempo on. A latched key is a
    // key; with keylock off the deck's key would move with every tempo change
    // from the moment it was set.
    if (m_pDeckKeylock && m_pDeckKeylock->get() <= 0.0) {
        m_pDeckKeylock->set(1.0);
    }
    applyToDeck();
    kLogger.debug() << "engaged on"
                    << KeyUtils::keyToString(m_state.target(), KeyUtils::KeyNotation::Lancelot);
}

void ProLinkKeySync::onFileKeyChanged(double value) {
    Q_UNUSED(value);
    reapply();
}

void ProLinkKeySync::reapply() {
    if (m_state.engaged()) {
        applyToDeck();
    }
}

void ProLinkKeySync::applyToDeck() {
    if (!m_pDeckPitchAdjust || !m_pDeckFileKey) {
        return;
    }
    const auto fileKey = KeyUtils::keyFromNumericValue(m_pDeckFileKey->get());
    if (fileKey == mixxx::track::io::key::INVALID) {
        // No track, or one nobody ever worked out a key for. The latch stands:
        // the next track that does have a key is pitched into it. Nothing is
        // held meanwhile, so a keylock toggle puts no stale shift back.
        m_heldSteps.store(kNoShift);
        return;
    }
    // Compatible rather than identical, exactly as Mixxx's own `sync_key`
    // does. It lands on the tonic, the fourth or the fifth -- keys that mix
    // with the master's -- and so never asks for more than two semitones.
    // Matching the master's tonic literally is a shift of up to six, which is
    // where a track starts sounding like a chipmunk.
    //
    // There is no cents term here, unlike KeyControl::syncKey: the target is
    // the master's track key as rekordbox analysed it (owner decision 7), and
    // nothing on the wire says how that CDJ is detuned, so there is nothing
    // finer than a semitone to aim at.
    const int steps = KeyUtils::shortestStepsToCompatibleKey(fileKey, m_state.target());
    m_heldSteps.store(steps);
    m_pDeckPitchAdjust->set(steps);
}

} // namespace prolink
} // namespace mixxx
