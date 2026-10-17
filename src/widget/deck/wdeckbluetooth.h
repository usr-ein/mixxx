#pragma once

#include <QColor>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <memory>

#include "widget/deck/deckpage.h"

class ControlProxy;

namespace mixxx {
namespace deck {

class BluetoothReceiver;

/// "Bluetooth receiver", in the browser's home list: the page the deck shows
/// while it plays a device, a phone or a Mac (BluetoothReceiver has the how).
///
/// The receiver is on exactly while this page is on screen: shown, the menu is
/// open, and the radio comes up; hidden -- BACK, the library closed, another
/// level -- it is closed, and Mixxx has its sound card back.
///
/// **Nothing playing: the paired devices**, to pick the one that plays (Sam:
/// nothing connects until he picks, and none jumps in untold), each with a
/// star and a bin (Sam): the star marks the one device the deck connects by
/// itself when the menu opens, the bin forgets one, asked first; so does a
/// long press on a row. + in the corner pairs a new device (the deck visible
/// to devices for a while), and Screen off is at the foot.
///
/// **Playing: what plays**, as the device tells it: the cover (or the
/// Bluetooth rune where the device sends none), title, artist and album, where
/// in the track it is, previous / play-pause / next for the device (the deck's
/// PLAY, loop halve and loop double do the same), and the device's name and
/// what the receiver is doing, with Devices (the list again, to switch at
/// once) and Screen off. While a device pairs, its code and Confirm / Cancel
/// instead.
///
/// The encoder moves through everything that can be pressed, in reading
/// order, never wrapping -- a row, its star, its bin, the next row -- and its
/// push presses it, as a finger does. BACK refuses a code waiting, steps out
/// of forgetting, or back to what plays from the list, and otherwise leaves.
///
/// **Screen off** blanks the panel and, through `[Bluetooth],screen_off`,
/// which the mapping watches, the S3's button lights: at once with the button,
/// or after three minutes without a touch or a control (Sam). Any touch wakes
/// it -- taken by the black cover over the window, so it presses nothing --
/// and so does any control, which the mapping takes too.
///
/// Painted rather than laid out in labels, as the info panel is.
class WDeckBluetooth : public QWidget, public DeckPage {
    Q_OBJECT

  public:
    /// *pReceiver* is used and not owned.
    WDeckBluetooth(BluetoothReceiver* pReceiver, QWidget* pParent = nullptr);
    ~WDeckBluetooth() override;

    // DeckPage
    bool handleMove(int steps) override;
    bool handleSelect() override;
    bool handleBack() override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;
    /// Touches anywhere on the deck, while this page is up: the screen-off
    /// clock starts again.
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    enum class View {
        /// A device's code to confirm.
        Code,
        /// What plays.
        Track,
        /// The paired devices, to pick one.
        Devices,
        /// "Forget it?"
        ForgetConfirm,
    };
    /// What can be pressed: by finger, or by the encoder's push.
    enum class Item {
        /// A device's row: picks it.
        Row,
        /// Its star: the device the deck connects by itself.
        Star,
        /// Its bin: forgets it, asked first.
        Bin,
        /// + in the list's corner: pair a new device.
        Pair,
        Previous,
        PlayPause,
        Next,
        Devices,
        ScreenOff,
        Confirm,
        Cancel,
        ForgetIt,
        Keep,
    };
    struct Press {
        Item item;
        /// The device's place in the list, for a row, a star or a bin.
        int row = -1;

        bool operator==(const Press& other) const {
            return item == other.item && row == other.row;
        }
    };

    View view() const;
    /// Everything that can be pressed now, in the encoder's order.
    QList<Press> presses() const;
    /// Where *press* is on screen; empty for a row scrolled out of view.
    QRect rectOf(const Press& press) const;
    /// The buttons at the foot.
    QRect footRect(int index) const;
    QRect rowRect(int row) const;
    /// The first row on screen: the list scrolls to keep the focus in view.
    int firstVisibleRow() const;
    int visibleRows() const;
    QRect column() const;
    /// The device row under *point*, rows only (their star and bin aside).
    int rowAt(const QPoint& point) const;
    QString footText(Item item) const;
    void press(const Press& press);
    void askToForget(int row);
    /// The focus where a view starts: the first device, else what is most
    /// pressed there (play-pause, Confirm, Keep).
    void focusFirst();
    void onChanged();
    /// The screen off (true) or on, as `[Bluetooth],screen_off` says.
    void applyScreenOff(bool off);
    /// A touch or a control: the clock again, and the screen on.
    void activity();

    void paintCover(QPainter* pPainter, const QRect& rect) const;
    void paintTrack(QPainter* pPainter, const QRect& rect) const;
    void paintDevices(QPainter* pPainter, const QRect& rect) const;
    void paintForgetConfirm(QPainter* pPainter, const QRect& rect) const;
    /// What the receiver is doing, at the column's foot, in the deck's colour.
    void paintStatus(QPainter* pPainter, const QRect& rect) const;
    void paintCode(QPainter* pPainter, const QRect& rect) const;
    void paintPresses(QPainter* pPainter) const;

    BluetoothReceiver* const m_pReceiver;
    const QColor m_accent;
    std::unique_ptr<ControlProxy> m_pScreenOff;
    std::unique_ptr<ControlProxy> m_pWake;
    /// What the encoder is on: one of presses().
    Press m_focusPress{Item::Pair};
    /// Devices pressed while a device plays: the list over what plays.
    bool m_devicesAsked = false;
    /// The device to forget, once its bin is pressed: "Forget it?".
    QString m_forgetPath;
    QString m_forgetName;
    /// The device playing, last seen: a new one ends the list asked for.
    QString m_connectedName;
    /// The view and the rows at the last look: the focus stays on what it
    /// was on as rows come and go.
    View m_viewSeen = View::Devices;
    int m_rowsSeen = 0;
    /// A finger on a row: a tap on release, a forget past the long press.
    int m_pressRow = -1;
    bool m_pressConsumed = false;
    QTimer m_longPressTimer;
    /// Once a second while a track plays: its position moves.
    QTimer m_tick;
    QTimer m_screenOffTimer;
    QPointer<QWidget> m_pBlackout;
    QString m_coverPath;
    QPixmap m_cover;
};

} // namespace deck
} // namespace mixxx
