#include "widget/wprolinkphasemeter.h"

#include <QPainter>
#include <QTimer>

#include <cmath>

#include "control/controlproxy.h"
#include "moc_wprolinkphasemeter.cpp"
#include "skin/legacy/skincontext.h"

namespace {
/// Beats per bar. Four everywhere in this protocol: rekordbox numbers beats 1-4
/// and the beat packet's bar field is 1-4.
constexpr int kBeatsPerBar = 4;

/// How often the meter repaints. The master's phase is republished at 30 Hz, so
/// matching that is as smooth as the data gets.
constexpr int kRepaintIntervalMs = 33;

/// Within this fraction of a beat, the two are called aligned and both rows go
/// green. A twentieth of a beat is about 20 ms at 130 BPM -- comfortably below
/// what anyone hears as flam, and tight enough that it means something.
constexpr double kInSyncTolerance = 0.05;

/// Width of the label column. The ticks start after it, so text and beat marks
/// never share a pixel.
constexpr double kLabelWidth = 190.0;

/// Phase difference wrapped to -0.5..0.5, so being a hair *behind* the downbeat
/// reads as a small negative rather than as almost a whole beat ahead.
double wrapPhase(double difference) {
    difference = std::fmod(difference, 1.0);
    if (difference > 0.5) {
        difference -= 1.0;
    } else if (difference < -0.5) {
        difference += 1.0;
    }
    return difference;
}
} // namespace

WProLinkPhaseMeter::WProLinkPhaseMeter(QWidget* pParent, const QString& group)
        : WWidget(pParent), m_group(group) {
    m_pMasterDevice = std::make_unique<ControlProxy>(
            QStringLiteral("[ProLink]"), QStringLiteral("master_device"), this);
    m_pMasterBarPhase = std::make_unique<ControlProxy>(
            QStringLiteral("[ProLink]"), QStringLiteral("master_bar_phase"), this);
    m_pMasterBpm = std::make_unique<ControlProxy>(
            QStringLiteral("[ProLink]"), QStringLiteral("master_bpm"), this);
    m_pBeatDistance =
            std::make_unique<ControlProxy>(m_group, QStringLiteral("beat_distance"), this);
    m_pBeatActive =
            std::make_unique<ControlProxy>(m_group, QStringLiteral("beat_active"), this);
    m_pPlay = std::make_unique<ControlProxy>(m_group, QStringLiteral("play"), this);
    m_pBpm = std::make_unique<ControlProxy>(m_group, QStringLiteral("bpm"), this);

    m_pBeatActive->connectValueChanged(this, &WProLinkPhaseMeter::onBeatActive);

    // Polled rather than driven by valueChanged: the master's phase moves
    // continuously and the marker has to move with it, so there is a repaint
    // every frame regardless of whether any single control changed.
    auto* pTimer = new QTimer(this);
    connect(pTimer, &QTimer::timeout, this, [this]() { update(); });
    pTimer->start(kRepaintIntervalMs);
}

WProLinkPhaseMeter::~WProLinkPhaseMeter() = default;

void WProLinkPhaseMeter::setup(const QDomNode& node, const SkinContext& context) {
    // hasNodeSelectString takes a QString, so the colours are read as text and
    // parsed here rather than through a QColor overload that does not exist.
    const auto readColour = [&](const char* name, QColor* pColour) {
        QString text;
        if (context.hasNodeSelectString(node, QString::fromLatin1(name), &text)) {
            const QColor parsed(text);
            if (parsed.isValid()) {
                *pColour = parsed;
            }
        }
    };
    readColour("MasterColor", &m_masterColour);
    readColour("DeckColor", &m_ourColour);
    readColour("InSyncColor", &m_inSyncColour);
}

void WProLinkPhaseMeter::onBeatActive(double value) {
    // beat_active pulses on each beat. Counting them is the only bar reference
    // available on this side; it is arbitrary in absolute terms, which is why
    // the header says bar alignment is a convenience and beat alignment is not.
    if (value > 0.0) {
        m_ourBeatInBar = (m_ourBeatInBar + 1) % kBeatsPerBar;
    }
}

