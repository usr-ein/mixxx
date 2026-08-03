#pragma once

#include <QList>
#include <QTimer>
#include <QWidget>

#include "library/deck/mediaregistry.h"
#include "skin/legacy/skincontext.h"
#include "widget/wbasewidget.h"

namespace mixxx {
namespace deck {

/// A media event, said out loud in the corner (browser-prd.md 13).
///
/// Lives at the top of the skin's stack rather than inside the browser, because
/// a stick landing while you are mixing is exactly when you need to know, and
/// at that moment the browser is not on screen.
///
/// **It must never take input.** The container spans the whole panel so it can
/// place itself in a stacked layout that centres its children, and is therefore
/// transparent to the mouse — otherwise it would swallow every tap on the deck.
/// The individual toasts are real widgets on top of it, so they can still be
/// touched to dismiss.
class WDeckToast : public QWidget, public WBaseWidget {
    Q_OBJECT

  public:
    explicit WDeckToast(QWidget* pParent);

    void setup(const QDomNode& node, const SkinContext& context);

  private slots:
    void onAppeared(mixxx::deck::MediumInfo medium);
    void onVanished(mixxx::deck::MediumInfo medium);
    void onFailed(mixxx::deck::MediumInfo medium);

  private:
    /// One line, or two when it has something to explain.
    void show(const MediumInfo& medium, const QString& text, bool wide);
    void reposition();
    void expire();

    struct Toast {
        QWidget* pWidget = nullptr;
        qint64 expiresAt = 0;
    };
    QList<Toast> m_toasts;
    QTimer m_tick;
};

} // namespace deck
} // namespace mixxx
