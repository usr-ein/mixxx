#include "widget/deck/wdeckbluetooth.h"

#include <QApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/deck/bluetoothreceiver.h"
#include "library/deck/screenpower.h"
#include "moc_wdeckbluetooth.cpp"
#include "widget/deck/deckaccent.h"
#include "widget/deck/deckrune.h"

namespace {
const QColor kBackground(0x0c, 0x0c, 0x0c);
const QColor kCoverBackdrop(0x14, 0x14, 0x14);
const QColor kTitle(0xee, 0xee, 0xee);
const QColor kArtist(0xbb, 0xbb, 0xbb);
const QColor kAlbum(0x77, 0x77, 0x77);
const QColor kTrackRest(0x33, 0x33, 0x33);
const QColor kButton(0x20, 0x20, 0x20);
const QColor kButtonBorder(0x33, 0x33, 0x33);

/// The page's budget, on 1024 x 516 (a panel with a lip) to 552 (flush):
///
///     32 + cover (as high as the room above the buttons) + 24 + 72 buttons + 24
constexpr int kMargin = 32;
constexpr int kButtonHeight = 72;
constexpr int kButtonWidth = 240;
constexpr int kButtonGap = 24;
constexpr int kBottom = 24;
constexpr int kCoverMax = 340;
constexpr int kColumnGap = 40;

QFont sized(const QFont& base, int pixels, bool bold = false) {
    QFont font(base);
    font.setPixelSize(pixels);
    font.setBold(bold);
    return font;
}

/// A line that fits *width*, an ellipsis where it does not.
void drawLine(QPainter* pPainter,
        const QFont& font,
        const QColor& colour,
        int x,
        int baseline,
        int width,
        const QString& text) {
    pPainter->setFont(font);
    pPainter->setPen(colour);
    pPainter->drawText(x, baseline, QFontMetrics(font).elidedText(text, Qt::ElideRight, width));
}

/// Black over the whole window while the screen is off: it is what is seen on
/// a panel that keeps a little light, and it takes the touch that wakes the
/// deck, so that touch presses nothing underneath.
class Blackout : public QWidget {
  public:
    Blackout(QWidget* pWindow, std::function<void()> onTouch)
            : QWidget(pWindow),
              m_onTouch(std::move(onTouch)) {
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral("background-color: black;"));
        setGeometry(pWindow->rect());
        raise();
        show();
    }

  protected:
    void mousePressEvent(QMouseEvent* pEvent) override {
        pEvent->accept();
        m_onTouch();
    }

  private:
    std::function<void()> m_onTouch;
};
} // namespace

namespace mixxx {
namespace deck {

WDeckBluetooth::WDeckBluetooth(BluetoothReceiver* pReceiver, QWidget* pParent)
        : QWidget(pParent),
          m_pReceiver(pReceiver),
          m_accent(deckAccent()) {
    setObjectName(QStringLiteral("DeckBluetooth"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);

    connect(m_pReceiver, &BluetoothReceiver::changed, this, &WDeckBluetooth::onChanged);

    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, [this]() { update(); });

    m_screenOffTimer.setSingleShot(true);
    m_screenOffTimer.setInterval(static_cast<int>(bluetooth::kScreenOffAfterMs));
    connect(&m_screenOffTimer, &QTimer::timeout, this, [this]() {
        if (isVisible()) {
            m_pReceiver->screenOffControl()->set(1.0);
        }
    });

    // The receiver's own controls, there since before the skin. Watched
    // through proxies, and set through the controls themselves: a proxy is
    // not told of its own sets, and this page acts on every one, its own
    // included.
    m_pScreenOff = std::make_unique<ControlProxy>(
            m_pReceiver->screenOffControl()->getKey(), this);
    m_pScreenOff->connectValueChanged(this, [this](double value) {
        applyScreenOff(value > 0.0);
    });
    m_pWake = std::make_unique<ControlProxy>(m_pReceiver->wakeControl()->getKey(), this);
    m_pWake->connectValueChanged(this, [this](double) {
        activity();
    });
}

WDeckBluetooth::~WDeckBluetooth() {
    qApp->removeEventFilter(this);
    applyScreenOff(false);
}

void WDeckBluetooth::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    m_focus = 0;
    qApp->installEventFilter(this);
    m_pReceiver->setActive(true);
    m_screenOffTimer.start();
    onChanged();
}

void WDeckBluetooth::hideEvent(QHideEvent* pEvent) {
    QWidget::hideEvent(pEvent);
    qApp->removeEventFilter(this);
    m_screenOffTimer.stop();
    m_tick.stop();
    // The receiver puts screen_off back to 0 as it closes, which lights the
    // screen through applyScreenOff().
    m_pReceiver->setActive(false);
}

bool WDeckBluetooth::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pEvent->type() == QEvent::MouseButtonPress || pEvent->type() == QEvent::TouchBegin) {
        activity();
    }
    return QWidget::eventFilter(pObject, pEvent);
}

void WDeckBluetooth::activity() {
    if (!isVisible()) {
        return;
    }
    m_screenOffTimer.start();
    if (m_pScreenOff->get() > 0.0) {
        m_pReceiver->screenOffControl()->set(0.0);
    }
}

