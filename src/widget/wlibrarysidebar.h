#pragma once

#include <QBasicTimer>
#include <QModelIndex>
#include <QTreeView>

#include "library/library_decl.h"
#include "widget/wbasewidget.h"

class LibraryFeature;
class QPoint;

class WLibrarySidebar : public QTreeView, public WBaseWidget {
    Q_OBJECT
  public:
    explicit WLibrarySidebar(QWidget* parent = nullptr);

    // Width of the widest item currently on show, so a layout can make the
    // sidebar exactly as wide as its content instead of a guessed constant.
    // Grows and shrinks as nodes are expanded and collapsed; clamp it with the
    // widget's maximumWidth (the skin does).
    QSize sizeHint() const override;
    void setModel(QAbstractItemModel* pModel) override;

    void contextMenuEvent(QContextMenuEvent * event) override;
    void dragMoveEvent(QDragMoveEvent * event) override;
    void dragEnterEvent(QDragEnterEvent * event) override;
    void dropEvent(QDropEvent * event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void timerEvent(QTimerEvent* event) override;
    void toggleSelectedItem();
    bool isLeafNodeSelected();
    bool isChildIndexSelected(const QModelIndex& index);
    bool isFeatureRootIndexSelected(LibraryFeature* pFeature);

  public slots:
    void selectIndex(const QModelIndex&);
    void selectChildIndex(const QModelIndex&, bool selectItem = true);
    void slotSetFont(const QFont& font);

  signals:
    void rightClicked(const QPoint&, const QModelIndex&);
    void renameItem(const QModelIndex&);
    void deleteItem(const QModelIndex&);
    FocusWidget setLibraryFocus(FocusWidget newFocus);

  protected:
    bool event(QEvent* pEvent) override;

  protected slots:
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;

  private:
    void focusSelectedIndex();
    QModelIndex selectedIndex();
    // Scroll sideways so the current item's whole label is on screen. QTreeView
    // will not do this itself: its scrollTo() reasons about the header SECTION,
    // which for a single-column tree always starts at x=0, so it never accounts
    // for how far a deeply nested row has been indented away from that edge.
    void ensureCurrentItemVisible();
    // Content width changed (expand, collapse, rows added, font swap), so the
    // cached sizeHint is stale and the selection may have moved sideways.
    void slotContentChanged();

    QBasicTimer m_expandTimer;
    QModelIndex m_hoverIndex;
};
