#pragma once

#include <QColor>
#include <QWidget>

#include "skin/legacy/skincontext.h"
#include "widget/wbasewidget.h"

namespace mixxx {
namespace deck {

/// "AUTOPLAY" and its genre, over the waveform, while autoplay is on.
///
/// In the bottom-left corner, as the tempo box is in the bottom-right and in
/// its style, so the deck view says at a glance that the next track will come
/// by itself, and from which genre. Stopping it is the browser's: Autoplay, then
/// Stop autoplay.
///
/// Painted rather than laid out, for the tempo box's reason: it sits in the
/// waveform's stacked layout, which centres a widget at its size hint instead of
/// placing it. So it covers the waveform, draws only its corner, and takes no
/// input at all -- the waveform under it keeps every touch.
class WDeckAutoplayBadge : public QWidget, public WBaseWidget {
    Q_OBJECT

  public:
    explicit WDeckAutoplayBadge(QWidget* pParent);

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    /// Above the waveform: a stacked layout raises only its current child,
    /// which is not this one (see WTempoPanel::showEvent()).
    void showEvent(QShowEvent* pEvent) override;

  private:
    /// This deck's accent (deckaccent.h): autoplay being on is something live.
    const QColor m_accent;
};

} // namespace deck
} // namespace mixxx