void WDeckBluetooth::applyScreenOff(bool off) {
    if (off == !m_pBlackout.isNull()) {
        return;
    }
    if (off) {
        m_pBlackout = new Blackout(window(), [this]() { activity(); });
        screenpower::off();
    } else {
        screenpower::on();
        delete m_pBlackout.data();
        if (isVisible()) {
            m_screenOffTimer.start();
        }
    }
}

void WDeckBluetooth::onChanged() {
    const QString coverPath = m_pReceiver->coverPath();
    if (coverPath != m_coverPath) {
        m_coverPath = coverPath;
        m_cover = coverPath.isEmpty() ? QPixmap() : QPixmap(coverPath);
    }
    if (m_focus >= buttons().size()) {
        m_focus = 0;
    }
    if (m_pReceiver->facts().playerStatus == QStringLiteral("playing") && isVisible()) {
        m_tick.start();
    } else {
        m_tick.stop();
    }
    update();
}

QList<WDeckBluetooth::Button> WDeckBluetooth::buttons() const {
    if (m_pReceiver->facts().hasPasskey) {
        return {Button::Confirm, Button::Cancel};
    }
    return {Button::Pair, Button::ScreenOff};
}

QRect WDeckBluetooth::buttonRect(int index) const {
    return QRect(kMargin + index * (kButtonWidth + kButtonGap),
            height() - kBottom - kButtonHeight,
            kButtonWidth,
            kButtonHeight);
}

QString WDeckBluetooth::buttonText(Button button) const {
    switch (button) {
    case Button::Pair:
        return m_pReceiver->facts().discoverable ? tr("Pairing…") : tr("Pair");
    case Button::ScreenOff:
        return tr("Screen off");
    case Button::Confirm:
        return tr("Confirm");
    case Button::Cancel:
        return tr("Cancel");
    }
    return QString();
}

void WDeckBluetooth::press(Button button) {
    switch (button) {
    case Button::Pair:
        m_pReceiver->pair();
        break;
    case Button::ScreenOff:
        m_pReceiver->screenOffControl()->set(1.0);
        break;
    case Button::Confirm:
        m_pReceiver->answerPairing(true);
        break;
    case Button::Cancel:
        m_pReceiver->answerPairing(false);
        break;
    }
}

bool WDeckBluetooth::handleMove(int steps) {
    const int count = static_cast<int>(buttons().size());
    m_focus = ((m_focus + steps) % count + count) % count;
    update();
    return true;
}

bool WDeckBluetooth::handleSelect() {
    const QList<Button> row = buttons();
    press(row.value(m_focus, row.first()));
    return true;
}

bool WDeckBluetooth::handleBack() {
    // A code waiting is refused before the page goes: BACK is the deck's no.
    if (m_pReceiver->facts().hasPasskey) {
        m_pReceiver->answerPairing(false);
        return true;
    }
    return false;
}

void WDeckBluetooth::mousePressEvent(QMouseEvent* pEvent) {
    const QList<Button> row = buttons();
    for (int i = 0; i < row.size(); ++i) {
        if (buttonRect(i).contains(pEvent->position().toPoint())) {
            m_focus = i;
            press(row.at(i));
            update();
            return;
        }
    }
    QWidget::mousePressEvent(pEvent);
}

void WDeckBluetooth::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kBackground);

    const int coverSize = qBound(
            120, height() - kMargin - kButtonGap - kButtonHeight - kBottom, kCoverMax);
    const QRect cover(kMargin, kMargin, coverSize, coverSize);
    paintCover(&painter, cover);

    const int left = cover.right() + kColumnGap;
    const QRect column(left, kMargin, width() - left - kMargin, coverSize);
    if (m_pReceiver->facts().hasPasskey) {
        paintCode(&painter, column);
    } else {
        paintTrack(&painter, column);
    }
    paintButtons(&painter);
}

