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
#include "widget/deck/decklistview.h"
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
/// The device list, in the column: a headline, rows, and the status line's
/// room at the foot.
constexpr int kListTop = 56;
constexpr int kRowHeight = 60;
constexpr int kRowGap = 8;
constexpr int kStatusRoom = 40;

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

    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(DeckListView::kLongPressMs);
    connect(&m_longPressTimer, &QTimer::timeout, this, [this]() {
        // Held: a row's second meaning, forgetting it; the release that
        // follows is not also a tap.
        if (m_pressRow >= 0) {
            m_pressConsumed = true;
            askToForget(m_pressRow);
        }
    });

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
    m_devicesAsked = false;
    m_forgetting = false;
    m_forgetPath.clear();
    m_pressRow = -1;
    focusFirst();
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
    const bluetooth::Facts& facts = m_pReceiver->facts();
    if (facts.phoneName != m_phoneName) {
        m_phoneName = facts.phoneName;
        // The one picked came in, or went: what it plays, or the list.
        if (!m_forgetting) {
            m_devicesAsked = false;
            focusFirst();
        }
    }
    if (m_forgetting) {
        bool there = m_forgetPath.isEmpty();
        for (const bluetooth::Device& device : facts.devices) {
            there = there || device.path == m_forgetPath;
        }
        if (!there || facts.devices.isEmpty()) {
            // Gone meanwhile (forgotten, or the radio went): nothing to ask.
            m_forgetting = false;
            m_forgetPath.clear();
            focusFirst();
        }
    }
    // A button keeps the focus as rows come and go; the first item stays the
    // first, so the first device listed takes it from Pair.
    const int rows = rowCount();
    if (m_focus >= m_rowsSeen && !(m_focus == 0 && m_rowsSeen == 0)) {
        m_focus += rows - m_rowsSeen;
    }
    m_rowsSeen = rows;
    m_focus = qBound(0, m_focus, itemCount() - 1);
    if (facts.playerStatus == QStringLiteral("playing") && isVisible()) {
        m_tick.start();
    } else {
        m_tick.stop();
    }
    update();
}

WDeckBluetooth::View WDeckBluetooth::view() const {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    if (facts.hasPasskey) {
        return View::Code;
    }
    if (!m_forgetPath.isEmpty()) {
        return View::ForgetConfirm;
    }
    if (m_forgetting) {
        return View::ForgetWhich;
    }
    if (!facts.phoneName.isEmpty() && !m_devicesAsked) {
        return View::Track;
    }
    return View::Devices;
}

int WDeckBluetooth::rowCount() const {
    const View now = view();
    return now == View::Devices || now == View::ForgetWhich
            ? static_cast<int>(m_pReceiver->facts().devices.size())
            : 0;
}

QList<WDeckBluetooth::Button> WDeckBluetooth::buttons() const {
    switch (view()) {
    case View::Code:
        return {Button::Confirm, Button::Cancel};
    case View::Track:
        return {Button::Pair, Button::Devices, Button::ScreenOff};
    case View::Devices:
        if (m_pReceiver->facts().devices.isEmpty()) {
            return {Button::Pair, Button::ScreenOff};
        }
        return {Button::Pair, Button::Forget, Button::ScreenOff};
    case View::ForgetWhich:
        return {Button::Cancel};
    case View::ForgetConfirm:
        return {Button::ForgetIt, Button::Keep};
    }
    return {};
}

int WDeckBluetooth::itemCount() const {
    return rowCount() + static_cast<int>(buttons().size());
}

void WDeckBluetooth::focusFirst() {
    m_focus = 0;
    m_rowsSeen = rowCount();
}

QRect WDeckBluetooth::buttonRect(int index) const {
    return QRect(kMargin + index * (kButtonWidth + kButtonGap),
            height() - kBottom - kButtonHeight,
            kButtonWidth,
            kButtonHeight);
}

QRect WDeckBluetooth::column() const {
    const int coverSize = qBound(
            120, height() - kMargin - kButtonGap - kButtonHeight - kBottom, kCoverMax);
    const int left = kMargin + coverSize + kColumnGap;
    return QRect(left, kMargin, width() - left - kMargin, coverSize);
}

