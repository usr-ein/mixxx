#pragma once

#include <QPainter>
#include <QPolygonF>
#include <QRectF>

namespace mixxx {
namespace deck {

/// The Bluetooth rune (ᛒ over ᚼ), as one stroke, in the middle of *box*: the
/// home list's mark for "Bluetooth receiver", and the cover the page shows
/// when a device sends none. Drawn rather than loaded, as the other marks are.
inline void paintBluetoothRune(QPainter* pPainter, const QRectF& box, const QColor& colour,
        qreal penWidth) {
    const qreal size = qMin(box.width(), box.height());
    const QPointF origin(box.center().x() - size / 2, box.center().y() - size / 2);
    const auto at = [&](qreal x, qreal y) {
        return origin + QPointF(x * size, y * size);
    };
    // From the lower arm's tip, down through the bottom arrow, up the stem,
    // round the top arrow, and out to the upper arm's tip.
    const QPolygonF stroke{at(0.30, 0.31),
            at(0.70, 0.67),
            at(0.50, 0.88),
            at(0.50, 0.12),
            at(0.70, 0.33),
            at(0.30, 0.69)};
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(colour, penWidth);
    pen.setJoinStyle(Qt::MiterJoin);
    pen.setCapStyle(Qt::RoundCap);
    pPainter->setPen(pen);
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawPolyline(stroke);
    pPainter->restore();
}

} // namespace deck
} // namespace mixxx
