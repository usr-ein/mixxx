#include "network/prolink/prolinkkeysync.h"

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "moc_prolinkkeysync.cpp"
#include "network/prolink/prolinkcontrols.h"
#include "track/keyutils.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("ProLinkKeySync");

/// The deck the network and the browser both mean by "this deck".
const char* kDeckGroup = "[Channel1]";

} // namespace

namespace mixxx {
namespace prolink {

ProLinkKeySync::ProLinkKeySync(QObject* pParent)
        : QObject(pParent), m_pControls(ProLinkControls::instance()) {
    VERIFY_OR_DEBUG_ASSERT(m_pControls) {
        kLogger.warning() << "the [ProLink] controls were never created;"
                          << "KEY SYNC will do nothing";
        return;
    }

    const auto deck = [this](const char* item) {
        return std::make_unique<ControlProxy>(QString::fromLatin1(kDeckGroup),
                QString::fromLatin1(item),
                this,
                ControlFlag::NoWarnIfMissing);
    };
    // `pitch` and not `rate`: with keylock on -- which this deck boots into --
    // it is the one that moves the key and leaves the tempo alone. It is also
    // what Mixxx's own `sync_key` writes, so the two cannot disagree about
    // what a key shift is.
    m_pDeckPitch = deck("pitch");
    m_pDeckFileKey = deck("file_key");

    connect(m_pControls->keySyncEnabled(),
            &ControlPushButton::valueChanged,
            this,
            &ProLinkKeySync::onEnabledChanged);

    // The track's own key, which is also how a load reaches us: it is set from
    // the Track the moment the deck takes one, and zeroed when the deck is
    // ejected.
    m_pDeckFileKey->connectValueChanged(this, &ProLinkKeySync::onFileKeyChanged);
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
        // Back to the track's own key. Not to whatever the pitch was before
        // the sync: `pitch` is zero unless something deliberately moved it,
        // and restoring a remembered value would mean guessing which of the
        // two -- the DJ or us -- moved it last.
        if (m_pDeckPitch) {
            m_pDeckPitch->set(0.0);
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
    applyToDeck();
    kLogger.debug() << "engaged on"
                    << KeyUtils::keyToString(m_state.target(), KeyUtils::KeyNotation::Lancelot);
}

void ProLinkKeySync::onFileKeyChanged(double value) {
    Q_UNUSED(value);
    if (m_state.engaged()) {
        applyToDeck();
    }
}

void ProLinkKeySync::applyToDeck() {
    if (!m_pDeckPitch || !m_pDeckFileKey) {
        return;
    }
    const auto fileKey = KeyUtils::keyFromNumericValue(m_pDeckFileKey->get());
    if (fileKey == mixxx::track::io::key::INVALID) {
        // No track, or one nobody ever worked out a key for. The latch stands:
        // the next track that does have a key is pitched into it.
        return;
    }
    // Compatible rather than identical, exactly as Mixxx's own `sync_key`
    // does. It lands on the tonic, the fourth or the fifth -- keys that mix
    // with the master's -- and so never asks for more than two semitones.
    // Matching the master's tonic literally is a shift of up to six, which is
    // where a track starts sounding like a chipmunk.
    //
    // There is no cents term here, unlike KeyControl::syncKey: a CDJ publishes
    // a key and not a detuning, so there is nothing finer than a semitone to
    // aim at.
    const int steps = KeyUtils::shortestStepsToCompatibleKey(fileKey, m_state.target());
    m_pDeckPitch->set(steps);
}

} // namespace prolink
} // namespace mixxx
