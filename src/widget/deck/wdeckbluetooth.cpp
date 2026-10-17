#include "widget/deck/wdeckbluetooth.h"

#include <QApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

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
/// A row's star and bin: squares as high as the row, after it.
constexpr int kIconGap = 8;
/// + in the list's corner, beside the headline.
constexpr int kPairWidth = 60;
constexpr int kPairHeight = 48;
/// The device's buttons under the time: previous, play-pause, next.
constexpr int kTransportWidth = 96;
constexpr int kTransportGap = 24;
/// The marks drawn in buttons, as the deck's other marks are: strokes.
constexpr qreal kMarkPen = 2.5;

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

/// A button's ground: the accent when focused, else dark with a rim.
void paintGround(QPainter* pPainter, const QRect& rect, bool focused, const QColor& accent) {
    QPainterPath path;
    path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    pPainter->fillPath(path, focused ? accent : kButton);
    if (!focused) {
        pPainter->setBrush(Qt::NoBrush);
        pPainter->setPen(QPen(kButtonBorder, 1));
        pPainter->drawPath(path);
    }
}

/// ▶, about *centre*.
void paintPlayMark(QPainter* pPainter, const QPointF& centre, qreal size, const QColor& colour) {
    QPainterPath triangle;
    triangle.moveTo(centre + QPointF(-size * 0.35, -size * 0.45));
    triangle.lineTo(centre + QPointF(size * 0.5, 0));
    triangle.lineTo(centre + QPointF(-size * 0.35, size * 0.45));
    triangle.closeSubpath();
    pPainter->fillPath(triangle, colour);
}

/// ❚❚, about *centre*.
void paintPauseMark(QPainter* pPainter, const QPointF& centre, qreal size, const QColor& colour) {
    const qreal bar = size * 0.28;
    const qreal height = size * 0.9;
    pPainter->fillRect(QRectF(centre.x() - size * 0.38, centre.y() - height / 2, bar, height),
            colour);
    pPainter->fillRect(QRectF(centre.x() + size * 0.1, centre.y() - height / 2, bar, height),
            colour);
}

/// ⏭ (forward) or ⏮: a triangle, and a bar at its point.
void paintSkipMark(QPainter* pPainter,
        const QPointF& centre,
        qreal size,
        const QColor& colour,
        bool forward) {
    const qreal side = forward ? 1.0 : -1.0;
    QPainterPath triangle;
    triangle.moveTo(centre + QPointF(-side * size * 0.45, -size * 0.45));
    triangle.lineTo(centre + QPointF(side * size * 0.3, 0));
    triangle.lineTo(centre + QPointF(-side * size * 0.45, size * 0.45));
    triangle.closeSubpath();
    pPainter->fillPath(triangle, colour);
    const qreal bar = size * 0.16;
    const qreal x = forward ? centre.x() + size * 0.3 : centre.x() - size * 0.3 - bar;
    pPainter->fillRect(QRectF(x, centre.y() - size * 0.45, bar, size * 0.9), colour);
}

/// A five-pointed star: filled for the starred device, an outline otherwise.
void paintStarMark(QPainter* pPainter,
        const QPointF& centre,
        qreal size,
        const QColor& colour,
        bool filled) {
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal radius = (i % 2 == 0 ? 0.5 : 0.21) * size;
        const qreal angle = qDegreesToRadians(-90.0 + 36.0 * i);
        star << centre + QPointF(radius * qCos(angle), radius * qSin(angle));
    }
    QPen pen(colour, kMarkPen);
    pen.setJoinStyle(Qt::RoundJoin);
    pPainter->setPen(pen);
    pPainter->setBrush(filled ? QBrush(colour) : Qt::NoBrush);
    pPainter->drawPolygon(star);
}

/// A bin: its lid with a handle, and its body with two ribs.
void paintBinMark(QPainter* pPainter, const QPointF& centre, qreal size, const QColor& colour) {
    QPen pen(colour, kMarkPen);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    pPainter->setPen(pen);
    pPainter->setBrush(Qt::NoBrush);
    const qreal top = centre.y() - size * 0.42;
    const qreal lid = top + size * 0.14;
    const qreal bottom = centre.y() + size * 0.45;
    pPainter->drawLine(QPointF(centre.x() - size * 0.42, lid), QPointF(centre.x() + size * 0.42, lid));
    pPainter->drawPolyline(QPolygonF{QPointF(centre.x() - size * 0.12, lid),
            QPointF(centre.x() - size * 0.12, top),
            QPointF(centre.x() + size * 0.12, top),
            QPointF(centre.x() + size * 0.12, lid)});
    pPainter->drawPolygon(QPolygonF{QPointF(centre.x() - size * 0.32, lid),
            QPointF(centre.x() - size * 0.25, bottom),
            QPointF(centre.x() + size * 0.25, bottom),
            QPointF(centre.x() + size * 0.32, lid)});
    pPainter->drawLine(QPointF(centre.x() - size * 0.08, lid + size * 0.16),
            QPointF(centre.x() - size * 0.08, bottom - size * 0.14));
    pPainter->drawLine(QPointF(centre.x() + size * 0.08, lid + size * 0.16),
            QPointF(centre.x() + size * 0.08, bottom - size * 0.14));
}

