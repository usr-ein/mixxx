#include "library/prolink/dlgprolinkfetch.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include "moc_dlgprolinkfetch.cpp"

namespace {
constexpr int kDialogSize = 220;
constexpr int kRingMargin = 46;
constexpr int kRingThickness = 10;
/// Matches the sidebar icon and the other ic_library_* artwork.
const QColor kRingColour(0xff, 0x66, 0x00);
const QColor kTrackColour(0x44, 0x44, 0x44);
/// A degree is 1/16th of the unit QPainter angles use.
constexpr int kSixteenths = 16;
} // namespace

namespace mixxx {

DlgProLinkFetch::DlgProLinkFetch(const QString& trackName, QWidget* pParent)
        : QDialog(pParent,
                  // Frameless: a title bar with a close button would offer a
                  // cancel this dialog cannot honour.
                  Qt::Dialog | Qt::FramelessWindowHint),
          m_trackName(trackName) {
    setFixedSize(kDialogSize, kDialogSize);
    setModal(true);

    // Drives the indeterminate sweep, and repaints the determinate ring often
    // enough that it moves smoothly between the byte updates, which arrive a
    // window at a time rather than continuously.
    auto* pTimer = new QTimer(this);
    connect(pTimer, &QTimer::timeout, this, [this]() {
        m_spin = (m_spin + 12) % 360;
        update();
    });
    pTimer->start(40);
}

void DlgProLinkFetch::setProgress(double fraction) {
    m_indeterminate = false;
    m_fraction = qBound(0.0, fraction, 1.0);
    update();
}

void DlgProLinkFetch::setIndeterminate() {
    m_indeterminate = true;
    update();
}

void DlgProLinkFetch::keyPressEvent(QKeyEvent* pEvent) {
    // Escape would otherwise reject() the dialog and return control to a caller
    // that is still inside a nested event loop waiting on the transfer.
    if (pEvent->key() == Qt::Key_Escape) {
        pEvent->accept();
        return;
    }
    QDialog::keyPressEvent(pEvent);
}

void DlgProLinkFetch::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));

    const QRectF ring = QRectF(rect()).adjusted(
            kRingMargin, kRingMargin, -kRingMargin, -kRingMargin);

    QPen backing(kTrackColour, kRingThickness);
    backing.setCapStyle(Qt::FlatCap);
    painter.setPen(backing);
    painter.drawEllipse(ring);

    QPen arc(kRingColour, kRingThickness);
    arc.setCapStyle(Qt::RoundCap);
    painter.setPen(arc);
    if (m_indeterminate) {
        // A 90-degree sweep chasing its tail. Qt measures from 3 o'clock and
        // counter-clockwise, hence the negation.
        painter.drawArc(ring, -m_spin * kSixteenths, -90 * kSixteenths);
    } else {
        // Determinate: start at 12 o'clock and fill clockwise, which is what a
        // ring is read as.
        painter.drawArc(ring,
                90 * kSixteenths,
                -static_cast<int>(m_fraction * 360.0) * kSixteenths);
    }

    painter.setPen(QColor(0xee, 0xee, 0xee));
    if (!m_indeterminate) {
        QFont percentFont = font();
        percentFont.setPointSize(20);
        percentFont.setBold(true);
        painter.setFont(percentFont);
        painter.drawText(ring,
                Qt::AlignCenter,
                QStringLiteral("%1%").arg(qRound(m_fraction * 100.0)));
    }

    // The track name, elided rather than wrapped: a rekordbox filename is often
    // far wider than the dialog and two ragged lines look like a fault.
    QFont nameFont = font();
    nameFont.setPointSize(9);
    painter.setFont(nameFont);
    const QRect nameRect(8, height() - 30, width() - 16, 20);
    painter.setPen(QColor(0xaa, 0xaa, 0xaa));
    painter.drawText(nameRect,
            Qt::AlignCenter,
            QFontMetrics(nameFont).elidedText(
                    m_trackName, Qt::ElideMiddle, nameRect.width()));
}

} // namespace mixxx
