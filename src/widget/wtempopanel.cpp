#include "widget/wtempopanel.h"

#include <QFontMetrics>
#include <QPainter>
#include <QTimer>

#include "control/controlproxy.h"
#include "moc_wtempopanel.cpp"
#include "skin/legacy/skincontext.h"

namespace {
/// Ranges and their colours, matching the deck's own A1 pad LED.
///
/// The controller script cycles the pitch range through 6, 10, 16 and 100 per
/// cent and lights that pad green, yellow, orange, red. Reusing the same four
/// steps means the number on screen and the light under the DJ's finger always
/// agree; inventing a second set of thresholds would put them out of step at
/// exactly the values the hardware can select.
/// See mixxx_config/TriMixxx.scripts.js, RATE_RANGES and RANGE_LED.
struct Tier {
    double upTo;
    const char* colour;
};
constexpr Tier kTiers[] = {
        {0.06, "#33dd55"},
        {0.10, "#ffd11a"},
        {0.16, "#ff8822"},
        {1e9, "#ff3b30"},
};

constexpr int kRepaintMs = 100;
/// Clear of the corner. The panel's own bezel padding is below this, and the
/// box still wants air around it or it reads as stuck to the edge.
constexpr int kPadRight = 44;
constexpr int kPadBottom = 26;
/// Gap between the pitch column and the tempo digits.
constexpr int kColumnGap = 14;
/// Inside the box, between its border and the text.
constexpr int kBoxPadX = 14;
constexpr int kBoxPadY = 8;
constexpr int kBoxRadius = 4;
} // namespace

WTempoPanel::WTempoPanel(QWidget* pParent, const QString& group)
        : WWidget(pParent) {
    m_pBpm = std::make_unique<ControlProxy>(group, QStringLiteral("bpm"), this);
    m_pRateRatio = std::make_unique<ControlProxy>(group, QStringLiteral("rate_ratio"), this);
    m_pRateRange = std::make_unique<ControlProxy>(group, QStringLiteral("rateRange"), this);

    // Polled: three controls, and a repaint ten times a second is cheaper than
    // wiring three valueChanged signals to the same update().
    auto* pTimer = new QTimer(this);
    connect(pTimer, &QTimer::timeout, this, [this]() {
        // Raised on every tick, not just once: the waveform beneath repaints
        // continuously and anything that re-raises it would otherwise bury this
        // permanently, with no event to notice it by.
        if (!isVisible()) {
            return;
        }
        raise();
        update();
    });
    pTimer->start(kRepaintMs);

    // Clicks belong to the waveform underneath; this is a read-out, not a
    // control, and swallowing a seek would be a real regression.
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void WTempoPanel::showEvent(QShowEvent* pEvent) {
    WWidget::showEvent(pEvent);
    raise();
}

void WTempoPanel::setup(const QDomNode& node, const SkinContext& context) {
    QString text;
    if (context.hasNodeSelectString(node, QStringLiteral("TempoFont"), &text)) {
        m_tempoFamily = text;
    }
    if (context.hasNodeSelectString(node, QStringLiteral("TempoSize"), &text)) {
        bool ok = false;
        const int value = text.toInt(&ok);
        if (ok) {
            m_tempoSize = value;
        }
    }
    if (context.hasNodeSelectString(node, QStringLiteral("TempoColor"), &text)) {
        const QColor colour(text);
        if (colour.isValid()) {
            m_tempoColour = colour;
        }
    }
}

QColor WTempoPanel::rangeColour(double range) const {
    for (const Tier& tier : kTiers) {
        if (range <= tier.upTo) {
            return QColor(QString::fromLatin1(tier.colour));
        }
    }
    return QColor(Qt::white);
}

void WTempoPanel::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const double bpm = m_pBpm->get();
    const double ratio = m_pRateRatio->get();
    const double range = m_pRateRange->get();

    // Everything is measured first, then a box is drawn behind it, then the
    // text goes on top. Measuring twice is cheaper than guessing at a size that
    // would have to be kept in step with three fonts by hand.
    QFont tempoFont(m_tempoFamily);
    tempoFont.setPixelSize(m_tempoSize);
    const QFontMetrics tempoMetrics(tempoFont);
    const QString tempoText = QStringLiteral("%1").arg(bpm, 0, 'f', 2);
    const int tempoWidth = tempoMetrics.horizontalAdvance(tempoText);

    QFont smallFont = font();
    smallFont.setPixelSize(19);
    const QFontMetrics smallMetrics(smallFont);
    QFont rangeFont = font();
    rangeFont.setPixelSize(16);
    const QFontMetrics rangeMetrics(rangeFont);

    const double percent = (ratio - 1.0) * 100.0;
    const QString pitchText = QStringLiteral("%1%2%")
                                      .arg(percent >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
                                      .arg(std::abs(percent), 0, 'f', 2);
    const QString rangeText = range >= m_wideAbove
            ? tr("WIDE")
            : QStringLiteral("%1%").arg(range * 100.0, 0, 'g', 3);

    const int columnWidth = std::max(smallMetrics.horizontalAdvance(pitchText),
            rangeMetrics.horizontalAdvance(rangeText));
    const int contentWidth = columnWidth + kColumnGap + tempoWidth;
    const int contentHeight = tempoMetrics.height();

    const int right = width() - kPadRight;
    const int bottom = height() - kPadBottom;
    const QRect box(right - contentWidth - kBoxPadX,
            bottom - contentHeight - kBoxPadY,
            contentWidth + 2 * kBoxPadX,
            contentHeight + 2 * kBoxPadY);

    // The box: solid black so the numbers are legible over any part of the
    // waveform, with a faint border to separate it from a dark passage where the
    // fill alone would be invisible.
    painter.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
    painter.setBrush(QColor(0x00, 0x00, 0x00));
    painter.drawRoundedRect(box, kBoxRadius, kBoxRadius);

    const int baseline = box.bottom() - kBoxPadY - tempoMetrics.descent();
    const int tempoRight = box.right() - kBoxPadX;
    const int columnRight = tempoRight - tempoWidth - kColumnGap;

    painter.setFont(tempoFont);
    painter.setPen(m_tempoColour);
    painter.drawText(tempoRight - tempoWidth, baseline, tempoText);

    painter.setFont(smallFont);
    painter.setPen(QColor(0xcc, 0xcc, 0xcc));
    painter.drawText(columnRight - smallMetrics.horizontalAdvance(pitchText),
            baseline - smallMetrics.height() - 2,
            pitchText);

    painter.setFont(rangeFont);
    painter.setPen(rangeColour(range));
    painter.drawText(columnRight - rangeMetrics.horizontalAdvance(rangeText),
            baseline,
            rangeText);
}