/// +.
void paintPlusMark(QPainter* pPainter, const QPointF& centre, qreal size, const QColor& colour) {
    QPen pen(colour, kMarkPen + 0.5);
    pen.setCapStyle(Qt::RoundCap);
    pPainter->setPen(pen);
    pPainter->drawLine(centre + QPointF(-size / 2, 0), centre + QPointF(size / 2, 0));
    pPainter->drawLine(centre + QPointF(0, -size / 2), centre + QPointF(0, size / 2));
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
    if (facts.connectedName != m_connectedName) {
        m_connectedName = facts.connectedName;
        // The one picked came in, or went: what it plays, or the list.
        if (m_forgetPath.isEmpty()) {
            m_devicesAsked = false;
        }
    }
    if (!m_forgetPath.isEmpty()) {
        bool there = false;
        for (const bluetooth::Device& device : facts.devices) {
            there = there || device.path == m_forgetPath;
        }
        if (!there) {
            // Gone meanwhile (forgotten, or the radio went): nothing to ask.
            m_forgetPath.clear();
        }
    }
    // A new view starts where it starts; the same one keeps the focus where it
    // was, on the same row as rows come and go, or as near as there is.
    const View now = view();
    const int rows = static_cast<int>(facts.devices.size());
    if (now != m_viewSeen) {
        focusFirst();
    } else if (rows != m_rowsSeen) {
        const QList<Press> list = presses();
        const Press was = m_focusPress;
        if (!list.contains(was)) {
            if (was.row >= rows && rows > 0) {
                m_focusPress = Press{was.item, rows - 1};
            } else {
                focusFirst();
            }
        }
    }
    m_viewSeen = now;
    m_rowsSeen = rows;
    if (bluetooth::isPlaying(facts.playerStatus) && isVisible()) {
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
    if (!facts.connectedName.isEmpty() && !m_devicesAsked) {
        return View::Track;
    }
    return View::Devices;
}

QList<WDeckBluetooth::Press> WDeckBluetooth::presses() const {
    switch (view()) {
    case View::Code:
        return {{Item::Confirm}, {Item::Cancel}};
    case View::Track:
        return {{Item::Previous},
                {Item::PlayPause},
                {Item::Next},
                {Item::Devices},
                {Item::ScreenOff}};
    case View::Devices: {
        // + first, as it is above the rows: the encoder starts on the first
        // row, and one turn back reaches it.
        QList<Press> list{{Item::Pair}};
        const int rows = static_cast<int>(m_pReceiver->facts().devices.size());
        for (int row = 0; row < rows; ++row) {
            list << Press{Item::Row, row} << Press{Item::Star, row} << Press{Item::Bin, row};
        }
        list << Press{Item::ScreenOff};
        return list;
    }
    case View::ForgetConfirm:
        return {{Item::ForgetIt}, {Item::Keep}};
    }
    return {};
}

void WDeckBluetooth::focusFirst() {
    switch (view()) {
    case View::Code:
        m_focusPress = Press{Item::Confirm};
        break;
    case View::Track:
        m_focusPress = Press{Item::PlayPause};
        break;
    case View::Devices:
        m_focusPress = m_pReceiver->facts().devices.isEmpty() ? Press{Item::Pair}
                                                              : Press{Item::Row, 0};
        break;
    case View::ForgetConfirm:
        // On Keep: forgetting is the deliberate choice, one more move away.
        m_focusPress = Press{Item::Keep};
        break;
    }
}

QRect WDeckBluetooth::footRect(int index) const {
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
    return m_focusPress.row >= 0 ? qMax(0, m_focusPress.row - visibleRows() + 1) : 0;
}

QRect WDeckBluetooth::rowRect(int row) const {
    const QRect area = column();
    const int index = row - firstVisibleRow();
    if (index < 0 || index >= visibleRows()) {
        return QRect();
    }
    return QRect(area.left(),
            area.top() + kListTop + index * (kRowHeight + kRowGap),
            area.width() - 2 * (kRowHeight + kIconGap),
            kRowHeight);
}

QRect WDeckBluetooth::rectOf(const Press& press) const {
    const QRect area = column();
    // The time's baseline, half way down the column, and the device's
    // buttons under it.
    const int transportTop = area.top() + area.height() / 2 + 28;
    const int transportLeft = area.left() +
            (area.width() - 3 * kTransportWidth - 2 * kTransportGap) / 2;
    switch (press.item) {
    case Item::Row:
        return rowRect(press.row);
    case Item::Star:
    case Item::Bin: {
        const QRect row = rowRect(press.row);
        if (row.isNull()) {
            return QRect();
        }
        const int step = press.item == Item::Star ? 0 : 1;
        return QRect(row.right() + 1 + kIconGap + step * (kRowHeight + kIconGap),
                row.top(),
                kRowHeight,
                kRowHeight);
    }
    case Item::Pair:
        return QRect(area.right() + 1 - kPairWidth, area.top(), kPairWidth, kPairHeight);
    case Item::Previous:
    case Item::PlayPause:
    case Item::Next: {
        const int index = press.item == Item::Previous ? 0 : press.item == Item::PlayPause ? 1 : 2;
        return QRect(transportLeft + index * (kTransportWidth + kTransportGap),
                transportTop,
                kTransportWidth,
                kButtonHeight);
    }
    default:
        break;
    }
    // At the foot, in the order listed.
    int index = 0;
    for (const Press& listed : presses()) {
        if (listed.item == press.item) {
            return footRect(index);
        }
        switch (listed.item) {
        case Item::Devices:
        case Item::ScreenOff:
        case Item::Confirm:
        case Item::Cancel:
        case Item::ForgetIt:
        case Item::Keep:
            ++index;
            break;
        default:
            break;
        }
    }
    return QRect();
}

int WDeckBluetooth::rowAt(const QPoint& point) const {
    const int rows = view() == View::Devices
            ? static_cast<int>(m_pReceiver->facts().devices.size())
            : 0;
    for (int row = 0; row < rows; ++row) {
        if (rowRect(row).contains(point)) {
            return row;
        }
    }
    return -1;
}

QString WDeckBluetooth::footText(Item item) const {
    switch (item) {
    case Item::Devices:
        return tr("Devices");
    case Item::ScreenOff:
        return tr("Screen off");
    case Item::Confirm:
        return tr("Confirm");
    case Item::Cancel:
        return tr("Cancel");
    case Item::ForgetIt:
        return tr("Forget");
    case Item::Keep:
        return tr("Keep");
    default:
        return QString();
    }
}

void WDeckBluetooth::press(const Press& press) {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    const QString path = facts.devices.value(press.row).path;
    switch (press.item) {
    case Item::Row: {
        if (press.row < 0 || press.row >= facts.devices.size()) {
            break;
        }
        const bluetooth::Device& device = facts.devices.at(press.row);
        if (device.picked && device.connected) {
            // The one playing already: back to what it plays.
            m_devicesAsked = false;
            focusFirst();
            break;
        }
        m_pReceiver->pick(device.path);
        break;
    }
    case Item::Star:
        if (!path.isEmpty()) {
            m_pReceiver->star(path);
        }
        break;
    case Item::Bin:
        askToForget(press.row);
        break;
    case Item::Pair:
        m_pReceiver->pair();
        break;
    case Item::Previous:
        m_pReceiver->previous();
        break;
    case Item::PlayPause:
        m_pReceiver->playPause();
        break;
    case Item::Next:
        m_pReceiver->next();
        break;
    case Item::Devices:
        m_devicesAsked = true;
        // From the one playing.
        focusFirst();
        for (int i = 0; i < facts.devices.size(); ++i) {
            if (facts.devices.at(i).picked) {
                m_focusPress = Press{Item::Row, i};
            }
        }
        m_viewSeen = view();
        break;
    case Item::ScreenOff:
        m_pReceiver->screenOffControl()->set(1.0);
        break;
    case Item::Confirm:
        m_pReceiver->answerPairing(true);
        break;
    case Item::Cancel:
        m_pReceiver->answerPairing(false);
        break;
    case Item::ForgetIt:
        m_pReceiver->forget(m_forgetPath);
        m_forgetPath.clear();
        m_forgetName.clear();
        focusFirst();
        m_viewSeen = view();
        break;
    case Item::Keep:
        m_forgetPath.clear();
        m_forgetName.clear();
        focusFirst();
        m_viewSeen = view();
        break;
    }
    update();
}

void WDeckBluetooth::askToForget(int row) {
    const QList<bluetooth::Device>& devices = m_pReceiver->facts().devices;
    if (row < 0 || row >= devices.size()) {
        return;
    }
    m_forgetPath = devices.at(row).path;
    m_forgetName = devices.at(row).name;
    focusFirst();
    m_viewSeen = view();
    update();
}

bool WDeckBluetooth::handleMove(int steps) {
    // Never wraps, as the deck's lists: the ends are where they look.
    const QList<Press> list = presses();
    if (list.isEmpty()) {
        return true;
    }
    const int at = qMax(0, static_cast<int>(list.indexOf(m_focusPress)));
    m_focusPress = list.at(qBound(0, at + steps, static_cast<int>(list.size()) - 1));
    update();
    return true;
}

bool WDeckBluetooth::handleSelect() {
    const QList<Press> list = presses();
    if (list.contains(m_focusPress)) {
        press(m_focusPress);
    }
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
        press(Press{Item::Keep});
        return true;
    case View::Devices:
        if (m_devicesAsked && !m_pReceiver->facts().connectedName.isEmpty()) {
            // The list over what plays: back to it.
            m_devicesAsked = false;
            focusFirst();
            m_viewSeen = view();
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
    // A row acts on release: held past the long press, it is forgotten
    // instead (asked first).
    const int pressed = rowAt(point);
    if (pressed >= 0) {
        m_pressRow = pressed;
        m_pressConsumed = false;
        m_focusPress = Press{Item::Row, pressed};
        m_longPressTimer.start();
        update();
        pEvent->accept();
        return;
    }
    // Everything else on the press, as the deck's buttons.
    for (const Press& listed : presses()) {
        if (listed.item != Item::Row && rectOf(listed).contains(point)) {
            m_focusPress = listed;
            press(listed);
            pEvent->accept();
            return;
        }
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
            press(Press{Item::Row, pressed});
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
        paintDevices(&painter, area);
        break;
    case View::ForgetConfirm:
        paintForgetConfirm(&painter, area);
        break;
    }
    paintPresses(&painter);
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
    // No cover from the device: the rune, which is what this page is.
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
        // Nothing to show yet: the device is the headline.
        y += 34;
        drawLine(pPainter,
                sized(font(), 32, true),
                kTitle,
                rect.left(),
                y,
                rect.width(),
                facts.connectedName);
        y += 40;
        drawLine(pPainter,
                sized(font(), 20),
                kArtist,
                rect.left(),
                y,
                rect.width(),
                tr("Play something on the device."));
    }

    // Where in the track: a bar, the time either side, half way down; the
    // device's buttons under it (paintPresses()).
    if (hasTrack) {
        const qint64 position = bluetooth::positionNow(facts, m_pReceiver->nowMs());
        const QFont timeFont = sized(font(), 20);
        const QFontMetrics metrics(timeFont);
        const QString elapsed = bluetooth::formatTime(position);
        const QString total = bluetooth::formatTime(
                facts.track.durationMs > 0 ? facts.track.durationMs : -1);
        const int baseline = rect.top() + rect.height() / 2;
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
    QString headline;
    if (!facts.devices.isEmpty()) {
        headline = tr("Pick a device");
    } else if (status == bluetooth::Status::Waiting) {
        headline = tr("No device paired yet");
    } else {
        headline = tr("Bluetooth receiver");
    }
    drawLine(pPainter,
            sized(font(), 32, true),
            kTitle,
            rect.left(),
            rect.top() + 34,
            rect.width() - kPairWidth - kIconGap,
            headline);

    if (facts.devices.isEmpty()) {
        QString hint;
        if (status == bluetooth::Status::Pairing) {
            hint = tr("On the device: Bluetooth, then %1.").arg(m_pReceiver->deckName());
        } else if (status == bluetooth::Status::Waiting) {
            hint = tr("Press +, then choose %1 on the device.").arg(m_pReceiver->deckName());
        }
        drawLine(pPainter,
                sized(font(), 20),
                kArtist,
                rect.left(),
                rect.top() + 74,
                rect.width(),
                hint);
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
                    .arg(facts.pairingName.isEmpty() ? tr("a device")
                                                     : facts.pairingName));
    y += 40;
    drawLine(pPainter,
            sized(font(), 20),
            kArtist,
            rect.left(),
            y,
            rect.width(),
            tr("Confirm if the device shows the same code."));
    const QFont codeFont = sized(font(), 88, true);
    pPainter->setFont(codeFont);
    pPainter->setPen(m_accent);
    pPainter->drawText(QRect(rect.left(), y + 20, rect.width(), rect.bottom() - y - 20),
            Qt::AlignLeft | Qt::AlignVCenter,
            bluetooth::formatPasskey(facts.passkey));
}

void WDeckBluetooth::paintPresses(QPainter* pPainter) const {
    const bluetooth::Facts& facts = m_pReceiver->facts();
    const QFont footFont = sized(font(), 22, true);
    const QFont nameFont = sized(font(), 22, true);
    const QFont detailFont = sized(font(), 20);
    for (const Press& listed : presses()) {
        const QRect rect = rectOf(listed);
        if (rect.isNull()) {
            continue; // a row scrolled out of view, and its star and bin
        }
        const bool focused = listed == m_focusPress;
        const QColor mark = focused ? QColor(Qt::black) : kTitle;
        const QPointF centre = QRectF(rect).center();
        const bluetooth::Device device = facts.devices.value(listed.row);
        switch (listed.item) {
        case Item::Row: {
            paintGround(pPainter, rect, focused, m_accent);
            // What the one picked is doing, at the row's end.
            QString detail;
            if (device.picked) {
                if (device.connected) {
                    detail = facts.streaming ? tr("Playing") : tr("Connected");
                } else if (!facts.connectingName.isEmpty()) {
                    detail = tr("Connecting…");
                }
            }
            const int detailWidth = detail.isEmpty()
                    ? 0
                    : QFontMetrics(detailFont).horizontalAdvance(detail) + 20;
            const int baseline = rect.top() + rect.height() / 2 + 8;
            drawLine(pPainter,
                    nameFont,
                    mark,
                    rect.left() + 20,
                    baseline,
                    rect.width() - 40 - detailWidth,
                    device.name);
            if (!detail.isEmpty()) {
                drawLine(pPainter,
                        detailFont,
                        focused ? QColor(Qt::black) : m_accent,
                        rect.right() - detailWidth,
                        baseline,
                        detailWidth,
                        detail);
            }
            break;
        }
        case Item::Star:
            paintGround(pPainter, rect, focused, m_accent);
            // The starred one in the accent, the others a quiet outline.
            paintStarMark(pPainter,
                    centre,
                    rect.height() * 0.5,
                    focused ? QColor(Qt::black) : (device.starred ? m_accent : kArtist),
                    device.starred);
            break;
        case Item::Bin:
            paintGround(pPainter, rect, focused, m_accent);
            paintBinMark(pPainter, centre, rect.height() * 0.42, focused ? mark : kArtist);
            break;
        case Item::Pair:
            paintGround(pPainter, rect, focused, m_accent);
            if (facts.discoverable && !focused) {
                // Visible to devices: the + in the deck's colour, ringed.
                QPainterPath path;
                path.addRoundedRect(QRectF(rect).adjusted(1, 1, -1, -1), 8, 8);
                pPainter->setBrush(Qt::NoBrush);
                pPainter->setPen(QPen(m_accent, 2));
                pPainter->drawPath(path);
            }
            paintPlusMark(pPainter,
                    centre,
                    rect.height() * 0.42,
                    focused ? mark : (facts.discoverable ? m_accent : kTitle));
            break;
        case Item::Previous:
            paintGround(pPainter, rect, focused, m_accent);
            paintSkipMark(pPainter, centre, rect.height() * 0.36, mark, false);
            break;
        case Item::PlayPause:
            paintGround(pPainter, rect, focused, m_accent);
            // What a press does next: pause while it plays.
            if (bluetooth::isPlaying(facts.playerStatus)) {
                paintPauseMark(pPainter, centre, rect.height() * 0.4, mark);
            } else {
                paintPlayMark(pPainter, centre, rect.height() * 0.4, mark);
            }
            break;
        case Item::Next:
            paintGround(pPainter, rect, focused, m_accent);
            paintSkipMark(pPainter, centre, rect.height() * 0.36, mark, true);
            break;
        default:
            paintGround(pPainter, rect, focused, m_accent);
            pPainter->setFont(footFont);
            pPainter->setPen(mark);
            pPainter->drawText(rect, Qt::AlignCenter, footText(listed.item));
            break;
        }
    }
}

} // namespace deck
} // namespace mixxx