void WProLinkPhaseMeter::paintRow(QPainter* pPainter,
        const QRectF& rect,
        double phase,
        const QColor& colour,
        const QString& label) {
    // The label sits in its own column so nothing ever overlaps the ticks --
    // this is a thing to be read at a glance from arm's length, and a beat tick
    // hidden behind text is worse than no tick.
    const QRectF labelRect(rect.left(), rect.top(), kLabelWidth, rect.height());
    const QRectF barRect(rect.left() + kLabelWidth,
            rect.top(),
            rect.width() - kLabelWidth,
            rect.height());

    pPainter->setPen(QColor(0x99, 0x99, 0x99));
    pPainter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    // The baseline, always drawn, so an idle row still reads as a row.
    pPainter->setPen(QColor(0x33, 0x33, 0x33));
    pPainter->drawLine(QPointF(barRect.left(), barRect.center().y()),
            QPointF(barRect.right(), barRect.center().y()));

    if (phase < 0.0) {
        return;
    }

    // **The ticks move; the frame does not.** Each tick is a beat, and its
    // position slides left as the beat progresses. Two rows in phase put their
    // ticks in the same columns, and any offset between the decks is the
    // horizontal gap between the two rows' ticks -- which is the whole point of
    // the meter, and reads without having to compare two numbers.
    const double cellWidth = barRect.width() / kBeatsPerBar;
    const double barPhase = phase * kBeatsPerBar; // in beats, 0..4

    for (int i = -1; i <= kBeatsPerBar + 1; ++i) {
        const double beatsAway = i - std::fmod(barPhase, 1.0);
        const double x = barRect.left() + beatsAway * cellWidth;
        if (x < barRect.left() - 1 || x > barRect.right() + 1) {
            continue;
        }
        // Which beat of the bar this tick is, so the downbeat can be marked.
        const int beatIndex =
                (static_cast<int>(std::floor(barPhase)) + i + kBeatsPerBar * 4) %
                kBeatsPerBar;
        const bool isDownbeat = beatIndex == 0;

        QPen pen(colour);
        pen.setWidth(isDownbeat ? 4 : 2);
        pPainter->setPen(pen);
        // The downbeat is full height and the others half, so the bar can be
        // counted as well as the beat.
        const double inset = isDownbeat ? 0.0 : rect.height() * 0.28;
        pPainter->drawLine(QPointF(x, barRect.top() + inset),
                QPointF(x, barRect.bottom() - inset));
    }
}

void WProLinkPhaseMeter::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(0x0c, 0x0c, 0x0c));

    const int masterDevice = static_cast<int>(m_pMasterDevice->get());
    const double masterPhase = m_pMasterBarPhase->get();
    const double masterBpm = m_pMasterBpm->get();

    // Our own bar phase: exact within the beat, arbitrary across the bar.
    const double ourBeatDistance = m_pBeatDistance->get();
    const bool ourTrackRunning = m_pBpm->get() > 0.0;
    const double ourPhase = ourTrackRunning
            ? (m_ourBeatInBar + ourBeatDistance) / kBeatsPerBar
            : -1.0;

    // Aligned when the *beat* phases agree. Comparing bar phases would demand
    // the bars line up too, which our locally counted bar cannot promise.
    bool inSync = false;
    if (masterPhase >= 0.0 && ourPhase >= 0.0) {
        const double masterBeatPhase = std::fmod(masterPhase * kBeatsPerBar, 1.0);
        inSync = std::abs(wrapPhase(masterBeatPhase - ourBeatDistance)) < kInSyncTolerance;
    }

    const QColor masterColour = inSync ? m_inSyncColour : m_masterColour;
    const QColor ourColour = inSync ? m_inSyncColour : m_ourColour;

    const double margin = 4.0;
    const double rowHeight = (height() - 3 * margin) / 2.0;
    const QRectF masterRect(margin, margin, width() - 2 * margin, rowHeight);
    const QRectF ourRect(margin, margin * 2 + rowHeight, width() - 2 * margin, rowHeight);

    const QString masterLabel = masterDevice > 0
            ? QStringLiteral("LINK %1  %2").arg(masterDevice).arg(masterBpm, 0, 'f', 2)
            : tr("NO LINK MASTER");
    paintRow(&painter, masterRect, masterPhase, masterColour, masterLabel);
    paintRow(&painter,
            ourRect,
            ourPhase,
            ourColour,
            QStringLiteral("DECK  %1").arg(m_pBpm->get(), 0, 'f', 2));

    // The offset in milliseconds, which is what a DJ nudging a jog wheel is
    // actually chasing. Only shown when there are two things to compare.
    if (masterPhase >= 0.0 && ourPhase >= 0.0 && masterBpm > 0.0) {
        const double masterBeatPhase = std::fmod(masterPhase * kBeatsPerBar, 1.0);
        const double errorBeats = wrapPhase(ourBeatDistance - masterBeatPhase);
        const double errorMs = errorBeats * 60000.0 / masterBpm;
        painter.setPen(inSync ? m_inSyncColour : QColor(0xcc, 0xcc, 0xcc));
        painter.drawText(QRectF(0, 0, width() - 6, height()),
                Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("%1%2 ms")
                        .arg(errorMs >= 0 ? QStringLiteral("+") : QString())
                        .arg(errorMs, 0, 'f', 0));
    }
}
