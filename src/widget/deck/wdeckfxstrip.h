#pragma once

#include <QTimer>
#include <QWidget>
#include <memory>

#include "skin/legacy/skincontext.h"
#include "widget/deck/deckencoder.h"
#include "widget/wbasewidget.h"

class ControlProxy;

namespace mixxx {
namespace deck {

/// The FX strip: the rack's master, down the left of the waveform.
///
/// Reaching the effects should not mean leaving the waveform (PRD §16). A DJ
/// riding a filter through a transition is watching the track, not a menu three
/// levels into the browser -- so the one control that matters mid-transition,
/// the master, is here as well as on the Effects page.
///
/// It shows what the rack is returning, what the master is set to, and which
/// end of the chain that master is riding. It does not duplicate the modules:
/// the strip is 76 px wide and a rack of six is not going to fit in it.
///
/// **Touching it claims the encoder.** The border lights while it holds focus,
/// so there is never a question of what the wheel is about to do; touching
/// anything else gives it back. See eventFilter().
class WDeckFxStrip : public QWidget, public WBaseWidget, public DeckEncoder::Target {
    Q_OBJECT

  public:
    explicit WDeckFxStrip(QWidget* pParent = nullptr);
    ~WDeckFxStrip() override;

    void setup(const QDomNode& node, const SkinContext& context);

    // DeckEncoder::Target. Rotation only arrives while this has focus -- the
    // router sends it to the waveform zoom otherwise -- but the press always
    // arrives, because on the deck view it is the FX mute either way.
    void encoderMove(int steps) override;
    void encoderPress() override;
    void encoderResetFocus() override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    /// Watches the whole window for presses, so a touch anywhere else releases
    /// focus. There is no timeout: a wheel that silently went back to the
    /// library after five seconds would be worse than one that stayed where it
    /// was put, because the moment it matters is mid-transition and that is
    /// exactly when nobody is looking at it. Releasing on the next touch
    /// elsewhere is the same rule the browser's pages already follow.
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    /// The gain the master is riding: the send in ring-out, the return in cut.
    ControlProxy* masterControl() const;
    ControlProxy* idleControl() const;
    bool ringOut() const;
    void setRingOut(bool ringOut);
    void setFocused(bool focused);

    QRect knobRect() const;
    QRect vuRect() const;
    QRect rockerRect() const;

    std::unique_ptr<ControlProxy> m_pOutputLevel;
    std::unique_ptr<ControlProxy> m_pAuxPregain;
    std::unique_ptr<ControlProxy> m_pMakeup;
    std::unique_ptr<ControlProxy> m_pRingOut;

    QTimer m_refresh;
    bool m_focused = false;
    /// >= 0 while muted: the level to come back to. Same rule as the rack's,
    /// and deliberately the same member name.
    double m_mutedLevel = -1.0;
    double m_dragStartValue = 0.0;
    QPoint m_dragStart;
    bool m_draggingKnob = false;
};

} // namespace deck
} // namespace mixxx
