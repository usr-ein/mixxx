#include "widget/deck/wdeckfxstrip.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>

#include "control/controlproxy.h"
#include "moc_wdeckfxstrip.cpp"
#include "widget/deck/deckfxcontrols.h"
#include "widget/deck/wdeckrack.h"

namespace {
const QString kUnit = QStringLiteral("[EffectRack1_EffectUnit2]");
const QString kTri = QStringLiteral("[TriMixxx]");
} // namespace

namespace mixxx {
namespace deck {

WDeckFxStrip::WDeckFxStrip(QWidget* pParent)
        : QWidget(pParent), WBaseWidget(this) {
    setObjectName(QStringLiteral("DeckFxStrip"));
    setAutoFillBackground(true);

    m_pOutputLevel = std::make_unique<ControlProxy>(kUnit, QStringLiteral("output_level"));
    m_pMakeup = std::make_unique<ControlProxy>(kUnit, QStringLiteral("makeup"));
    m_pAuxPregain = std::make_unique<ControlProxy>(
            QStringLiteral("[Auxiliary1]"), QStringLiteral("pregain"));
    m_pRingOut = std::make_unique<ControlProxy>(kTri, QStringLiteral("fx_ring_out"));
    m_pMove = std::make_unique<ControlProxy>(kTri, QStringLiteral("fx_move"));
    m_pSelect = std::make_unique<ControlProxy>(kTri, QStringLiteral("fx_select"));

    // The encoder arrives through the mapping, which only routes here while
    // `fx_focus` is set -- so the strip does not have to fight the browser for
    // it, and the browser does not have to know the strip exists.
    m_pMove->connectValueChanged(this, [this](double steps) {
        if (!m_focused || steps == 0.0) {
            return;
        }
        ControlProxy* pMaster = masterControl();
        if (!pMaster || !pMaster->valid()) {
            return;
        }
        const double base = m_mutedLevel >= 0.0 ? m_mutedLevel : pMaster->getParameter();
        const double next = qBound(0.0, base + steps * 0.028, 1.0);
        if (m_mutedLevel >= 0.0) {
            m_mutedLevel = next;
        } else {
            pMaster->setParameter(next);
        }
        update();
    });
    m_pSelect->connectValueChanged(this, [this](double v) {
        if (!m_focused || v <= 0.0) {
            return;
        }
        ControlProxy* pMaster = masterControl();
        if (!pMaster || !pMaster->valid()) {
            return;
        }
        if (m_mutedLevel >= 0.0) {
            pMaster->setParameter(m_mutedLevel);
            ControlProxy* pIdle = idleControl();
            if (pIdle && pIdle->valid()) {
                pIdle->setParameter(0.5);
            }
            m_mutedLevel = -1.0;
        } else {
            m_mutedLevel = pMaster->getParameter();
            pMaster->setParameter(0.0);
        }
        update();
    });

    // The VU has to move without anyone touching anything, so unlike the rack
    // this repaints whether or not it is being used. It is 76 px wide.
    m_refresh.setInterval(100);
    connect(&m_refresh, &QTimer::timeout, this, [this] { update(); });
    m_refresh.start();
}

void WDeckFxStrip::setup(const QDomNode& node, const SkinContext& context) {
    Q_UNUSED(node);
    Q_UNUSED(context);
    // Nothing skinnable yet: the strip is drawn rather than styled, like the
    // rack it belongs to.
}

WDeckFxStrip::~WDeckFxStrip() {
    if (DeckFxControls::instance()) {
        DeckFxControls::instance()->focus()->forceSet(0.0);
    }
}

bool WDeckFxStrip::ringOut() const {
    return !m_pRingOut || !m_pRingOut->valid() || m_pRingOut->toBool();
}

ControlProxy* WDeckFxStrip::masterControl() const {
    return ringOut() ? m_pAuxPregain.get() : m_pMakeup.get();
}

ControlProxy* WDeckFxStrip::idleControl() const {
    return ringOut() ? m_pMakeup.get() : m_pAuxPregain.get();
}

void WDeckFxStrip::setRingOut(bool wantRingOut) {
    if (wantRingOut == ringOut()) {
        return;
    }
    // Identical to the rack's rule, and it has to be: they are the same switch
    // seen twice. Carry the level across, and while muted take BOTH stages to
    // zero rather than opening the arriving one -- see WDeckRack::setRingOut.
    ControlProxy* pWas = masterControl();
    double level = 0.5;
    if (m_mutedLevel >= 0.0) {
        level = m_mutedLevel;
    } else if (pWas && pWas->valid()) {
        level = pWas->getParameter();
    }
    m_pRingOut->set(wantRingOut ? 1.0 : 0.0);
    ControlProxy* pNow = masterControl();
    ControlProxy* pIdle = idleControl();
    if (m_mutedLevel >= 0.0) {
        if (pNow && pNow->valid()) {
            pNow->setParameter(0.0);
        }
        if (pIdle && pIdle->valid()) {
            pIdle->setParameter(0.0);
        }
    } else {
        if (pNow && pNow->valid()) {
            pNow->setParameter(level);
        }
        if (pIdle && pIdle->valid()) {
            pIdle->setParameter(0.5);
        }
    }
}

void WDeckFxStrip::setFocused(bool focused) {
    if (m_focused == focused) {
        return;
    }
    m_focused = focused;
    // forceSet, and written on the ControlObject rather than through a proxy.
    // The control is read-only so that no mapping can steer the encoder
    // somewhere nothing is lit -- and setReadOnly() enforces that with a
    // change-request handler that drops the value, which set() goes through.
    // A proxy would therefore have written nothing at all, silently, and the
    // border would have lit while the encoder stayed with the browser.
    if (DeckFxControls::instance()) {
        DeckFxControls::instance()->focus()->forceSet(focused ? 1.0 : 0.0);
    }
    if (focused) {
        // On qApp, not on window(). The waveform is a native OpenGLWindow, and
        // events delivered to a native child window never pass through the
        // parent widget's filter -- so a tap on the waveform, which is the
        // most likely place a DJ looks next, did not release focus at all.
        qApp->installEventFilter(this);
    } else {
        qApp->removeEventFilter(this);
    }
    update();
}

bool WDeckFxStrip::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (m_focused && pEvent->type() == QEvent::MouseButtonPress) {
        auto* pMouse = static_cast<QMouseEvent*>(pEvent);
        const QPoint local = mapFromGlobal(pMouse->globalPosition().toPoint());
        if (!rect().contains(local)) {
            setFocused(false);
        }
    }
    // Never consumed: this only watches. Whatever was touched still gets it.
    return QWidget::eventFilter(pObject, pEvent);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

QRect WDeckFxStrip::knobRect() const {
    const int size = qMin(width() - 18, 58);
    return QRect((width() - size) / 2, height() / 2 - size / 2 - 20, size, size);
}

QRect WDeckFxStrip::vuRect() const {
    return QRect(width() / 2 - 9, 34, 18, height() / 2 - 84);
}

QRect WDeckFxStrip::rockerRect() const {
    return QRect(6, height() - 58, width() - 12, 44);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void WDeckFxStrip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(0x0C, 0x0C, 0x0C));

