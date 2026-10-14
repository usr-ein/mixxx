#include "widget/deck/deckmenumodel.h"

namespace mixxx {
namespace deck {

DeckMenuModel::DeckMenuModel(QObject* pParent)
        : QAbstractListModel(pParent) {
    qRegisterMetaType<mixxx::deck::MenuRow>("mixxx::deck::MenuRow");
}

void DeckMenuModel::setRows(QList<MenuRow> rows) {
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

bool DeckMenuModel::updateRows(const QList<MenuRow>& rows) {
    if (rows.size() != m_rows.size()) {
        return false;
    }
    for (int i = 0; i < rows.size(); ++i) {
        const MenuRow& before = m_rows.at(i);
        const MenuRow& after = rows.at(i);
        // Field by field: a MenuRow has no operator==, and a payload is a
        // QVariant that compares as it likes.
        if (before.mark == after.mark && before.playerNumber == after.playerNumber &&
                before.slot == after.slot && before.title == after.title &&
                before.subtitle == after.subtitle && before.detail == after.detail &&
                before.coverPaths == after.coverPaths &&
                before.coversSideBySide == after.coversSideBySide &&
                before.dimmed == after.dimmed && before.payload == after.payload) {
            continue;
        }
        m_rows[i] = after;
        emit dataChanged(index(i), index(i));
    }
    return true;
}

const MenuRow& DeckMenuModel::rowAt(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        // A reference to a default row rather than undefined behaviour: the
        // view asks about rows during a reset, when the index it holds may
        // already be past the end.
        return m_invalid;
    }
    return m_rows.at(row);
}

int DeckMenuModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant DeckMenuModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const MenuRow& row = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        // Not what the delegate paints -- it reads the whole row -- but what
        // accessibility and keyboard search fall back on.
        return row.title;
    case RowDataRole:
        return QVariant::fromValue(row);
    default:
        return QVariant();
    }
}

} // namespace deck
} // namespace mixxx

#include "moc_deckmenumodel.cpp"
