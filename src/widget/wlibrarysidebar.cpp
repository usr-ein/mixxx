#include "widget/wlibrarysidebar.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QUrl>
#include <QtDebug>

#include "library/sidebarmodel.h"
#include "moc_wlibrarysidebar.cpp"
#include "util/defs.h"
#include "util/dnd.h"

constexpr int expand_time = 250;

WLibrarySidebar::WLibrarySidebar(QWidget* parent)
        : QTreeView(parent),
          WBaseWidget(this) {
    qRegisterMetaType<FocusWidget>("FocusWidget");
    //Set some properties
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    //Drag and drop setup
    setDragEnabled(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(true);
    setAcceptDrops(true);
    setAutoScroll(true);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    header()->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    // The line above sets the scroll mode on the HEADER, which is a view in its
    // own right; the tree keeps its own. Set the tree's too, or its horizontal
    // scrollbar is measured in columns: range(0, columnCount - visibleCount),
    // which for a one-column tree is range(0, 0). Sideways scrolling would then
    // be impossible no matter how far an item overflows.
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    // A wider or narrower tree means a different sizeHint, and an expand can
    // push the selected row sideways. Both need re-checking when that happens.
    connect(this, &QTreeView::expanded, this, &WLibrarySidebar::slotContentChanged);
    connect(this, &QTreeView::collapsed, this, &WLibrarySidebar::slotContentChanged);
}

void WLibrarySidebar::setModel(QAbstractItemModel* pModel) {
    QTreeView::setModel(pModel);
    if (!pModel) {
        return;
    }
    // Features populate asynchronously (Rekordbox finds USB drives, ProLink
    // finds players), so the widest item is rarely the widest one at startup.
    connect(pModel,
            &QAbstractItemModel::rowsInserted,
            this,
            &WLibrarySidebar::slotContentChanged);
    connect(pModel,
            &QAbstractItemModel::rowsRemoved,
            this,
            &WLibrarySidebar::slotContentChanged);
    connect(pModel,
            &QAbstractItemModel::modelReset,
            this,
            &WLibrarySidebar::slotContentChanged);
    connect(pModel,
            &QAbstractItemModel::dataChanged,
            this,
            &WLibrarySidebar::slotContentChanged);
}

QSize WLibrarySidebar::sizeHint() const {
    const QSize base = QTreeView::sizeHint();
    // Includes each row's icon, its label and the indentation of its depth.
    const int contentWidth = sizeHintForColumn(0);
    if (contentWidth <= 0) {
        // No model, or nothing laid out yet.
        return base;
    }
    // Chrome is MEASURED, not reconstructed from frameWidth(): the skin adds
    // padding through the stylesheet, and stylesheet padding never appears in
    // frameWidth(). Falls back to the frame alone before the first layout,
    // when width() and the viewport are not yet meaningful.
    const int chrome = qMax(width() - viewport()->width(), 2 * frameWidth());
    return QSize(contentWidth + chrome, base.height());
}

void WLibrarySidebar::slotContentChanged() {
    updateGeometry();
    ensureCurrentItemVisible();
}

void WLibrarySidebar::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QTreeView::currentChanged(current, previous);
    ensureCurrentItemVisible();
}

void WLibrarySidebar::ensureCurrentItemVisible() {
    const QModelIndex index = currentIndex();
    if (!index.isValid()) {
        return;
    }
    QScrollBar* pScrollBar = horizontalScrollBar();
    const int viewportWidth = viewport()->width();
    if (!pScrollBar || viewportWidth <= 0) {
        return;
    }
    const QRect itemRect = visualRect(index);
    if (!itemRect.isValid()) {
        return;
    }

    // visualRect() is in viewport coordinates and already carries the row's
    // indentation, but its width is the rest of the COLUMN, not the label. Take
    // the width from the delegate instead, or "fully visible" would mean "the
    // column is visible" and a long label would still sit clipped.
    const int scrollOffset = pScrollBar->value();
    const int itemLeft = itemRect.left() + scrollOffset;
    const int itemRight = itemLeft + sizeHintForIndex(index).width();

    int target = scrollOffset;
    if (itemRight > target + viewportWidth) {
        target = itemRight - viewportWidth;
    }
    // Second, and deliberately after the right edge: an item too wide to fit
    // has to lose its tail rather than its head, because the start of the name
    // is what tells you which item it is.
    if (itemLeft < target) {
        target = itemLeft;
    }
    // setValue() clamps to the bar's own range, so an over-scroll is harmless.
    pScrollBar->setValue(target);
}

