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
/// A pop-over, raised by a short SORT press or a tap on the breadcrumb's sort
/// indicator, and only ever over a track list. Two steps: pick a field, then
/// pick a direction — except `Default`, which is one step because "the order
/// the list came in" has no direction to choose.
///
/// While it is up it takes all input, which is what "in focus" has to mean when
/// there is one encoder and no pointer to click elsewhere with.
class WDeckSortMenu : public QWidget {
    Q_OBJECT

  public:
    /// A sortable field: what it is called, and which column of the track model
    /// it sorts on. The order here is the order on screen, and it is the DJ's
    /// order of reach rather than the schema's.
    struct Field {
        QString title;
        QString column;   ///< Empty for Default.
        bool descendingByDefault = false;
    };

    explicit WDeckSortMenu(QWidget* pParent = nullptr);

    /// Raise it, with *currentColumn* preselected.
    void open(const QString& currentColumn);
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

  private:
    void showFields();
    void showDirections();

    DeckListView* m_pView;
    DeckMenuModel* m_pModel;
    MenuRowDelegate* m_pDelegate;
    /// Which step is on screen. The direction step is not a second widget: it
    /// is the same list with two rows in it.
    bool m_choosingDirection = false;
    int m_chosenField = -1;
    QString m_currentColumn;
};

} // namespace deck
} // namespace mixxx
