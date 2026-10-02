#pragma once

#include <QList>
#include <QTimer>
#include <QWidget>

#include "library/deck/mediaregistry.h"
#include "skin/legacy/skincontext.h"
#include "track/track_decl.h"
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

  public slots:
    /// A track could not be loaded. Said here, in the corner, instead of the
    /// modal dialog Mixxx raises -- which on a deck with no mouse blocks the
    /// whole interface until somebody finds the button with a finger.
    void onLoadFailed(TrackPointer pTrack, const QString& reason);

  private slots:
    void onAppeared(mixxx::deck::MediumInfo medium);
    void onVanished(mixxx::deck::MediumInfo medium);
    void onFailed(mixxx::deck::MediumInfo medium);
    void onNotice(mixxx::deck::MediumInfo medium, const QString& text);

  private:
    /// One line, or two when it has something to explain. A toast with a
    /// *key* -- a medium's -- replaces one still up with the same key.
    void show(const QString& text, bool wide, const QString& key = QString());
    void reposition();
    void expire();

    struct Toast {
        QWidget* pWidget = nullptr;
        qint64 expiresAt = 0;
        QString text;
        QString key;
    };
    QList<Toast> m_toasts;
    QTimer m_tick;
};

} // namespace deck
} // namespace mixxx
