#include "widget/wtempopanel.h"

#include <QFontMetrics>
#include <QPainter>
#include <QTimer>

#include <cmath>

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
constexpr int kBoxPadY = 16;
constexpr int kBoxRadius = 4;

/// Anything below this is "the fader has not been heard from", which is not the
/// same as centre. The skin seeds the control at -2 for exactly this.
constexpr double kFaderUnknown = -1.5;

/// How close the fader has to get before it counts as having caught the tempo,
/// as a fraction of the fader's travel.
///
/// Mixxx's own soft-takeover threshold, which is what actually decides when the
/// fader takes over -- 3/128 of the parameter's range, and the parameter runs
/// 0..1 across a `rate` of -1..1, so it is twice that in rate units. Deriving
/// it rather than picking a number is what keeps the read-out honest: the
/// hardware tempo disappears at the moment the fader is really in control, not
/// a little before or after it.
constexpr double kTakeoverRate = 2.0 * 3.0 / 128.0;
} // namespace

WTempoPanel::WTempoPanel(QWidget* pParent, const QString& group)
        : WWidget(pParent) {
    m_pBpm = std::make_unique<ControlProxy>(group, QStringLiteral("bpm"), this);
    m_pRateRatio = std::make_unique<ControlProxy>(group, QStringLiteral("rate_ratio"), this);
    m_pRateRange = std::make_unique<ControlProxy>(group, QStringLiteral("rateRange"), this);
    m_pFileBpm = std::make_unique<ControlProxy>(group, QStringLiteral("file_bpm"), this);
    // Where the fader physically is, published by the mapping. The direction
    // preference is Mixxx's own and lives outside the deck's group, because it
    // is a preference about every deck rather than about this one.
    m_pTempoFader = std::make_unique<ControlProxy>(QStringLiteral("[TriMixxx]"),
            QStringLiteral("tempo_fader"),
            this,
            ControlFlag::NoWarnIfMissing);
    // Per DECK, not a global preference: `[ChannelN],rate_dir`, which is what
    // RateControl multiplies by. Reading it from `[Controls],RateDir` -- which
    // is where the *preference* lives -- resolved to nothing, fell back to +1,
    // and drew the fader running the opposite way to the tempo it was chasing.
    m_pRateDir = std::make_unique<ControlProxy>(group, QStringLiteral("rate_dir"), this);
    // Whether the fader is ours to use. See the read-out's own comment.
    m_pIsMaster = std::make_unique<ControlProxy>(QStringLiteral("[ProLink]"),
            QStringLiteral("is_master"),
            this,
            ControlFlag::NoWarnIfMissing);

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

    // What the fader is asking for, while it is not yet the thing being obeyed.
    //
    // Drawn only when the two differ by more than soft-takeover's own
    // threshold, which is precisely "the fader has not caught it yet": at the
    // moment it does, the number it was aiming at becomes the number already on
    // screen and a second copy of it would be noise.
    // **Only while this deck holds tempo master**, which is the only time the
    // fader is a thing the DJ can act with.
    //
    // Following a master, the fader is disconnected because the tempo is coming
    // off the wire, and it will still be disconnected when the master next
    // changes it -- so where it happens to be sitting is not a target, it is
    // noise. The moment mastership is taken it becomes the opposite: the tempo
    // is this deck's to set, the fader is the only thing that can set it, and
    // soft-takeover is standing between the two until it is met. That is the
    // gap this number exists to close, and it does not exist anywhere else.
    QString faderText;
    const double faderRate = m_pTempoFader->get();
    const double fileBpm = m_pFileBpm->get();
    const bool ourFader = m_pIsMaster->valid() && m_pIsMaster->get() > 0.0;
    if (ourFader && faderRate > kFaderUnknown && fileBpm > 0.0) {
        // Mixxx's own arithmetic, with Mixxx's own direction preference, so the
        // number the fader is aiming at is the number it will produce.
        const double dir = m_pRateDir->valid() ? m_pRateDir->get() : 1.0;
        const double faderBpm = fileBpm * (1.0 + faderRate * range * dir);
        // In BPM, from a threshold expressed in fader travel: the same distance
        // means different tempos at ±6% and at WIDE, and it is the tempo the
        // read-out is in.
        const double caught = fileBpm * range * kTakeoverRate;
        if (std::abs(faderBpm - bpm) > caught) {
            faderText = QStringLiteral("%1").arg(faderBpm, 0, 'f', 2);
        }
    }

    QFont faderFont(m_tempoFamily);
    faderFont.setPixelSize(m_tempoSize * 3 / 5);
    const QFontMetrics faderMetrics(faderFont);
    const int faderWidth =
            faderText.isEmpty() ? 0 : faderMetrics.horizontalAdvance(faderText) + kColumnGap;

    const int columnWidth = std::max(smallMetrics.horizontalAdvance(pitchText),
            rangeMetrics.horizontalAdvance(rangeText));
    const int contentWidth = columnWidth + kColumnGap + faderWidth + tempoWidth;
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
    const int columnRight = tempoRight - tempoWidth - faderWidth - kColumnGap;

    painter.setFont(tempoFont);
    painter.setPen(m_tempoColour);
    painter.drawText(tempoRight - tempoWidth, baseline, tempoText);

    // Smaller and dimmer than the tempo, and to its left: it is where the fader
    // is, not where the deck is, and nothing should be able to mistake the two
    // at a glance in a dark booth.
    if (!faderText.isEmpty()) {
        painter.setFont(faderFont);
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.drawText(tempoRight - tempoWidth - faderWidth, baseline, faderText);
    }

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
