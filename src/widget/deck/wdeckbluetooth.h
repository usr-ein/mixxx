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
/// while it plays a phone (BluetoothReceiver has the how).
///
/// The receiver is on exactly while this page is on screen: shown, the menu is
/// open, and the radio comes up; hidden -- BACK, the library closed, another
/// level -- it is closed, and Mixxx has its sound card back.
///
/// **Nothing playing: the paired devices**, to pick the one that plays (Sam:
/// nothing connects until he picks, and none jumps in untold), with Pair (the
/// deck visible to phones for a while), Forget and Screen off. Forget asks
/// which device, then to confirm; so does a long press on a row.
///
/// **Playing: what plays**, as the phone tells it: the cover (or the
/// Bluetooth rune where the phone sends none), title, artist and album, where
/// in the track it is, and the phone's name and what the receiver is doing,
/// with Pair, Devices (the list again, to switch at once) and Screen off.
/// While a phone pairs, its code and Confirm / Cancel instead.
///
/// The encoder moves through the rows and the buttons, never wrapping, and
/// its push presses one, as a finger does. BACK refuses a code waiting, steps
/// out of forgetting, or back to what plays from the list, and otherwise
/// leaves.
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
        /// A phone's code to confirm.
        Code,
        /// What plays.
        Track,
        /// The paired devices, to pick one.
        Devices,
        /// The same, to pick the one to forget.
        ForgetWhich,
        /// "Forget it?"
        ForgetConfirm,
    };
    enum class Button {
        Pair,
        Devices,
        Forget,
        ScreenOff,
        Confirm,
        Cancel,
        ForgetIt,
        Keep,
    };

    View view() const;
    /// The device rows on screen now, none outside the lists.
    int rowCount() const;
    /// The buttons on screen now.
    QList<Button> buttons() const;
    /// Rows first, then buttons: what the encoder moves through.
    int itemCount() const;
    QRect buttonRect(int index) const;
    QRect rowRect(int row) const;
    /// The first row on screen: the list scrolls to keep the focus in view.
    int firstVisibleRow() const;
    int visibleRows() const;
    QRect column() const;
    int rowAt(const QPoint& point) const;
    QString buttonText(Button button) const;
    void press(Button button);
    /// A row pushed or tapped: picked, or chosen to forget.
    void activateRow(int row);
    void askToForget(int row);
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
    void paintButtons(QPainter* pPainter) const;

    BluetoothReceiver* const m_pReceiver;
    const QColor m_accent;
    std::unique_ptr<ControlProxy> m_pScreenOff;
    std::unique_ptr<ControlProxy> m_pWake;
    /// Which row or button the encoder is on: rows first, then buttons.
    int m_focus = 0;
    /// Devices pressed while a phone plays: the list over what plays.
    bool m_devicesAsked = false;
    /// Forget pressed: the list picks the device to forget.
    bool m_forgetting = false;
    /// The device to forget, once picked: "Forget it?".
    QString m_forgetPath;
    QString m_forgetName;
    /// The phone playing, last seen: a new one ends the list asked for.
    QString m_phoneName;
    /// The rows at the last look: a button keeps the focus as rows come
    /// and go.
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