void WLibrarySidebar::contextMenuEvent(QContextMenuEvent *event) {
    //if (event->state() & Qt::RightButton) { //Dis shiz don werk on windowze
    QModelIndex clickedIndex = indexAt(event->pos());
    if (!clickedIndex.isValid()) {
        return;
    }
    // Use this instead of setCurrentIndex() to keep current selection
    selectionModel()->setCurrentIndex(clickedIndex, QItemSelectionModel::NoUpdate);
    event->accept();
    emit rightClicked(event->globalPos(), clickedIndex);
    //}
}

// Drag enter event, happens when a dragged item enters the track sources view
void WLibrarySidebar::dragEnterEvent(QDragEnterEvent * event) {
    qDebug() << "WLibrarySidebar::dragEnterEvent" << event->mimeData()->formats();
    if (event->mimeData()->hasUrls()) {
        // We don't have a way to ask the LibraryFeatures whether to accept a
        // drag so for now we accept all drags. Since almost every
        // LibraryFeature accepts all files in the drop and accepts playlist
        // drops we default to those flags to DragAndDropHelper.
        QList<mixxx::FileInfo> fileInfos = DragAndDropHelper::supportedTracksFromUrls(
                event->mimeData()->urls(), false, true);
        if (!fileInfos.isEmpty()) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
    //QTreeView::dragEnterEvent(event);
}

// Drag move event, happens when a dragged item hovers over the track sources view...
void WLibrarySidebar::dragMoveEvent(QDragMoveEvent * event) {
    //qDebug() << "dragMoveEvent" << event->mimeData()->formats();
    // Start a timer to auto-expand sections the user hovers on.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPoint pos = event->position().toPoint();
#else
    QPoint pos = event->pos();
#endif
    QModelIndex index = indexAt(pos);
    if (m_hoverIndex != index) {
        m_expandTimer.stop();
        m_hoverIndex = index;
        m_expandTimer.start(expand_time, this);
    }
    // This has to be here instead of after, otherwise all drags will be
    // rejected -- rryan 3/2011
    QTreeView::dragMoveEvent(event);
    if (event->mimeData()->hasUrls()) {
        const QList<QUrl> urls = event->mimeData()->urls();
        // Drag and drop within this widget
        if ((event->source() == this)
                && (event->possibleActions() & Qt::MoveAction)) {
            // Do nothing.
            event->ignore();
        } else {
            SidebarModel* sidebarModel = qobject_cast<SidebarModel*>(model());
            bool accepted = true;
            if (sidebarModel) {
                accepted = false;
                for (const QUrl& url : urls) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    QPoint pos = event->position().toPoint();
#else
                    QPoint pos = event->pos();
#endif
                    QModelIndex destIndex = indexAt(pos);
                    if (sidebarModel->dragMoveAccept(destIndex, url)) {
                        // We only need one URL to be valid for us
                        // to accept the whole drag...
                        // consider we have a long list of valid files, checking all will
                        // take a lot of time that stales Mixxx and this makes the drop feature useless
                        // Eg. you may have tried to drag two MP3's and an EXE, the drop is accepted here,
                        // but the EXE is sorted out later after dropping
                        accepted = true;
                        break;
                    }
                }
            }
            if (accepted) {
                event->acceptProposedAction();
            } else {
                event->ignore();
            }
        }
    } else {
        event->ignore();
    }
}

