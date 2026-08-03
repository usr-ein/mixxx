#include "widget/deck/wdecksortmenu.h"

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
    pLayout->addWidget(m_pView);

    // A tap chooses, as in every other menu -- the sort menu is not a track
    // list, so there is no second meaning for a tap to carry.
    connect(m_pView, &DeckListView::activated, this, [this](int) {
        activateSelection();
    });
    connect(m_pView, &DeckListView::reselected, this, [this](int) {
        activateSelection();
    });
    connect(m_pView, &DeckListView::backRequested, this, &WDeckSortMenu::dismiss);

    hide();
}

bool WDeckSortMenu::isOpen() const {
    return isVisible();
}

void WDeckSortMenu::open(const QString& currentColumn) {
    m_currentColumn = currentColumn;
    m_choosingDirection = false;
    m_chosenField = -1;
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
            // The one in force, marked so the menu says what the list is doing
            // rather than only offering to change it.
            row.detail = QStringLiteral("•");
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

void WDeckSortMenu::showDirections() {
    QList<MenuRow> rows;
    MenuRow ascending;
    ascending.title = QStringLiteral("Ascending");
    rows.append(ascending);
    MenuRow descending;
    descending.title = QStringLiteral("Descending");
    rows.append(descending);
    m_pModel->setRows(rows);
    setFixedHeight(2 * kRowHeight);
    // Preselected to whichever direction the field is normally read in.
    m_pView->selectRow(fields().at(m_chosenField).descendingByDefault ? 1 : 0);
}

void WDeckSortMenu::moveSelection(int steps) {
    m_pView->moveSelection(steps);
}

void WDeckSortMenu::activateSelection() {
    const int row = m_pView->selectedRow();
    if (row < 0) {
        return;
    }
    if (!m_choosingDirection) {
        if (fields().at(row).column.isEmpty()) {
            // Default is one step: there is no direction to "the order the list
            // came in".
            emit defaultChosen();
            hide();
            return;
        }
        m_chosenField = row;
        m_choosingDirection = true;
        showDirections();
        return;
    }
    emit sortChosen(fields().at(m_chosenField).column, row == 1);
    hide();
}

void WDeckSortMenu::dismiss() {
    hide();
    emit dismissed();
}

} // namespace deck
} // namespace mixxx

#include "moc_wdecksortmenu.cpp"