int WDeckBluetooth::visibleRows() const {
    return qMax(1, (column().height() - kListTop - kStatusRoom + kRowGap) / (kRowHeight + kRowGap));
}

int WDeckBluetooth::firstVisibleRow() const {
    // Scrolled just enough for the focused row; from the top otherwise.
    return m_focus < rowCount() ? qMax(0, m_focus - visibleRows() + 1) : 0;
}

QRect WDeckBluetooth::rowRect(int row) const {
    const QRect area = column();
    const int index = row - firstVisibleRow();
    return QRect(area.left(),
            area.top() + kListTop + index * (kRowHeight + kRowGap),
            area.width(),
            kRowHeight);
}

int WDeckBluetooth::rowAt(const QPoint& point) const {
    const int last = qMin(rowCount(), firstVisibleRow() + visibleRows());
    for (int row = firstVisibleRow(); row < last; ++row) {
        if (rowRect(row).contains(point)) {
            return row;
        }
    }
    return -1;
}

QString WDeckBluetooth::buttonText(Button button) const {
    switch (button) {
    case Button::Pair:
        return m_pReceiver->facts().discoverable ? tr("Pairing…") : tr("Pair");
    case Button::Devices:
        return tr("Devices");
    case Button::Forget:
        return tr("Forget");
    case Button::ScreenOff:
        return tr("Screen off");
    case Button::Confirm:
        return tr("Confirm");
    case Button::Cancel:
        return tr("Cancel");
    case Button::ForgetIt:
        return tr("Forget");
    case Button::Keep:
        return tr("Keep");
    }
    return QString();
}

void WDeckBluetooth::press(Button button) {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    switch (button) {
    case Button::Pair:
        m_pReceiver->pair();
        break;
    case Button::Devices:
        m_devicesAsked = true;
        focusFirst();
        // From the one playing.
        for (int i = 0; i < facts.devices.size(); ++i) {
            if (facts.devices.at(i).picked) {
                m_focus = i;
            }
        }
        break;
    case Button::Forget:
        m_forgetting = true;
        focusFirst();
        break;
    case Button::ScreenOff:
        m_pReceiver->screenOffControl()->set(1.0);
        break;
    case Button::Confirm:
        m_pReceiver->answerPairing(true);
        break;
    case Button::Cancel:
        if (view() == View::ForgetWhich) {
            m_forgetting = false;
            focusFirst();
        } else {
            m_pReceiver->answerPairing(false);
        }
        break;
    case Button::ForgetIt:
        m_pReceiver->forget(m_forgetPath);
        m_forgetPath.clear();
        m_forgetName.clear();
        m_forgetting = false;
        focusFirst();
        break;
    case Button::Keep:
        m_forgetPath.clear();
        m_forgetName.clear();
        m_forgetting = false;
        focusFirst();
        break;
    }
    update();
}

void WDeckBluetooth::activateRow(int row) {
    const QList<bluetooth::Device>& devices = m_pReceiver->facts().devices;
    if (row < 0 || row >= devices.size()) {
        return;
    }
    if (view() == View::ForgetWhich) {
        askToForget(row);
        return;
    }
    const bluetooth::Device& device = devices.at(row);
    if (device.picked && device.connected) {
        // The one playing already: back to what it plays.
        m_devicesAsked = false;
        focusFirst();
        update();
        return;
    }
    m_pReceiver->pick(device.path);
    update();
}

void WDeckBluetooth::askToForget(int row) {
    const QList<bluetooth::Device>& devices = m_pReceiver->facts().devices;
    if (row < 0 || row >= devices.size()) {
        return;
    }
    m_forgetting = true;
    m_forgetPath = devices.at(row).path;
    m_forgetName = devices.at(row).name;
    // On Keep: forgetting is the deliberate choice, one more move away.
    m_focus = 1;
    m_rowsSeen = 0;
    update();
}

bool WDeckBluetooth::handleMove(int steps) {
    // Never wraps, as the deck's lists: the ends are where they look.
    m_focus = qBound(0, m_focus + steps, itemCount() - 1);
    update();
    return true;
}

