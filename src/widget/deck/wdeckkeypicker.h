#pragma once

#include <QWidget>
#include <memory>

#include "skin/legacy/skincontext.h"
#include "widget/wbasewidget.h"

class ControlEncoder;
class ControlObject;
class ControlProxy;
class ControlPushButton;

namespace mixxx {
namespace deck {

class DeckListView;
class DeckMenuModel;
class KeyRowDelegate;

/// The key list, raised by holding B6 over the deck (skin node
/// `<DeckKeyPicker>`).
///
/// The twelve keys the track on the deck can be pitched to, one per shift,
/// +6 at the top and -5 at the bottom (Sam, 2026-10-14), each row its Camelot
/// code, its name and its shift. The track's own key says "original", and the
/// key it plays in now is drawn in the colour the header shows it in: the
/// accent in its own key, KEY SYNC's purple when shifted. Picking a row writes
/// its shift to `[ProLink] key_shift`, which ProLinkKeySync applies; any other
/// way out changes nothing.
///
/// Built like the sort menu (WDeckSortMenu), on the same DeckListView: the
/// encoder moves the selection and its push picks, a finger flicks the list
/// and a tap picks, a swipe right goes back. Not inside the browser, because
/// it is raised over the deck: it spans the panel and paints only the list,
/// so a press anywhere off the list lands on it, closes it and goes no
/// further -- the "never mind" tap must not also press whatever is under it.
///
/// Driven through `[KeyPicker]`, which it owns:
///  - `open`: 1 raises it, 0 closes it, and it reads 1 while it is up. The
///    mapping routes the encoder, its push, BACK and B6 here while it does.
///    A raise is refused (put back to 0) with the library on screen or no
///    track on the deck: there is nothing to pitch.
///  - `move`: the encoder, in rows, + down the list (towards -5).
///  - `select`: the encoder's push, which picks the selected row.
///
/// It closes by itself when a track loads or the library opens: the list was
/// of the keys of a track, over the deck.
class WDeckKeyPicker : public QWidget, public WBaseWidget {
    Q_OBJECT

  public:
    explicit WDeckKeyPicker(QWidget* pParent);
    ~WDeckKeyPicker() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    /// A tap off the list closes it, on the release, and goes no further:
    /// press and release both land here.
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;

  private:
    void open();
    /// Put it away, changing nothing.
    void dismiss();
    void pick(int row);
    /// The rows for the track's key, the marks for the shift in force.
    void showRows();
    /// Repaint the marks after the shift moved, the selection staying put.
    void showShift();
    void place();

    DeckListView* m_pView;
    DeckMenuModel* m_pModel;
    KeyRowDelegate* m_pDelegate;

    std::unique_ptr<ControlObject> m_pOpen;
    std::unique_ptr<ControlEncoder> m_pMove;
    std::unique_ptr<ControlPushButton> m_pSelect;

    std::unique_ptr<ControlProxy> m_pKeyShift;
    std::unique_ptr<ControlProxy> m_pFileKey;
    std::unique_ptr<ControlProxy> m_pTrackLoaded;
    std::unique_ptr<ControlProxy> m_pShowLibrary;

    /// The press now down landed off the list.
    bool m_pressedOff = false;
};

} // namespace deck
} // namespace mixxx
