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
        Diagnostics,
        Power,
        Autoplay,
        Stop,
        History,
        /// "Record set": the record button's dot.
        Record,
        /// "End recording": the same dot, lit, while it records.
        Recording,
        /// Discard, and nothing else: it deletes.
        Discard,
    };

    /// A bar across the row under its title.
    enum class Bar {
        None,
        /// "End recording": the input's two channels, L over R, each with
        /// its peak tick, so a channel that is missing or low shows (Sam:
        /// "to diagnose bad cabling").
        Levels,
        /// A write to a stick: how much of the set is on it.
        Progress,
    };

    Mark mark = Mark::None;
    /// Remote media only: the owning player's number, drawn before the name.
    int playerNumber = 0;
    /// Local media only: which of the deck's ports the stick is in, drawn
    /// beside the mark. 0 for anything that is not a local medium.
    int slot = 0;
    QString title;
    /// A second line under the title, smaller, or nothing: the title alone is
    /// centred in the row.
    QString subtitle;
    /// Right-aligned: counts, durations, or a failure.
    QString detail;
    /// Up to four covers, stitched 2x2 for a playlist; the first alone fills
    /// the square for an album or a label.
    QStringList coverPaths;
    /// The covers as squares side by side instead, one per path, up to four:
    /// the tracks that played together in a History row, each its own.
    bool coversSideBySide = false;
    Bar bar = Bar::None;
    /// Bar::Levels: each channel's level and its peak tick, in dBFS.
    /// Bar::Progress: how much is done, 0..1, in barLeft.
    double barLeft = 0.0;
    double barRight = 0.0;
    double holdLeft = 0.0;
    double holdRight = 0.0;
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
    /// The same number of rows with new contents, in place: the view keeps
    /// its selection and its scroll, and only what changed is redrawn. False,
    /// and nothing changed, when the number differs -- that is setRows().
    bool updateRows(const QList<MenuRow>& rows);
    /// One row's new contents, in place: a meter moving twenty times a second
    /// redraws its row and builds no other.
    void updateRow(int row, const MenuRow& contents);
    /// The row whose payload is *payload*, or -1.
    int indexOf(const QVariant& payload) const;
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