    WDeckRack::paintChrome(&painter, rect(), WDeckRack::Material::BrushedSteel);

    const QColor ink = WDeckRack::materialInk(0);
    QFont font = painter.font();
    font.setPixelSize(13);
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    painter.setFont(font);
    WDeckRack::paintEngraved(&painter,
            QRect(0, 10, width(), 18),
            Qt::AlignCenter,
            tr("FX"),
            ink);
    font.setLetterSpacing(QFont::PercentageSpacing, 100.0);

    WDeckRack::paintVu(&painter,
            vuRect(),
            m_pOutputLevel && m_pOutputLevel->valid() ? m_pOutputLevel->get() : 0.0,
            true);

    const bool muted = m_mutedLevel >= 0.0;
    ControlProxy* pMaster = masterControl();
    const double level = muted
            ? m_mutedLevel
            : (pMaster && pMaster->valid() ? pMaster->getParameter() : 0.5);
    WDeckRack::paintKnob(&painter,
            knobRect(),
            level,
            muted ? tr("MUTE") : (ringOut() ? tr("SEND") : tr("RET")),
            WDeckRack::Material::BrushedSteel,
            false,
            muted,
            m_focused);

    font.setPixelSize(15);
    font.setBold(true);
    painter.setFont(font);
    WDeckRack::paintEngraved(&painter,
            QRect(0, knobRect().bottom() + 22, width(), 18),
            Qt::AlignCenter,
            level <= 0.0 ? QStringLiteral("−∞")
                         : QStringLiteral("%1").arg(level * 24.0 - 12.0, 0, 'f', 1),
            muted ? QColor(0x6A, 0x70, 0x7B) : ink);

    // The rocker, stacked rather than side by side: 76 px will not take two
    // legible words in a row.
    const QRect rocker = rockerRect();
    painter.fillRect(rocker, QColor(0, 0, 0, 90));
    WDeckRack::paintBevel(&painter, rocker, false);
    font.setPixelSize(11);
    for (int i = 0; i < 2; ++i) {
        const QRect half(rocker.left() + 2,
                rocker.top() + 2 + i * (rocker.height() - 4) / 2,
                rocker.width() - 4,
                (rocker.height() - 4) / 2);
        const bool on = (i == 0) == ringOut();
        painter.fillRect(half, on ? QColor(0x3A, 0x4A, 0x22) : QColor(0x1A, 0x1C, 0x1F));
        WDeckRack::paintBevel(&painter, half, on);
        painter.setPen(on ? QColor(0xAE, 0xE8, 0x4A) : QColor(0x77, 0x7D, 0x86));
        painter.drawText(half, Qt::AlignCenter, i == 0 ? tr("RING") : tr("CUT"));
    }

    if (m_focused) {
        // The border says the encoder is here. Without it the wheel would do
        // something different depending on history, with nothing on screen
        // admitting to it.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0x88, 0xFF, 0x00), 2));
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

void WDeckFxStrip::mousePressEvent(QMouseEvent* pEvent) {
    const QPoint point = pEvent->pos();
    setFocused(true);

    if (rockerRect().contains(point)) {
        setRingOut(point.y() < rockerRect().center().y());
        update();
        return;
    }
    if (knobRect().adjusted(-14, -12, 14, 26).contains(point)) {
        ControlProxy* pMaster = masterControl();
        m_dragStartValue = m_mutedLevel >= 0.0
                ? m_mutedLevel
                : (pMaster && pMaster->valid() ? pMaster->getParameter() : 0.5);
        m_dragStart = point;
        m_draggingKnob = true;
    }
    update();
}

void WDeckFxStrip::mouseMoveEvent(QMouseEvent* pEvent) {
    if (!m_draggingKnob) {
        return;
    }
    const double delta = (m_dragStart.y() - pEvent->pos().y()) / 160.0;
    const double next = qBound(0.0, m_dragStartValue + delta, 1.0);
    if (m_mutedLevel >= 0.0) {
        m_mutedLevel = next;
    } else {
        ControlProxy* pMaster = masterControl();
        if (pMaster && pMaster->valid()) {
            pMaster->setParameter(next);
        }
    }
    update();
}

void WDeckFxStrip::mouseReleaseEvent(QMouseEvent*) {
    m_draggingKnob = false;
}

} // namespace deck
} // namespace mixxx
