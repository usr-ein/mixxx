#include "widget/deck/wdecksearch.h"

#include <QGridLayout>
#include <QPushButton>

namespace {
/// Nine keys a row, which on a 1024 px panel is a 113 px target -- comfortably
/// wider than a fingertip, and it fits the alphabet in three rows.
constexpr int kColumns = 9;
constexpr int kRowHeight = 75;

const QString kLetters = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
/// Digits, then the punctuation that actually turns up in track titles. There
/// is no % or _ here, which is why the search query needs no LIKE escaping.
const QString kNumbers = QStringLiteral("0123456789&'-.()+#");
} // namespace

namespace mixxx {
namespace deck {

WDeckKeyboard::WDeckKeyboard(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("DeckKeyboard"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_pGrid = new QGridLayout(this);
    m_pGrid->setContentsMargins(4, 4, 4, 4);
    m_pGrid->setSpacing(4);
    buildKeys();
}

void WDeckKeyboard::buildKeys() {
    setPage(false);
}

void WDeckKeyboard::setPage(bool numbers) {
    m_numbers = numbers;
    for (QPushButton* pButton : m_buttons) {
        m_pGrid->removeWidget(pButton);
        pButton->deleteLater();
    }
    m_buttons.clear();

    const QString characters = numbers ? kNumbers : kLetters;
    const auto addButton = [this](const QString& label,
                                   int row,
                                   int column,
                                   int span,
                                   const std::function<void()>& action) {
        auto* pButton = new QPushButton(label, this);
        pButton->setObjectName(QStringLiteral("DeckKey"));
        pButton->setFixedHeight(kRowHeight - 8);
        pButton->setFocusPolicy(Qt::NoFocus);
        connect(pButton, &QPushButton::clicked, this, action);
        m_pGrid->addWidget(pButton, row, column, 1, span);
        m_buttons.append(pButton);
    };

    int index = 0;
    for (; index < characters.size(); ++index) {
        const QString character = characters.mid(index, 1);
        addButton(character, index / kColumns, index % kColumns, 1, [this, character]() {
            emit keyPressed(character);
        });
    }

    // Space fills whatever is left on the last letter row rather than starting
    // a row of its own.
    const int lastRow = (characters.size() - 1) / kColumns;
    const int usedInLastRow = characters.size() - lastRow * kColumns;
    if (usedInLastRow < kColumns) {
        addButton(QStringLiteral("␣"),
                lastRow,
                usedInLastRow,
                kColumns - usedInLastRow,
                [this]() { emit keyPressed(QStringLiteral(" ")); });
    }

    const int actionRow = lastRow + 1;
    addButton(numbers ? QStringLiteral("ABC") : QStringLiteral("123"),
            actionRow,
            0,
            2,
            [this]() { setPage(!m_numbers); });
    addButton(QStringLiteral("⌫"), actionRow, 2, 2, [this]() {
        emit backspacePressed();
    });
    addButton(QStringLiteral("CLEAR"), actionRow, 4, 2, [this]() {
        emit clearPressed();
    });
    addButton(QStringLiteral("DONE"), actionRow, 6, 3, [this]() {
        emit donePressed();
    });
}

} // namespace deck
} // namespace mixxx

#include "moc_wdecksearch.cpp"