void WLibrarySidebar::timerEvent(QTimerEvent *event) {
    if (event->timerId() == m_expandTimer.timerId()) {
        QPoint pos = viewport()->mapFromGlobal(QCursor::pos());
        if (viewport()->rect().contains(pos)) {
            QModelIndex index = indexAt(pos);
            if (m_hoverIndex == index) {
                setExpanded(index, !isExpanded(index));
            }
        }
        m_expandTimer.stop();
        return;
    }
    QTreeView::timerEvent(event);
}

// Drag-and-drop "drop" event. Occurs when something is dropped onto the track sources view
void WLibrarySidebar::dropEvent(QDropEvent * event) {
    if (event->mimeData()->hasUrls()) {
        // Drag and drop within this widget
        if ((event->source() == this)
                && (event->possibleActions() & Qt::MoveAction)) {
            // Do nothing.
            event->ignore();
        } else {
            //Reset the selected items (if you had anything highlighted, it clears it)
            //this->selectionModel()->clear();
            //Drag-and-drop from an external application or the track table widget
            //eg. dragging a track from Windows Explorer onto the sidebar
            SidebarModel* sidebarModel = qobject_cast<SidebarModel*>(model());
            if (sidebarModel) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QPoint pos = event->position().toPoint();
#else
                QPoint pos = event->pos();
#endif

                QModelIndex destIndex = indexAt(pos);
                // event->source() will return NULL if something is dropped from
                // a different application
                const QList<QUrl> urls = event->mimeData()->urls();
                if (sidebarModel->dropAccept(destIndex, urls, event->source())) {
                    event->acceptProposedAction();
                } else {
                    event->ignore();
                }
            }
        }
        //emit trackDropped(name);
        //repaintEverything();
    } else {
        event->ignore();
    }
}

void WLibrarySidebar::toggleSelectedItem() {
    QModelIndex index = selectedIndex();
    if (index.isValid()) {
        // Activate the item so its content shows in the main library.
        emit clicked(index);
        // Expand or collapse the item as necessary.
        setExpanded(index, !isExpanded(index));
    }
}

bool WLibrarySidebar::isLeafNodeSelected() {
    QModelIndex index = selectedIndex();
    if (index.isValid()) {
        if(!index.model()->hasChildren(index)) {
            return true;
        }
        const SidebarModel* sidebarModel = qobject_cast<const SidebarModel*>(index.model());
        if (sidebarModel) {
            return sidebarModel->hasTrackTable(index);
        }
    }
    return false;
}

bool WLibrarySidebar::isChildIndexSelected(const QModelIndex& index) {
    // qDebug() << "WLibrarySidebar::isChildIndexSelected" << index;
    QModelIndex selIndex = selectedIndex();
    if (!selIndex.isValid()) {
        return false;
    }
    SidebarModel* sidebarModel = qobject_cast<SidebarModel*>(model());
    VERIFY_OR_DEBUG_ASSERT(sidebarModel) {
        // qDebug() << " >> model() is not SidebarModel";
        return false;
    }
    QModelIndex translated = sidebarModel->translateChildIndex(index);
    if (!translated.isValid()) {
        // qDebug() << " >> index can't be translated";
        return false;
    }
    return translated == selIndex;
}

bool WLibrarySidebar::isFeatureRootIndexSelected(LibraryFeature* pFeature) {
    // qDebug() << "WLibrarySidebar::isFeatureRootIndexSelected";
    QModelIndex selIndex = selectedIndex();
    if (!selIndex.isValid()) {
        return false;
    }
    SidebarModel* sidebarModel = qobject_cast<SidebarModel*>(model());
    VERIFY_OR_DEBUG_ASSERT(sidebarModel) {
        return false;
    }
    const QModelIndex rootIndex = sidebarModel->getFeatureRootIndex(pFeature);
    return rootIndex == selIndex;
}

