#include "widget/deck/deckencoder.h"

#include "moc_deckencoder.cpp"

namespace {
mixxx::deck::DeckEncoder* s_pInstance = nullptr;
const QString kGroup = QStringLiteral("[TriMixxx]");
} // namespace

namespace mixxx {
namespace deck {

DeckEncoder::DeckEncoder() {
    // Nop-ignoring OFF. Eight detents in the same direction write the same
    // value eight times, and a control that suppresses no-op writes emits once
    // -- which reads as an encoder that moves one step and sticks.
    m_pMove = std::make_unique<ControlEncoder>(
            ConfigKey(kGroup, QStringLiteral("encoder_move")), false);
    connect(m_pMove.get(), &ControlEncoder::valueChanged, this, &DeckEncoder::onMove);

    m_pPress = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("encoder_press")));
    connect(m_pPress.get(), &ControlPushButton::valueChanged, this, &DeckEncoder::onPress);

    m_pFxFocus = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("fx_focus")));
    m_pFxFocus->setReadOnly();

    // Persisted so the deck comes up on the mode it was left in. The default
    // goes in the constructor rather than setDefaultValue(), which only sets
    // what a reset returns to and leaves the current value at zero.
    m_pRingOut = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("fx_ring_out")), true, 1.0);
    m_pRingOut->setButtonMode(ControlPushButton::TOGGLE);
    m_pRingOut->setStates(2);

    // The three proxies below are bound LATE, on first use, and that is the
    // whole reason this class is careful about timing in two directions at
    // once. Its own controls have to exist before the skin, or LegacySkinParser
    // creates them first and this one gets objects wired to nothing. But
    // `[Master] show_library` is a SKIN control, and the deck's zoom controls
    // belong to a deck PlayerManager has not made yet -- so binding those here
    // gets proxies that are permanently invalid.
    //
    // Not hypothetical: show_library bound at construction read false forever,
    // so the router believed the library was never up and sent every detent to
    // the waveform zoom. The encoder did nothing at all in the browser.

    s_pInstance = this;
}

DeckEncoder::~DeckEncoder() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

// static
DeckEncoder* DeckEncoder::instance() {
    return s_pInstance;
}

void DeckEncoder::setDeckView(Target* pTarget) {
    m_pDeckView = pTarget;
    // Registration happens while the skin is being parsed, which is exactly
    // when the controls above come into existence.
    if (pTarget) {
        bindLate();
    }
}

void DeckEncoder::setBrowser(Target* pTarget) {
    m_pBrowser = pTarget;
    if (pTarget) {
        bindLate();
    }
}

void DeckEncoder::bindLate() {
    if (!m_pShowLibrary || !m_pShowLibrary->valid()) {
        m_pShowLibrary = std::make_unique<ControlProxy>(
                QStringLiteral("[Master]"), QStringLiteral("show_library"));
        if (m_pShowLibrary->valid()) {
            m_pShowLibrary->connectValueChanged(this, &DeckEncoder::onScreenChanged);
        }
    }
    if (!m_pZoomUp || !m_pZoomUp->valid()) {
        m_pZoomUp = std::make_unique<ControlProxy>(
                QStringLiteral("[Channel1]"), QStringLiteral("waveform_zoom_up"));
    }
    if (!m_pZoomDown || !m_pZoomDown->valid()) {
        m_pZoomDown = std::make_unique<ControlProxy>(
                QStringLiteral("[Channel1]"), QStringLiteral("waveform_zoom_down"));
    }
}

bool DeckEncoder::libraryShown() const {
    return m_pShowLibrary && m_pShowLibrary->valid() && m_pShowLibrary->toBool();
}

void DeckEncoder::setFxFocused(bool focused) {
    if (m_fxFocused == focused) {
        return;
    }
    m_fxFocused = focused;
    // forceSet: the control is read-only, and setReadOnly() enforces that with
    // a change-request handler that drops the value -- which set() goes
    // through. Written any other way this would silently do nothing.
    m_pFxFocus->forceSet(focused ? 1.0 : 0.0);
}

void DeckEncoder::onScreenChanged(double v) {
    Q_UNUSED(v);
    // No screen inherits a claim made on another one. This is the whole reason
    // the class exists: focus used to survive the change, and the browser's
    // encoder then did nothing until something else happened to clear it.
    setFxFocused(false);
    if (m_pDeckView) {
        m_pDeckView->encoderResetFocus();
    }
    if (m_pBrowser) {
        m_pBrowser->encoderResetFocus();
    }
}

void DeckEncoder::onMove(double steps) {
    bindLate();
    const int n = static_cast<int>(steps);
    if (n == 0) {
        return;
    }
    if (libraryShown()) {
        if (m_pBrowser) {
            m_pBrowser->encoderMove(n);
        }
        return;
    }
    if (m_fxFocused && m_pDeckView) {
        m_pDeckView->encoderMove(n);
        return;
    }
    // The deck view's default. Zoom lives here rather than in the FX strip
    // because it is what the SCREEN does with the encoder, and the strip is
    // only one thing on it.
    ControlProxy* pZoom = n > 0 ? m_pZoomUp.get() : m_pZoomDown.get();
    if (pZoom && pZoom->valid()) {
        for (int i = 0; i < qAbs(n); ++i) {
            pZoom->set(1.0);
        }
    }
}

void DeckEncoder::onPress(double v) {
    if (v <= 0.0) {
        return;
    }
    bindLate();
    if (libraryShown()) {
        if (m_pBrowser) {
            m_pBrowser->encoderPress();
        }
        return;
    }
    // Deck view: the FX mute, focused or not. The press does not change
    // meaning within a screen.
    if (m_pDeckView) {
        m_pDeckView->encoderPress();
    }
}

} // namespace deck
} // namespace mixxx
