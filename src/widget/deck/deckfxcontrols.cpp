#include "widget/deck/deckfxcontrols.h"

namespace {
mixxx::deck::DeckFxControls* s_pInstance = nullptr;
const QString kGroup = QStringLiteral("[TriMixxx]");
} // namespace

namespace mixxx {
namespace deck {

DeckFxControls::DeckFxControls() {
    m_pFocus = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("fx_focus")));
    // Read-only from outside: the strip is the only thing that knows whether it
    // has been touched, and a mapping that could set this would be able to
    // steer the encoder somewhere nothing is lit.
    m_pFocus->setReadOnly();

    m_pMove = std::make_unique<ControlEncoder>(
            ConfigKey(kGroup, QStringLiteral("fx_move")), false);
    m_pSelect = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("fx_select")));

    // Persisted, so the deck comes up on the mode it was left in. Default ring
    // out: it is the behaviour that cannot be got any other way.
    m_pRingOut = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("fx_ring_out")), true);
    m_pRingOut->setButtonMode(ControlPushButton::TOGGLE);
    m_pRingOut->setStates(2);
    m_pRingOut->setDefaultValue(1.0);

    s_pInstance = this;
}

DeckFxControls::~DeckFxControls() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

// static
DeckFxControls* DeckFxControls::instance() {
    return s_pInstance;
}

} // namespace deck
} // namespace mixxx
