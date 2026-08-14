#pragma once

#include <QObject>
#include <memory>

#include "control/controlencoder.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"

namespace mixxx {
namespace deck {

/// What the deck's one encoder means, decided in one place.
///
/// There is a single physical encoder — turn and press — and it has to do a
/// different job on each screen. That routing used to live in three places at
/// once: conditionals in the controller script, controls owned by the browser,
/// and a focus flag owned by the FX strip. Each was correct on its own and the
/// combination was not: focus could survive a screen change, so the browser's
/// encoder did nothing until something else happened to clear it.
///
/// So the rules live here, in full, and nowhere else:
///
/// | Screen | Rotate | Press |
/// |---|---|---|
/// | Waveform | zoom the waveform | mute/unmute the FX master |
/// | Waveform, FX strip focused | the FX master level | mute/unmute the FX master |
/// | Browser, in a menu | move the selection | enter it |
/// | Browser, in a track list | move the selection | load the track |
/// | Effects page | the FX master level | mute/unmute the FX master |
/// | Effects page, a knob touched | that knob | mute/unmute the FX master |
///
/// Two invariants hold that table together:
///
/// - **The press never changes meaning within a screen.** On the deck view it
///   is the FX mute whether or not the strip has focus; on the Effects page it
///   is the master's mute whatever the rotation is pointed at. It is the panic
///   button and it does not move.
/// - **Changing screen resets focus.** Every target is told, so no screen can
///   inherit a claim made on another one. That is the bug this class exists to
///   make impossible rather than to keep fixing.
///
/// The controller script forwards a detent and a press and knows nothing else.
///
/// Created from CoreServices **before any skin is parsed**, for the reason
/// ProLinkControls documents at length: a second creator of a control key gets
/// an object wired to nothing, and LegacySkinParser creates any key it does not
/// find.
class DeckEncoder : public QObject {
    Q_OBJECT

  public:
    /// A screen that can take the encoder.
    class Target {
      public:
        virtual ~Target() = default;
        virtual void encoderMove(int steps) = 0;
        virtual void encoderPress() = 0;
        /// The screen changed. Drop whatever this target had focused, so a
        /// claim made on one screen cannot outlive it.
        virtual void encoderResetFocus() {
        }
    };

    DeckEncoder();
    ~DeckEncoder() override;

    static DeckEncoder* instance();

    /// Registered by the widgets that own each screen. Passing nullptr
    /// deregisters, which their destructors do.
    void setDeckView(Target* pTarget);
    void setBrowser(Target* pTarget);

    /// The FX strip's claim on the deck view's ROTATION only; see the table.
    void setFxFocused(bool focused);
    bool fxFocused() const {
        return m_fxFocused;
    }

    /// Which end of the chain the rack's master rides. Shared by the Effects
    /// page and the FX strip, which show the same switch twice.
    ControlPushButton* ringOut() const {
        return m_pRingOut.get();
    }

  private:
    void onMove(double steps);
    void onPress(double v);
    void onScreenChanged(double v);
    /// Bind the proxies that cannot exist at construction. Idempotent.
    void bindLate();
    bool libraryShown() const;

    std::unique_ptr<ControlEncoder> m_pMove;
    std::unique_ptr<ControlPushButton> m_pPress;
    std::unique_ptr<ControlPushButton> m_pRingOut;
    /// Read-only outside: a mapping that could set this would be able to point
    /// the encoder at something with nothing lit to say so.
    std::unique_ptr<ControlObject> m_pFxFocus;

    std::unique_ptr<ControlProxy> m_pShowLibrary;
    std::unique_ptr<ControlProxy> m_pZoomUp;
    std::unique_ptr<ControlProxy> m_pZoomDown;

    Target* m_pDeckView = nullptr;
    Target* m_pBrowser = nullptr;
    bool m_fxFocused = false;
};

} // namespace deck
} // namespace mixxx
