#pragma once

#include <QList>
#include <QString>
#include <QWidget>

namespace mixxx {
namespace deck {

class DeckListView;
class DeckMenuModel;
class MenuRowDelegate;

/// The sort menu (browser-prd.md 9).
///
/// A pop-over, raised by a short SORT press, and only ever over a track list.
/// **One step:** pick a field and the list re-sorts, in the direction that
/// field is normally read in.
///
/// There is no direction step. It was a second screen that every sort had to
/// walk through to answer a question that already had a right answer, and it
/// is now a toggle in the place the direction is *shown*: the breadcrumb's
/// `▲ Album`, which flips to `▼ Album` when tapped. Picking the field that is
/// already in force does the same thing, so the direction stays reachable from
/// the encoder alone.
///
/// While it is up it takes all input, which is what "in focus" has to mean when
/// there is one encoder and no pointer to click elsewhere with — and a tap
/// anywhere outside it closes it, which is what "elsewhere" means when there
/// is one.
class WDeckSortMenu : public QWidget {
    Q_OBJECT

  public:
    /// A sortable field: what it is called, and which column of the track model
    /// it sorts on. The order here is the order on screen, and it is the DJ's
    /// order of reach rather than the schema's.
    struct Field {
        QString title;
        QString column; ///< What the model sorts on. Empty for Default.
        bool descendingByDefault = false;
        /// What the info layout shows beside each title, when that is not the
        /// column being sorted on. Key is the one case: it sorts on
        /// `camelot_order`, an integer nobody wants to read, and displays the
        /// key text. Empty means "same as `column`".
        QString displayColumn;
    };

    explicit WDeckSortMenu(QWidget* pParent = nullptr);

    /// Raise it, with the sort in force preselected and marked with the arrow
    /// it is running in.
    void open(const QString& currentColumn, bool descending);
    bool isOpen() const;

    static const QList<Field>& fields();

  signals:
    /// A field and a direction were chosen.
    void sortChosen(const QString& column, bool descending);
    /// "Show these rows in the order the model produces them."
    void defaultChosen();
    void dismissed();

  public slots:
    /// The encoder, forwarded by the browser while this is up.
    void moveSelection(int steps);
    void activateSelection();
    void dismiss();

  protected:
    /// Watches the whole application while this is up, for the one thing a
    /// child widget cannot see: a press that landed somewhere else.
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;

  private:
    void showFields();

    DeckListView* m_pView;
    DeckMenuModel* m_pModel;
    MenuRowDelegate* m_pDelegate;
    /// The sort the list is running, so the menu can mark it and so picking it
    /// again means "the other way round" rather than "no change".
    QString m_currentColumn;
    bool m_currentDescending = false;
};

} // namespace deck
} // namespace mixxx
