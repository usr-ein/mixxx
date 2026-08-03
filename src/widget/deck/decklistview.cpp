#include "widget/deck/decklistview.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QScroller>

namespace {
/// Past this, a press is a hold. Matches LONG_PRESS_MS in TriMixxx.scripts.js,
/// so the screen and the deck's buttons agree on what "held" means.
constexpr int kLongPressMs = 600;
/// A swipe has to travel this far, and be mostly horizontal, to be a swipe
/// rather than a shaky tap.
constexpr int kSwipeMinPx = 120;
constexpr int kSwipeMaxMs = 500;
/// Movement under this is still a tap. Fingers are not styluses.
constexpr int kTapSlopPx = 16;
} // namespace

namespace mixxx {
namespace deck {

DeckListView::DeckListView(QWidget* pParent)
        : QListView(pParent) {
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Per pixel, not per item: a list that jumps a whole row at a time reads as
    // broken under a finger, and the kinetic scroller needs sub-row positions.
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setUniformItemSizes(true);
    setFocusPolicy(Qt::StrongFocus);

    // Kinetic scrolling, from the same synthesised pointer events the gestures
    // are read from. QScroller consumes the drag; the handlers below still see
    // press and release, which is what tap and long press need.
    QScroller::grabGesture(viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties properties = QScroller::scroller(viewport())->scrollerProperties();
    // Overshoot off: a list that bounces past its end looks like it has more
    // rows than it does.
    properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
            QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
            QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    QScroller::scroller(viewport())->setScrollerProperties(properties);

    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(kLongPressMs);
    connect(&m_longPressTimer, &QTimer::timeout, this, &DeckListView::onLongPress);
}

void DeckListView::setRowHeight(int height) {
    m_rowHeight = height;
    // The delegate reads this back through the view; setting it here keeps the
    // geometry in one place rather than in each delegate's sizeHint.
    scheduleDelayedItemsLayout();
}

int DeckListView::selectedRow() const {
    const QModelIndex index = currentIndex();
    return index.isValid() ? index.row() : -1;
}

void DeckListView::moveSelection(int steps) {
    if (!model() || model()->rowCount() == 0) {
        return;
    }
    const int last = model()->rowCount() - 1;
    int row = selectedRow();
    if (row < 0) {
        // First movement lands on the top row rather than jumping by `steps`
        // from nowhere.
        row = 0;
    } else {
        row = qBound(0, row + steps, last);
    }
    selectRow(row);
}

void DeckListView::selectRow(int row) {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return;
    }
    const QModelIndex index = model()->index(row, 0);
    setCurrentIndex(index);
    // EnsureVisible rather than PositionAtCenter: centring makes every detent
    // scroll the whole list, which is disorienting when the list is short
    // enough to fit.
    scrollTo(index, QAbstractItemView::EnsureVisible);
    emit selectionMoved(row);
}

void DeckListView::mousePressEvent(QMouseEvent* pEvent) {
    m_pressPos = pEvent->pos();
    m_pressRow = indexAt(pEvent->pos()).row();
    m_gestureConsumed = false;
    m_pressTimer.start();
    if (m_pressRow >= 0) {
        m_longPressTimer.start();
    }
    // Deliberately NOT calling the base: QListView would select on press, and
    // the selection must not move while a finger is dragging the list.
    pEvent->accept();
}

void DeckListView::mouseMoveEvent(QMouseEvent* pEvent) {
    const QPoint delta = pEvent->pos() - m_pressPos;
    if (delta.manhattanLength() > kTapSlopPx) {
        // Moved: it is a drag or a swipe, so it is not a long press any more.
        m_longPressTimer.stop();
    }
    pEvent->accept();
}

void DeckListView::mouseReleaseEvent(QMouseEvent* pEvent) {
    m_longPressTimer.stop();
    if (m_gestureConsumed) {
        resetGesture();
        pEvent->accept();
        return;
    }

    const QPoint delta = pEvent->pos() - m_pressPos;
    const qint64 elapsed = m_pressTimer.isValid() ? m_pressTimer.elapsed() : 0;

    // A swipe first: it is the only gesture that means something other than
    // "this row", and testing it last would let a sloppy one register as a tap.
    if (delta.x() >= kSwipeMinPx && qAbs(delta.x()) > qAbs(delta.y()) &&
            elapsed <= kSwipeMaxMs) {
        emit backRequested();
        resetGesture();
        pEvent->accept();
        return;
    }

    if (delta.manhattanLength() <= kTapSlopPx && m_pressRow >= 0) {
        const bool wasSelected = m_pressRow == selectedRow();
        if (wasSelected) {
            // The caller decides what this means: in a track list it toggles
            // the info layout, everywhere else the browser treats it as going
            // in. Both come from the same tap, so both cannot be decided here.
            emit reselected(m_pressRow);
        } else {
            selectRow(m_pressRow);
        }
    }
    resetGesture();
    pEvent->accept();
}

void DeckListView::onLongPress() {
    if (m_pressRow < 0) {
        return;
    }
    // Consumed, so the release that follows does not also register as a tap on
    // the same row -- which would activate it twice.
    m_gestureConsumed = true;
    selectRow(m_pressRow);
    emit activated(m_pressRow);
}

void DeckListView::resetGesture() {
    m_pressRow = -1;
    m_gestureConsumed = false;
    m_pressTimer.invalidate();
}

void DeckListView::keyPressEvent(QKeyEvent* pEvent) {
    // The keyboard mirrors the encoder exactly. Two reasons, neither cosmetic:
    // it is how the UI is driven from a script over ssh (pi_config/deck-poke),
    // and it is what a keyboard plugged into a deck with a dead touchscreen
    // would need.
    switch (pEvent->key()) {
    case Qt::Key_Up:
        moveSelection(-1);
        return;
    case Qt::Key_Down:
        moveSelection(1);
        return;
    case Qt::Key_PageUp:
        moveSelection(-5);
        return;
    case Qt::Key_PageDown:
        moveSelection(5);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (selectedRow() >= 0) {
            emit activated(selectedRow());
        }
        return;
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        emit backRequested();
        return;
    default:
        break;
    }
    QListView::keyPressEvent(pEvent);
}

} // namespace deck
} // namespace mixxx

#include "moc_decklistview.cpp"