bool WDeckBluetooth::handleSelect() {
    const int rows = rowCount();
    if (m_focus < rows) {
        activateRow(m_focus);
        return true;
    }
    const QList<Button> row = buttons();
    press(row.value(m_focus - rows, row.first()));
    return true;
}

bool WDeckBluetooth::handleBack() {
    // A code waiting is refused before the page goes: BACK is the deck's no.
    if (m_pReceiver->facts().hasPasskey) {
        m_pReceiver->answerPairing(false);
        return true;
    }
    switch (view()) {
    case View::ForgetConfirm:
        press(Button::Keep);
        return true;
    case View::ForgetWhich:
        m_forgetting = false;
        focusFirst();
        update();
        return true;
    case View::Devices:
        if (m_devicesAsked && !m_pReceiver->facts().phoneName.isEmpty()) {
            // The list over what plays: back to it.
            m_devicesAsked = false;
            focusFirst();
            update();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void WDeckBluetooth::mousePressEvent(QMouseEvent* pEvent) {
    const QPoint point = pEvent->position().toPoint();
    const QList<Button> row = buttons();
    for (int i = 0; i < row.size(); ++i) {
        if (buttonRect(i).contains(point)) {
            m_focus = rowCount() + i;
            press(row.at(i));
            return;
        }
    }
    // A row acts on release: held past the long press, it is forgotten
    // instead (asked first).
    const int pressed = rowAt(point);
    if (pressed >= 0) {
        m_pressRow = pressed;
        m_pressConsumed = false;
        m_focus = pressed;
        m_longPressTimer.start();
        update();
        pEvent->accept();
        return;
    }
    QWidget::mousePressEvent(pEvent);
}

void WDeckBluetooth::mouseReleaseEvent(QMouseEvent* pEvent) {
    m_longPressTimer.stop();
    const int pressed = m_pressRow;
    const bool consumed = m_pressConsumed;
    m_pressRow = -1;
    m_pressConsumed = false;
    if (pressed >= 0) {
        if (!consumed && rowAt(pEvent->position().toPoint()) == pressed) {
            activateRow(pressed);
        }
        pEvent->accept();
        return;
    }
    QWidget::mouseReleaseEvent(pEvent);
}

void WDeckBluetooth::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kBackground);

    const QRect area = column();
    paintCover(&painter, QRect(kMargin, kMargin, area.height(), area.height()));
    switch (view()) {
    case View::Code:
        paintCode(&painter, area);
        break;
    case View::Track:
        paintTrack(&painter, area);
        break;
    case View::Devices:
    case View::ForgetWhich:
        paintDevices(&painter, area);
        break;
    case View::ForgetConfirm:
        paintForgetConfirm(&painter, area);
        break;
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
        drawLine(pPainter,
                sized(font(), 32, true),
                kTitle,
                rect.left(),
                y,
                rect.width(),
                facts.track.title);
        y += 40;
        drawLine(pPainter,
                sized(font(), 24),
                kArtist,
                rect.left(),
                y,
                rect.width(),
                facts.track.artist);
        y += 32;
        drawLine(pPainter,
                sized(font(), 20),
                kAlbum,
                rect.left(),
                y,
                rect.width(),
                facts.track.album);
    } else {
        // Nothing to show yet: what the receiver is doing is the headline.
        y += 34;
        const QString headline = status == bluetooth::Status::Playing ||
                        status == bluetooth::Status::Connected
                ? facts.phoneName
                : tr("Bluetooth receiver");
        drawLine(pPainter, sized(font(), 32, true), kTitle, rect.left(), y, rect.width(), headline);
        y += 40;
        const QString hint = status == bluetooth::Status::Connected
                ? tr("Play something on the phone.")
                : QString();
        drawLine(pPainter, sized(font(), 20), kArtist, rect.left(), y, rect.width(), hint);
    }

    // Where in the track: a bar, the time either side.
    if (hasTrack) {
        const qint64 position = bluetooth::positionNow(facts, m_pReceiver->nowMs());
        const QFont timeFont = sized(font(), 20);
        const QFontMetrics metrics(timeFont);
        const QString elapsed = bluetooth::formatTime(position);
        const QString total = bluetooth::formatTime(
                facts.track.durationMs > 0 ? facts.track.durationMs : -1);
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

    paintStatus(pPainter, rect);
}

void WDeckBluetooth::paintDevices(QPainter* pPainter, const QRect& rect) const {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    const bluetooth::Status status = bluetooth::status(facts);
    const bool forgetting = view() == View::ForgetWhich;
    QString headline;
    if (forgetting) {
        headline = tr("Forget which phone?");
    } else if (!facts.devices.isEmpty()) {
        headline = tr("Pick a phone");
    } else if (status == bluetooth::Status::Waiting) {
        headline = tr("No phone paired yet");
    } else {
        headline = tr("Bluetooth receiver");
    }
    drawLine(pPainter,
            sized(font(), 32, true),
            kTitle,
            rect.left(),
            rect.top() + 34,
            rect.width(),
            headline);

    if (facts.devices.isEmpty()) {
        QString hint;
        if (status == bluetooth::Status::Pairing) {
            hint = tr("On the phone: Bluetooth, then %1.").arg(m_pReceiver->deckName());
        } else if (status == bluetooth::Status::Waiting) {
            hint = tr("Press Pair, then choose %1 on the phone.").arg(m_pReceiver->deckName());
        }
        drawLine(pPainter,
                sized(font(), 20),
                kArtist,
                rect.left(),
                rect.top() + 74,
                rect.width(),
                hint);
        paintStatus(pPainter, rect);
        return;
    }

    const QFont nameFont = sized(font(), 22, true);
    const QFont detailFont = sized(font(), 20);
    const int last = qMin(static_cast<int>(facts.devices.size()),
            firstVisibleRow() + visibleRows());
    for (int i = firstVisibleRow(); i < last; ++i) {
        const bluetooth::Device& device = facts.devices.at(i);
        const QRect row = rowRect(i);
        const bool focused = i == m_focus;
        QPainterPath path;
        path.addRoundedRect(QRectF(row).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        pPainter->fillPath(path, focused ? m_accent : kButton);
        if (!focused) {
            pPainter->setBrush(Qt::NoBrush);
            pPainter->setPen(QPen(kButtonBorder, 1));
            pPainter->drawPath(path);
        }
        // What the one picked is doing, at the row's end.
        QString detail;
        if (!forgetting && device.picked) {
            if (device.connected) {
                detail = facts.streaming ? tr("Playing") : tr("Connected");
            } else if (!facts.connectingName.isEmpty()) {
                detail = tr("Connecting…");
            }
        }
        const int detailWidth = detail.isEmpty()
                ? 0
                : QFontMetrics(detailFont).horizontalAdvance(detail) + 20;
        const int baseline = row.top() + row.height() / 2 + 8;
        drawLine(pPainter,
                nameFont,
                focused ? QColor(Qt::black) : kTitle,
                row.left() + 20,
                baseline,
                row.width() - 40 - detailWidth,
                device.name);
        if (!detail.isEmpty()) {
            drawLine(pPainter,
                    detailFont,
                    focused ? QColor(Qt::black) : m_accent,
                    row.right() - detailWidth,
                    baseline,
                    detailWidth,
                    detail);
        }
    }
    paintStatus(pPainter, rect);
}

void WDeckBluetooth::paintForgetConfirm(QPainter* pPainter, const QRect& rect) const {
    drawLine(pPainter,
            sized(font(), 28, true),
            kTitle,
            rect.left(),
            rect.top() + 34,
            rect.width(),
            tr("Forget %1?").arg(m_forgetName));
    drawLine(pPainter,
            sized(font(), 20),
            kArtist,
            rect.left(),
            rect.top() + 74,
            rect.width(),
            tr("It has to pair again to play here."));
}

void WDeckBluetooth::paintStatus(QPainter* pPainter, const QRect& rect) const {
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
            bluetooth::statusText(m_pReceiver->facts(), m_pReceiver->deckName()));
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
            tr("Pair with %1?")
                    .arg(facts.pairingName.isEmpty() ? tr("a phone")
                                                     : facts.pairingName));
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
    const int rows = rowCount();
    const QFont font = sized(this->font(), 22, true);
    pPainter->setFont(font);
    for (int i = 0; i < row.size(); ++i) {
        const QRect rect = buttonRect(i);
        const bool focused = rows + i == m_focus;
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
