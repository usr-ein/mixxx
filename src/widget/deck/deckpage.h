#pragma once

namespace mixxx {
namespace deck {

/// A full-screen page in the browser's stack that wants the deck's controls.
///
/// The browser owns three of them — the encoder's rotate and push, and BACK —
/// and most of its levels are lists, so the default is that a list consumes
/// them. Anything that is *not* a list has to say so.
///
/// Before this, it said so in `WDeckBrowser`: each control's handler carried a
/// chain of `if (m_stack.last().kind == …)` tests naming every non-list page,
/// which meant three more branches in three more places every time a page was
/// added, all of them far from the page they were about. The effect rack needs
/// drag, long-press and horizontal scroll on top of that.
///
/// So the question is asked the other way round. The browser hands the gesture
/// to whatever page is currently on the stack; a page that wants it takes it and
/// says so by returning true, and a page that does not lets the list underneath
/// have it. `WDeckBrowser::currentPage()` finds the page by asking the
/// QStackedWidget what is on screen, so there is no list of kinds to keep in
/// step either.
class DeckPage {
  public:
    virtual ~DeckPage() = default;

    /// Encoder detents. Positive is clockwise.
    virtual bool handleMove(int steps) {
        return false;
    }

    /// Encoder press.
    ///
    /// Returning true for "nothing to activate here" is deliberate and is not
    /// the same as returning false: the menu view underneath still holds the
    /// level we came from with a row selected, so letting a press fall through
    /// activates *that* — entering a medium, or raising the shutdown overlay,
    /// from a page showing neither.
    virtual bool handleSelect() {
        return false;
    }

    /// BACK. For pages with an inner mode -- adjusting a value, dragging a
    /// module -- this is where that mode is left, so BACK does not throw away
    /// the whole page from under it.
    virtual bool handleBack() {
        return false;
    }
};

} // namespace deck
} // namespace mixxx
