#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace mixxx {
namespace deck {

/// One row of any menu above a track list.
///
/// Sources, categories, genres, albums, playlists and folders are drawn by one
/// delegate off one model, because they are the same row: a mark on the left,
/// a name, something small on the right, and sometimes a cover. Giving each its
/// own model and delegate would be five copies of the same painting code.
struct MenuRow {
    enum class Mark {
        None,
        Usb,
        Sd,
        UsbLinked, ///< A remote medium: the link mark and the medium mark.
        SdLinked,
        Folder,
        Search,
        Effects,
        Diagnostics,
        Power,
    };

    Mark mark = Mark::None;
    /// Remote media only: the owning player's number, drawn before the name.
    int playerNumber = 0;
    /// Local media only: which of the deck's ports the stick is in, drawn
    /// beside the mark. 0 for anything that is not a local medium.
    int slot = 0;
    QString title;
    /// Right-aligned: counts, durations, or a failure.
    QString detail;
    /// Up to four covers, stitched 2x2 for a playlist; the first alone fills
    /// the square for an album or a label.
    QStringList coverPaths;
    /// Drawn dimmed, and not enterable: a medium still being read, a category
    /// with nothing in it.
    bool dimmed = false;
    /// What activating this row means. Interpreted by WDeckBrowser alone.
    QVariant payload;
};

/// A list of MenuRows, and nothing else.
class DeckMenuModel : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Roles {
        RowDataRole = Qt::UserRole + 1,
    };

    explicit DeckMenuModel(QObject* pParent = nullptr);

    void setRows(QList<MenuRow> rows);
    const MenuRow& rowAt(int row) const;
    bool isEmpty() const {
        return m_rows.isEmpty();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

  private:
    QList<MenuRow> m_rows;
    MenuRow m_invalid;
};

} // namespace deck
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::deck::MenuRow)