/// Invoked by actual keypresses (requires widget focus) and emulated keypresses
/// sent by LibraryControl
void WLibrarySidebar::keyPressEvent(QKeyEvent* event) {
    // TODO(XXX) Should first keyEvent ensure previous item has focus? I.e. if the selected
    // item is not focused, require second press to perform the desired action.

    SidebarModel* sidebarModel = qobject_cast<SidebarModel*>(model());
    QModelIndex selIndex = selectedIndex();
    if (sidebarModel && selIndex.isValid() && event->matches(QKeySequence::Paste)) {
        sidebarModel->paste(selIndex);
        return;
    }

    focusSelectedIndex();

    switch (event->key()) {
    case Qt::Key_Return:
        toggleSelectedItem();
        return;
    case Qt::Key_Down:
    case Qt::Key_Up:
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
    case Qt::Key_End:
    case Qt::Key_Home: {
        // Let the tree view move up and down for us.
        QTreeView::keyPressEvent(event);
        // After the selection changed force-activate (click) the newly selected
        // item to save us from having to push "Enter".
        QModelIndex selIndex = selectedIndex();
        if (!selIndex.isValid()) {
            return;
        }
        // Ensure the new selection is visible even if it was already selected/
        // focused, like when the topmost item was selected but out of sight and
        // we pressed Up, Home or PageUp.
        scrollTo(selIndex);
        emit pressed(selIndex);
        return;
    }
    case Qt::Key_Right: {
        if (event->modifiers() & Qt::ControlModifier) {
            emit setLibraryFocus(FocusWidget::TracksTable);
        } else {
            QTreeView::keyPressEvent(event);
        }
        return;
    }
    case Qt::Key_Left: {
        // If an expanded item is selected let QTreeView collapse it
        QModelIndex selIndex = selectedIndex();
        if (!selIndex.isValid()) {
            return;
        }
        // collapse knot
        if (isExpanded(selIndex)) {
            QTreeView::keyPressEvent(event);
            return;
        }
        // Else jump to its parent and activate it
        QModelIndex parentIndex = selIndex.parent();
        if (parentIndex.isValid()) {
            selectIndex(parentIndex);
            emit pressed(parentIndex);
        }
        return;
    }
    case Qt::Key_Escape:
        // Focus tracks table
        emit setLibraryFocus(FocusWidget::TracksTable);
        return;
    case kRenameSidebarItemShortcutKey: { // F2
        // Rename crate or playlist (internal, external, history)
        QModelIndex selIndex = selectedIndex();
        if (!selIndex.isValid()) {
            return;
        }
        if (isExpanded(selIndex)) {
            // Root views / knots can not be renamed
            return;
        }
        emit renameItem(selIndex);
        return;
    }
    case kHideRemoveShortcutKey: { // Del (macOS: Cmd+Backspace)
        // Delete crate or playlist (internal, external, history)
        if (event->modifiers() != kHideRemoveShortcutModifier) {
            return;
        }
        QModelIndex selIndex = selectedIndex();
        if (!selIndex.isValid()) {
            return;
        }
        emit deleteItem(selIndex);
        return;
    }
    default:
        QTreeView::keyPressEvent(event);
    }
}

void WLibrarySidebar::mousePressEvent(QMouseEvent* event) {
    // handle right click only in contextMenuEvent() to not select the clicked index
    if (event->buttons().testFlag(Qt::RightButton)) {
        return;
    }
    QTreeView::mousePressEvent(event);
}

void WLibrarySidebar::focusInEvent(QFocusEvent* event) {
    // Clear the current index, i.e. remove the focus indicator
    selectionModel()->clearCurrentIndex();
    QTreeView::focusInEvent(event);
}

