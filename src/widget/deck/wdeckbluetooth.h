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
/// What plays, as the phone tells it: the cover (or the Bluetooth rune where
/// the phone sends none), title, artist and album, where in the track it is,
/// and the phone's name and what the receiver is doing. Under it, Pair (the
/// deck visible to phones for a while) and Screen off; while a phone pairs,
/// its code and Confirm / Cancel instead. The encoder moves between the
/// buttons and its push presses one, as a finger does; BACK refuses a code
/// waiting, and otherwise leaves.
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
    void showEvent(QShowEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;
    /// Touches anywhere on the deck, while this page is up: the screen-off
    /// clock starts again.
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    enum class Button {
        Pair,
        ScreenOff,
        Confirm,
        Cancel,
    };

    /// The buttons on screen now: Pair and Screen off, or the code's two.
    QList<Button> buttons() const;
    QRect buttonRect(int index) const;
    QString buttonText(Button button) const;
    void press(Button button);
    void onChanged();
    /// The screen off (true) or on, as `[Bluetooth],screen_off` says.
    void applyScreenOff(bool off);
    /// A touch or a control: the clock again, and the screen on.
    void activity();

    void paintCover(QPainter* pPainter, const QRect& rect) const;
    void paintTrack(QPainter* pPainter, const QRect& rect) const;
    void paintCode(QPainter* pPainter, const QRect& rect) const;
    void paintButtons(QPainter* pPainter) const;

    BluetoothReceiver* const m_pReceiver;
    const QColor m_accent;
    std::unique_ptr<ControlProxy> m_pScreenOff;
    std::unique_ptr<ControlProxy> m_pWake;
    /// Which button the encoder is on.
    int m_focus = 0;
    /// Once a second while a track plays: its position moves.
    QTimer m_tick;
    QTimer m_screenOffTimer;
    QPointer<QWidget> m_pBlackout;
    QString m_coverPath;
    QPixmap m_cover;
};

} // namespace deck
} // namespace mixxx
