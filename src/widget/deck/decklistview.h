#pragma once

#include <QElapsedTimer>
#include <QListView>
#include <QPoint>
#include <QTimer>

namespace mixxx {
namespace deck {

/// The one list widget the browser uses, for every level.
///
/// It exists to hold the interaction model (browser-prd.md 4) in one place
/// rather than in six delegates: the encoder moves a selection that never
/// wraps, a finger scrolls without moving that selection, and the two do not
/// interfere.
///
/// **The gestures are recognised here, not by Qt.** The panel is an X11 touch
/// device whose taps reach Qt as synthesised pointer events, so press, move and
/// release are all this has to work with — which is enough for all four:
///
///   tap            press and release, near enough the same place, quickly
///   long press     press, held past 600 ms without moving
///   swipe right    press, moved > 120 px horizontally, dominant axis
///   flick          anything else vertical, handed to QScroller
///
/// The asymmetry between tap and long press is deliberate and load-bearing:
/// entering a menu is free to undo, loading a track is not, so the expensive
/// action costs the deliberate gesture.
class DeckListView : public QListView {
    Q_OBJECT

  public:
    explicit DeckListView(QWidget* pParent = nullptr);

    /// Move the selection by *steps*, clamped at both ends.
    ///
    /// Never wraps: on a screen this size, a list that jumps from its end to
    /// its start makes "am I at the bottom?" unanswerable.
    void moveSelection(int steps);
    /// Select a row and scroll it into the fully visible band.
    void selectRow(int row);
    int selectedRow() const;

    /// Row height, since every level wants its own (browser-prd.md 5..8).
    void setRowHeight(int height);
    int rowHeight() const {
        return m_rowHeight;
    }

  signals:
    /// The selection moved, however it moved. The info panel follows this.
    void selectionMoved(int row);
    /// Activate: encoder push, a long press, or a tap on an already-selected
    /// row in a menu. What it means is the browser's business.
    void activated(int row);
    /// A tap on the row that was already selected, in a track list, which is
    /// the info-layout toggle rather than an activation.
    void reselected(int row);
    void backRequested();

  protected:
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void keyPressEvent(QKeyEvent* pEvent) override;

  private slots:
    void onLongPress();

  private:
    void resetGesture();

    int m_rowHeight = 72;

    // Gesture state. One press at a time; the panel is not multi-touch as far
    // as Qt is concerned here.
    QPoint m_pressPos;
    int m_pressRow = -1;
    bool m_gestureConsumed = false;
    QElapsedTimer m_pressTimer;
    QTimer m_longPressTimer;
};

} // namespace deck
} // namespace mixxx