void WDeckBluetooth::paintCover(QPainter* pPainter, const QRect& rect) const {
    if (!m_cover.isNull()) {
        const QPixmap scaled = m_cover.scaled(
                rect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pPainter->fillRect(rect, kCoverBackdrop);
        pPainter->drawPixmap(rect.x() + (rect.width() - scaled.width()) / 2,
                rect.y() + (rect.height() - scaled.height()) / 2,
                scaled);
        return;
    }
    // No cover from the phone: the rune, which is what this page is.
    QPainterPath backdrop;
    backdrop.addRoundedRect(rect, 12, 12);
    pPainter->fillPath(backdrop, kCoverBackdrop);
    paintBluetoothRune(pPainter,
            QRectF(rect).adjusted(rect.width() * 0.1,
                    rect.height() * 0.1,
                    -rect.width() * 0.1,
                    -rect.height() * 0.1),
            m_accent,
            qMax(4.0, rect.width() / 22.0));
}

void WDeckBluetooth::paintTrack(QPainter* pPainter, const QRect& rect) const {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    const bluetooth::Status status = bluetooth::status(facts);
    const bool hasTrack = !facts.track.title.isEmpty();
    int y = rect.top();

    if (hasTrack) {
        y += 34;
        drawLine(pPainter, sized(font(), 32, true), kTitle, rect.left(), y, rect.width(), facts.track.title);
        y += 40;
        drawLine(pPainter, sized(font(), 24), kArtist, rect.left(), y, rect.width(), facts.track.artist);
        y += 32;
        drawLine(pPainter, sized(font(), 20), kAlbum, rect.left(), y, rect.width(), facts.track.album);
    } else {
        // Nothing to show yet: what the receiver is doing is the headline.
        y += 34;
        const QString headline = status == bluetooth::Status::Playing ||
                        status == bluetooth::Status::Connected
                ? facts.phoneName
                : tr("Bluetooth receiver");
        drawLine(pPainter, sized(font(), 32, true), kTitle, rect.left(), y, rect.width(), headline);
        y += 40;
        QString hint;
        switch (status) {
        case bluetooth::Status::Waiting:
            hint = tr("Phones paired before connect by themselves.");
            break;
        case bluetooth::Status::Pairing:
            hint = tr("On the phone: Bluetooth, then %1.").arg(m_pReceiver->deckName());
            break;
        case bluetooth::Status::Connected:
            hint = tr("Play something on the phone.");
            break;
        default:
            break;
        }
        drawLine(pPainter, sized(font(), 20), kArtist, rect.left(), y, rect.width(), hint);
    }

    // Where in the track: a bar, the time either side.
    if (hasTrack) {
        const qint64 position = bluetooth::positionNow(facts, m_pReceiver->nowMs());
        const QFont timeFont = sized(font(), 20);
        const QFontMetrics metrics(timeFont);
        const QString elapsed = bluetooth::formatTime(position);
        const QString total = bluetooth::formatTime(facts.track.durationMs > 0 ? facts.track.durationMs : -1);
        const int baseline = rect.top() + rect.height() * 2 / 3;
        const int timeWidth = metrics.horizontalAdvance(QStringLiteral("00:00:00"));
        pPainter->setFont(timeFont);
        pPainter->setPen(kTitle);
        pPainter->drawText(rect.left(), baseline, elapsed);
        pPainter->drawText(rect.right() - metrics.horizontalAdvance(total), baseline, total);
        const QRect bar(rect.left() + timeWidth,
                baseline - metrics.ascent() / 2 - 3,
                rect.width() - 2 * timeWidth,
                6);
        pPainter->fillRect(bar, kTrackRest);
        if (facts.track.durationMs > 0) {
            const int played = static_cast<int>(bar.width() * position / facts.track.durationMs);
            pPainter->fillRect(QRect(bar.left(), bar.top(), played, bar.height()), m_accent);
        }
    }

    // What the receiver is doing, last, in the deck's colour.
    const int statusBaseline = rect.bottom() - 6;
    pPainter->setBrush(m_accent);
    pPainter->setPen(Qt::NoPen);
    pPainter->drawEllipse(QPoint(rect.left() + 7, statusBaseline - 7), 6, 6);
    drawLine(pPainter,
            sized(font(), 20),
            m_accent,
            rect.left() + 24,
            statusBaseline,
            rect.width() - 24,
            bluetooth::statusText(facts, m_pReceiver->deckName()));
}

void WDeckBluetooth::paintCode(QPainter* pPainter, const QRect& rect) const {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    int y = rect.top() + 34;
    drawLine(pPainter,
            sized(font(), 28, true),
            kTitle,
            rect.left(),
            y,
            rect.width(),
            tr("Pair with %1?").arg(facts.pairingName.isEmpty() ? tr("a phone") : facts.pairingName));
    y += 40;
    drawLine(pPainter,
            sized(font(), 20),
            kArtist,
            rect.left(),
            y,
            rect.width(),
            tr("Confirm if the phone shows the same code."));
    const QFont codeFont = sized(font(), 88, true);
    pPainter->setFont(codeFont);
    pPainter->setPen(m_accent);
    pPainter->drawText(QRect(rect.left(), y + 20, rect.width(), rect.bottom() - y - 20),
            Qt::AlignLeft | Qt::AlignVCenter,
            bluetooth::formatPasskey(facts.passkey));
}

void WDeckBluetooth::paintButtons(QPainter* pPainter) const {
    const QList<Button> row = buttons();
    const QFont font = sized(this->font(), 22, true);
    pPainter->setFont(font);
    for (int i = 0; i < row.size(); ++i) {
        const QRect rect = buttonRect(i);
        const bool focused = i == m_focus;
        QPainterPath path;
        path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        pPainter->fillPath(path, focused ? m_accent : kButton);
        if (!focused) {
            pPainter->setBrush(Qt::NoBrush);
            pPainter->setPen(QPen(kButtonBorder, 1));
            pPainter->drawPath(path);
        }
        pPainter->setPen(focused ? QColor(Qt::black) : kTitle);
        pPainter->drawText(rect, Qt::AlignCenter, buttonText(row.at(i)));
    }
}

} // namespace deck
} // namespace mixxx
