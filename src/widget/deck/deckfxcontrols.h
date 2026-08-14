#pragma once

#include <memory>

#include "control/controlobject.h"
#include "control/controlencoder.h"
#include "control/controlpushbutton.h"

namespace mixxx {
namespace deck {

/// The controls the effect rack and the deck view's FX strip both need,
/// created **before the skin is parsed**.
///
/// Same reasoning as ProLinkControls, and the same sharp edge:
/// `ControlDoublePrivate::getControl()` returns null to a second creator of the
/// same key, so ownership goes to whoever runs first and the loser silently
/// holds an object wired to nothing. Two widgets in one skin cannot both create
/// these -- and which of them is built first is decided by the order of nodes
/// in skin.xml, which is not a contract anybody would think to keep.
///
/// So they are made here, from CoreServices, and both widgets reach them
/// through proxies.
class DeckFxControls {
  public:
    DeckFxControls();
    ~DeckFxControls();

    static DeckFxControls* instance();

    /// True while the FX strip has claimed the encoder. Read by the controller
    /// mapping to decide where a detent goes: the strip, the browser, or the
    /// waveform zoom. Written only by the strip.
    ControlObject* focus() const {
        return m_pFocus.get();
    }
    /// Encoder detents, while focused. Relative: -1 or +1 per detent.
    ///
    /// A ControlEncoder with nop-ignoring OFF, which is the whole reason it is
    /// not a plain ControlObject: eight detents in the same direction write
    /// the same value eight times, and a control that suppresses no-op writes
    /// emits once. The level moved a single step and stopped.
    ControlEncoder* move() const {
        return m_pMove.get();
    }
    /// Encoder press, while focused. Mutes and unmutes the rack's master.
    ControlPushButton* select() const {
        return m_pSelect.get();
    }
    /// Which end of the chain the master rides: 1 = the send, so tails ring
    /// out; 0 = the return, so they are cut dead. Shared because the rack's
    /// rocker and the strip's rocker are the same switch seen twice, and a
    /// copy in each would drift apart the moment either was touched.
    ControlPushButton* ringOut() const {
        return m_pRingOut.get();
    }

  private:
    std::unique_ptr<ControlObject> m_pFocus;
    std::unique_ptr<ControlEncoder> m_pMove;
    std::unique_ptr<ControlPushButton> m_pSelect;
    std::unique_ptr<ControlPushButton> m_pRingOut;
};

} // namespace deck
} // namespace mixxx
