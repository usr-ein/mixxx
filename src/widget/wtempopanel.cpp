#include "widget/wtempopanel.h"

#include <QFontMetrics>
#include <QPainter>
#include <QTimer>

#include "control/controlproxy.h"
#include "moc_wtempopanel.cpp"
#include "skin/legacy/skincontext.h"

namespace {
/// Ranges as fractions, and what each means on a deck:
///
///   up to  8%  the everyday beatmatching range
///   up to 16%  wide, still musical
///   up to 50%  far enough to hear the key move
///   beyond     effectively a tape stop
///
/// The thresholds sit between Mixxx's own selectable range presets, so a
/// boundary never falls halfway through a value the user can pick.
struct Tier {
    double upTo;
    const char* colour;
};
constexpr Tier kTiers[] = {
        {0.08, "#44ff88"},
        {0.16, "#ffdd33"},
        {0.50, "#ff9933"},
        {1e9, "#ff4444"},
};

constexpr int kRepaintMs = 100;
constexpr int kPadRight = 26;
constexpr int kPadBottom = 8;
/// Gap between the pitch column and the tempo digits.
constexpr int kColumnGap = 14;
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

    // Tempo, right-aligned against the padding.
    QFont tempoFont(m_tempoFamily);
    tempoFont.setPixelSize(m_tempoSize);
    painter.setFont(tempoFont);
    const QFontMetrics tempoMetrics(tempoFont);
    const QString tempoText = QStringLiteral("%1").arg(bpm, 0, 'f', 2);
    const int tempoWidth = tempoMetrics.horizontalAdvance(tempoText);
    const int tempoRight = width() - kPadRight;
    const int baseline = height() - kPadBottom - tempoMetrics.descent();

    painter.setPen(m_tempoColour);
    painter.drawText(tempoRight - tempoWidth, baseline, tempoText);

    // The pitch column, to its left: how far off the track's own tempo, over the
    // range the fader spans.
    QFont smallFont = font();
    smallFont.setPixelSize(19);
    painter.setFont(smallFont);
    const QFontMetrics smallMetrics(smallFont);
    const int columnRight = tempoRight - tempoWidth - kColumnGap;

    const double percent = (ratio - 1.0) * 100.0;
    const QString pitchText = QStringLiteral("%1%2%")
                                      .arg(percent >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
                                      .arg(std::abs(percent), 0, 'f', 2);
    painter.setPen(QColor(0xcc, 0xcc, 0xcc));
    painter.drawText(columnRight - smallMetrics.horizontalAdvance(pitchText),
            baseline - smallMetrics.height() - 2,
            pitchText);

    QFont rangeFont = font();
    rangeFont.setPixelSize(16);
    painter.setFont(rangeFont);
    const QFontMetrics rangeMetrics(rangeFont);
    const QString rangeText = range >= m_wideAbove
            ? tr("WIDE")
            : QStringLiteral("%1%").arg(range * 100.0, 0, 'g', 3);
    painter.setPen(rangeColour(range));
    painter.drawText(columnRight - rangeMetrics.horizontalAdvance(rangeText),
            baseline,
            rangeText);
}
