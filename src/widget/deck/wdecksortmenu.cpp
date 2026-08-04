#include "widget/deck/wdecksortmenu.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "library/dao/trackschema.h"
#include "widget/deck/deckdelegates.h"
#include "widget/deck/decklistview.h"
#include "widget/deck/deckmenumodel.h"

namespace {
constexpr int kMenuWidth = 420;
constexpr int kRowHeight = 56;
} // namespace

namespace mixxx {
namespace deck {

const QList<WDeckSortMenu::Field>& WDeckSortMenu::fields() {
    // The order a DJ reaches for, not the schema's: tempo and key first,
    // because those are what a track has to match; then who made it and what
    // it is; then when it arrived; then the rest.
    //
    // Descending-by-default where ascending would be useless: the newest
    // additions, the highest rating, and the tracks most recently played are
    // what those columns are consulted for.
    static const QList<Field> kFields = {
            {QStringLiteral("Default"), QString(), false},
            {QStringLiteral("BPM"), LIBRARYTABLE_BPM, false},
            // Sorts on the stored Camelot index, not key_id -- key_id is
            // Mixxx's ChromaticKey enum, and sorting on it gives 11A, 6A,
            // 1A. Displays the key text regardless.
            {QStringLiteral("Key"),
                    QStringLiteral("camelot_order"),
                    false,
                    LIBRARYTABLE_KEY},
            {QStringLiteral("Title"), LIBRARYTABLE_TITLE, false},
            {QStringLiteral("Artist"), LIBRARYTABLE_ARTIST, false},
            {QStringLiteral("Genre"), LIBRARYTABLE_GENRE, false},
            {QStringLiteral("Album"), LIBRARYTABLE_ALBUM, false},
            {QStringLiteral("Date added"), LIBRARYTABLE_DATETIMEADDED, true},
            {QStringLiteral("Label"), QStringLiteral("label"), false},
            {QStringLiteral("Year"), LIBRARYTABLE_YEAR, false},
            {QStringLiteral("Duration"), LIBRARYTABLE_DURATION, false},
            {QStringLiteral("Rating"), LIBRARYTABLE_RATING, true},
    };
    return kFields;
}

WDeckSortMenu::WDeckSortMenu(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("DeckSortMenu"));
    // Without this a plain QWidget ignores its stylesheet background entirely,
    // and an overlay you can see the list through is not an overlay.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    setFixedWidth(kMenuWidth);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    m_pView = new DeckListView(this);
    m_pModel = new DeckMenuModel(this);
    m_pDelegate = new MenuRowDelegate(this);
    m_pDelegate->setRowHeight(kRowHeight);
    m_pView->setRowHeight(kRowHeight);
    m_pView->setModel(m_pModel);
    m_pView->setItemDelegate(m_pDelegate);
    // A pop-over, not a place. Scrolling here is scrolling to reach a row, so
    // the list moves and the selection stays put; and one tap on a row chooses
    // it, because the sort menu is not a track list and a tap here has no
    // second meaning to be told apart from.
    m_pView->setCentreSelection(false);
    m_pView->setTapActivates(true);
    pLayout->addWidget(m_pView);

    connect(m_pView, &DeckListView::activated, this, [this](int) {
        activateSelection();
    });
    connect(m_pView, &DeckListView::backRequested, this, &WDeckSortMenu::dismiss);

    hide();
}

bool WDeckSortMenu::isOpen() const {
    return isVisible();
}

void WDeckSortMenu::open(const QString& currentColumn, bool descending) {
    m_currentColumn = currentColumn;
    m_currentDescending = descending;
    showFields();
    show();
    raise();
    m_pView->setFocus();
}

void WDeckSortMenu::showFields() {
    QList<MenuRow> rows;
    int currentIndex = 0;
    const QList<Field>& all = fields();
    for (int i = 0; i < all.size(); ++i) {
        MenuRow row;
        row.title = all.at(i).title;
        if (!all.at(i).column.isEmpty() && all.at(i).column == m_currentColumn) {
            // The one in force, marked with the direction it is running in
            // rather than with a dot. The arrow is the whole of what a second
            // tap on this row would change, so it is what the row has to show.
            row.detail = m_currentDescending ? QStringLiteral("▼")
                                             : QStringLiteral("▲");
            currentIndex = i;
        }
        rows.append(row);
    }
    m_pModel->setRows(rows);
    // Sized to its content up to a limit, so a short menu is not a tall empty
    // box and a long one scrolls.
    setFixedHeight(qMin(rows.size() * kRowHeight, 8 * kRowHeight));
    m_pView->selectRow(currentIndex);
}

void WDeckSortMenu::moveSelection(int steps) {
    m_pView->moveSelection(steps);
}

void WDeckSortMenu::activateSelection() {
    const int row = m_pView->selectedRow();
    if (row < 0) {
        return;
    }
    const Field& field = fields().at(row);
    if (field.column.isEmpty()) {
        // Default has no direction: "the order the list came in" only comes
        // one way round.
        emit defaultChosen();
        hide();
        return;
    }
    // Picking the field already in force means the other direction -- there is
    // nothing else it could mean, and it is what keeps descending reachable
    // without a pointer. Any other field arrives in the direction it is worth
    // reading in: newest first, highest rating first, A to Z for the rest.
    const bool descending = field.column == m_currentColumn
            ? !m_currentDescending
            : field.descendingByDefault;
    emit sortChosen(field.column, descending);
    hide();
}

void WDeckSortMenu::dismiss() {
    hide();
    emit dismissed();
}

void WDeckSortMenu::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    // On the application, not on the parent: the press this is watching for
    // lands on a list view's viewport, on the breadcrumb, or on the keyboard,
    // and those are siblings and cousins rather than anything this widget can
    // see from where it sits.
    qApp->installEventFilter(this);
}

void WDeckSortMenu::hideEvent(QHideEvent* pEvent) {
    // Every path out goes through hide(), including the two that do not go
    // through dismiss(), so this is the one place the filter is certain to be
    // taken off again. An application-wide filter left installed is a tax on
    // every event the deck delivers.
    qApp->removeEventFilter(this);
    QWidget::hideEvent(pEvent);
}

bool WDeckSortMenu::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pEvent->type() != QEvent::MouseButtonPress) {
        return QWidget::eventFilter(pObject, pEvent);
    }
    // **Geometry, not parentage**, and that distinction is the whole of this
    // function being right.
    //
    // The platform delivers one press to this filter more than once, and the
    // FIRST delivery is to the QWidgetWindow -- which is a QWindow and not a
    // QWidget at all. Asking "is the receiver inside me" therefore answered no
    // for a touch on the menu's own list, closed the menu, and swallowed the
    // press on the way: the popover stopped responding to touch entirely and
    // every tap anywhere dismissed it.
    //
    // Where the finger landed is the same answer whichever object is holding
    // the event, so that is what this asks.
    auto* pMouse = static_cast<QMouseEvent*>(pEvent);
    if (rect().contains(mapFromGlobal(pMouse->globalPosition().toPoint()))) {
        return QWidget::eventFilter(pObject, pEvent);
    }
    // Consumed, not just answered: the tap's whole job was to put the menu
    // away. Letting it through as well would close the menu *and* select a
    // track row underneath it, from one touch the DJ meant as "never mind".
    // Blocking the window-level delivery is what stops the rest of the chain.
    dismiss();
    return true;
}

} // namespace deck
} // namespace mixxx

#include "moc_wdecksortmenu.cpp"
