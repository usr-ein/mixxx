#pragma once

#include <QElapsedTimer>
#include <QListView>
#include <QPoint>
#include <QScroller>
#include <QTimer>

namespace mixxx {
namespace deck {

/// The one list widget the browser uses, for every level.
///
/// It exists to hold the interaction model (browser-prd.md 4) in one place
/// rather than in six delegates: the encoder moves a selection that never
/// wraps, and a finger flicks the list past it.
///
/// **The selection sits in the middle and the list moves under it**, like the
/// reel of a slot machine. There is one scroll position and the selection is
/// it: a detent moves the list by a row, a flick moves it by many and settles
/// on whichever row it left in the middle. That is the whole reason the two
/// gestures no longer fight — before this, a finger scrolled without moving the
/// selection, so the next detent snapped the list back to wherever the
/// selection had been left behind, and the flick was undone by the thing the
/// DJ did next.
///
/// The ends are the exception, and they have to be: a list cannot scroll past
/// its own first row, so the top few and the bottom few sit where they fit and
/// the selection walks to meet them.
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
    /// Past this, a press is a hold. Matches `LONG_PRESS_MS` in
    /// TriMixxx.scripts.js, so the screen and the deck's buttons agree on what
    /// "held" means — and public, so anything else on screen that carries a
    /// second meaning under a finger agrees with both.
    static constexpr int kLongPressMs = 600;

    explicit DeckListView(QWidget* pParent = nullptr);

    /// Move the selection by *steps*, clamped at both ends.
    ///
    /// Never wraps: on a screen this size, a list that jumps from its end to
    /// its start makes "am I at the bottom?" unanswerable.
    void moveSelection(int steps);
    /// Select a row and bring it to the middle of the view.
    void selectRow(int row);
    int selectedRow() const;

    /// Row height, since every level wants its own (browser-prd.md 5..8).
    void setRowHeight(int height);
    int rowHeight() const {
        return m_rowHeight;
    }

    /// Turn the reel off: the selection stays where it is and a finger just
    /// scrolls, as in an ordinary list.
    ///
    /// For the sort menu, which is a **pop-over rather than a place**. A dozen
    /// rows you are picking one of is not a list you browse along, and a
    /// selection that moved as you scrolled toward the row you wanted would
    /// mean the thing under your finger was never the thing that was chosen.
    void setCentreSelection(bool on);
    /// Whether a scroll is under way *and* the selection is riding it. What the
    /// delegates draw the moving highlight from — see the outline in paint().
    bool isScrollingSelection() const;

    /// One tap chooses, with no select-first step.
    ///
    /// The asymmetry everywhere else exists because loading a track is
    /// expensive to undo (browser-prd.md 4.2). Picking a sort field is not: it
    /// is one tap to put back.
    void setTapActivates(bool on) {
        m_tapActivates = on;
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
    /// The list moved under a finger: the selection follows the middle of the
    /// view, live, for the whole of the scroll.
    void onScrolled();
    /// The kinetic scroll has come to rest: line the middle row up exactly.
    void onScrollerStateChanged(QScroller::State state);

  private:
    void resetGesture();
    /// The row under the middle of the viewport, or -1 when there is none.
    int centreRow() const;

    int m_rowHeight = 72;

    // Gesture state. One press at a time; the panel is not multi-touch as far
    // as Qt is concerned here.
    QPoint m_pressPos;
    int m_pressRow = -1;
    bool m_gestureConsumed = false;
    QElapsedTimer m_pressTimer;
    QTimer m_longPressTimer;
    bool m_centreSelection = true;
    bool m_tapActivates = false;
    /// Where the list was when the finger went down, so the settle can tell a
    /// scroll from a tap. A tap must not move the selection to the middle row
    /// -- it has a row of its own in mind, and it is the one under the finger.
    int m_scrollAtPress = 0;
};

} // namespace deck
} // namespace mixxx
