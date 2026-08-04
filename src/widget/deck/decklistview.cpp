#include "widget/deck/decklistview.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QScroller>

namespace {
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
    // What turns a free scroll back into a selection. Without it a flick leaves
    // the list somewhere the selection is not, which is the state this whole
    // widget exists to avoid.
    connect(QScroller::scroller(viewport()),
            &QScroller::stateChanged,
            this,
            &DeckListView::onScrollerStateChanged);
    // And this is what makes it a reel rather than a settle: the highlight
    // follows the middle of the view all the way through the scroll, so what is
    // selected is legible at every moment of it rather than announced at the
    // end.
    connect(verticalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            &DeckListView::onScrolled);
    // Nothing else may scroll this. QAbstractItemView's autoScroll follows the
    // current index with EnsureVisible whenever it changes -- which is a second
    // opinion about where the list should be, formed from inside the change
    // this widget makes to answer that same question.
    setAutoScroll(false);

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

void DeckListView::setCentreSelection(bool on) {
    m_centreSelection = on;
}

bool DeckListView::isScrollingSelection() const {
    return m_centreSelection &&
            QScroller::scroller(viewport())->state() != QScroller::Inactive;
}

void DeckListView::selectRow(int row) {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return;
    }
    const QModelIndex index = model()->index(row, 0);
    setCurrentIndex(index);
    // The middle, always, so the selection has one place on the screen and the
    // list is what moves. Qt clamps this at both ends by itself, which is the
    // behaviour the ends need: a list that fits entirely does not scroll at
    // all, and this becomes a plain selection change.
    scrollTo(index,
            m_centreSelection ? QAbstractItemView::PositionAtCenter
                              : QAbstractItemView::EnsureVisible);
    emit selectionMoved(row);
}

int DeckListView::centreRow() const {
    if (!model() || model()->rowCount() == 0) {
        return -1;
    }
    // An invalid index means the middle of the viewport is past the last row --
    // a list shorter than the view -- and there is no row in the middle to
    // speak of.
    const QModelIndex index = indexAt(
            QPoint(viewport()->width() / 2, viewport()->height() / 2));
    return index.isValid() ? index.row() : -1;
}

void DeckListView::onScrolled() {
    // Only while a finger is driving it. The encoder's path picks a row and
    // then scrolls to match, and reacting to that scroll would be the same
    // decision taken twice -- the second time from a viewport still moving.
    if (!isScrollingSelection()) {
        return;
    }
    const int row = centreRow();
    if (row < 0 || row == selectedRow()) {
        return;
    }
    // The index only, NOT selectRow(): that would scroll the list to centre the
    // row it had just picked, from inside the scroll that picked it. The reel
    // is already exactly where the finger put it; the highlight is the only
    // thing missing from it.
    setCurrentIndex(model()->index(row, 0));
    emit selectionMoved(row);
}

void DeckListView::onScrollerStateChanged(QScroller::State state) {
    if (state != QScroller::Inactive) {
        return;
    }
    // The selected row goes back from an outline to a solid highlight, and
    // nothing else would ask for that repaint: the row itself has not changed,
    // only what a stopped list draws it as.
    viewport()->update();
    if (!m_centreSelection) {
        return;
    }
    // A tap goes through the scroller too -- press and release with nothing in
    // between -- and it must not be answered by selecting the middle row, which
    // is not the row that was touched. The list not having moved is what tells
    // the two apart, and it is the only signal that survives however Qt routed
    // the events.
    if (verticalScrollBar()->value() == m_scrollAtPress) {
        return;
    }
    // The settle. onScrolled() has been keeping the selection on the middle row
    // the whole way, so this is almost always that same row -- what it adds is
    // the last half-row of travel that lines it up exactly.
    const int row = centreRow();
    if (row >= 0) {
        selectRow(row);
    }
}

void DeckListView::mousePressEvent(QMouseEvent* pEvent) {
    m_pressPos = pEvent->pos();
    m_pressRow = indexAt(pEvent->pos()).row();
    m_gestureConsumed = false;
    m_scrollAtPress = verticalScrollBar()->value();
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
        if (m_tapActivates) {
            // Selected first, so whoever handles `activated` reads the row that
            // was touched rather than the one that happened to be selected when
            // the finger came down.
            selectRow(m_pressRow);
            emit activated(m_pressRow);
        } else if (m_pressRow == selectedRow()) {
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