void WLibrarySidebar::selectIndex(const QModelIndex& index) {
    //qDebug() << "WLibrarySidebar::selectIndex" << index;
    if (!index.isValid()) {
        return;
    }
    auto* pModel = new QItemSelectionModel(model());
    pModel->select(index, QItemSelectionModel::Select);
    if (selectionModel()) {
        selectionModel()->deleteLater();
    }
    if (index.parent().isValid()) {
        expand(index.parent());
    }
    setSelectionModel(pModel);
    setCurrentIndex(index);
    scrollTo(index);
}

/// Selects a child index from a feature and ensures visibility
void WLibrarySidebar::selectChildIndex(const QModelIndex& index, bool selectItem) {
    SidebarModel* sidebarModel = qobject_cast<SidebarModel*>(model());
    VERIFY_OR_DEBUG_ASSERT(sidebarModel) {
        qDebug() << "model() is not SidebarModel";
        return;
    }
    QModelIndex translated = sidebarModel->translateChildIndex(index);
    if (!translated.isValid()) {
        return;
    }

    if (selectItem) {
        auto* pModel = new QItemSelectionModel(sidebarModel);
        pModel->select(translated, QItemSelectionModel::Select);
        if (selectionModel()) {
            selectionModel()->deleteLater();
        }
        setSelectionModel(pModel);
        setCurrentIndex(translated);
    }

    QModelIndex parentIndex = translated.parent();
    while (parentIndex.isValid()) {
        expand(parentIndex);
        parentIndex = parentIndex.parent();
    }
    scrollTo(translated, EnsureVisible);
}

QModelIndex WLibrarySidebar::selectedIndex() {
    QModelIndexList selectedIndices = selectionModel()->selectedRows();
    if (selectedIndices.isEmpty()) {
        return QModelIndex();
    }
    QModelIndex selIndex = selectedIndices.first();
    DEBUG_ASSERT(selIndex.isValid());
    return selIndex;
}

/// Refocus the selected item after right-click
void WLibrarySidebar::focusSelectedIndex() {
    // After the context menu was activated (and closed, with or without clicking
    // an action), the currentIndex is the right-clicked item.
    // If if the currentIndex is not selected, make the selection the currentIndex
    QModelIndex selIndex = selectedIndex();
    if (selIndex.isValid() && selIndex != selectionModel()->currentIndex()) {
        setCurrentIndex(selIndex);
    }
}

bool WLibrarySidebar::event(QEvent* pEvent) {
    if (pEvent->type() == QEvent::ToolTip) {
        updateTooltip();
    } else if (pEvent->type() == QEvent::LayoutRequest ||
            pEvent->type() == QEvent::Resize) {
        // Force-resize the header to expand the item's clickable area.
        //
        // Reason:
        // Currently, the sidebar header expands to the width of the widest item.
        // If the sidebar is wider than that, there's some space right next to
        // items that does not respond to clicks. This is somewhat frustration as
        // it is perceived inconsistent with the state when e.g. Playlist are
        // expanded and the entire 'Tracks' row responds to clicks.
        //
        // Desired appearance & behavior:
        // * full-width items (for click success)
        // * full item text (no elide)
        // * show horizontal scrollbars as needed
        //
        // Unfortunately, there's no combination of
        //   header()->setStretchLastSection(bool);
        //   header()->setSectionResizeMode(QHeaderView::ResizeMode);
        // to achieve that.
        //
        // Though we can listen to LayoutRequest and adjust the headers minimum
        // section size to viewport width (-1 for section separator?).
        // This event occurs after Show, Resize or model data change.
        header()->setMinimumSectionSize(viewport()->width() - 1);
    }
    return QTreeView::event(pEvent);
}

void WLibrarySidebar::slotSetFont(const QFont& font) {
    setFont(font);
    // Resize the feature icons to be a bit taller than the label's capital
    int iconSize = static_cast<int>(QFontMetrics(font).height() * 0.8);
    setIconSize(QSize(iconSize, iconSize));
}
