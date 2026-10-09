#pragma once

#include <QObject>
#include <QPointer>

class QEvent;
class QMenuBar;
class QMouseEvent;
class QWidget;

namespace mixxx {
namespace deck {

/// The main window's menu bar, hidden, and shown only while a real mouse
/// hovers the top edge.
///
/// This deck has a touchscreen, no pointer and no keyboard. The bar is
/// unreachable in normal use and costs ~22 px off a panel whose layout is
/// budgeted to the pixel -- but deleting it would take the preferences dialog
/// and the developer tools with it, which are exactly what you want on the one
/// day you have a keyboard plugged in.
///
/// So: reveal it on HOVER in a strip along the top edge. Hover is the
/// discriminator that matters, and it is not a heuristic -- a touchscreen can
/// only press, never hover, so a fingertip cannot trigger this however X11
/// chooses to synthesise its events. Checking QMouseEvent::source() for
/// synthesis would be the obvious alternative and is strictly weaker: some
/// panels present as plain pointer devices and defeat it.
///
/// It overlays rather than reflows. The skin is a fixed 1024x600; giving the
/// bar its own row would push the last one off the bottom.
class MenuBarReveal : public QObject {
    Q_OBJECT

  public:
    /// Lays *pMenuBar* over the top of *pCentralWidget*, hidden, and watches
    /// the application's mouse moves from then on. *pParent*, the window,
    /// owns this.
    MenuBarReveal(QMenuBar* pMenuBar, QWidget* pCentralWidget, QObject* pParent);

  protected:
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    /// Show the menu bar while a real pointer hovers the top edge, hide it
    /// when it leaves.
    void update(QMouseEvent* pEvent);

    /// Guarded: the window deletes both before it deletes this.
    QPointer<QMenuBar> m_pMenuBar;
    QPointer<QWidget> m_pCentralWidget;
};

} // namespace deck
} // namespace mixxx
