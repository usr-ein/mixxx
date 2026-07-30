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

/// The player number is drawn *over* the top row rather than beside it. In the
/// top bar there is no width to spare for a label column, and "which player"
/// is the only thing a label was telling us that the rows do not.

/// Beat marks are blocks rather than hairlines: this is read at a glance from
/// arm's length, and a one-pixel line disappears against a waveform.
constexpr int kDownbeatWidth = 9;
constexpr int kBeatWidth = 5;

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
    m_pPlay = std::make_unique<ControlProxy>(m_group, QStringLiteral("play"), this);
    m_pBpm = std::make_unique<ControlProxy>(m_group, QStringLiteral("bpm"), this);
    m_pFileBpm = std::make_unique<ControlProxy>(m_group, QStringLiteral("file_bpm"), this);
    m_pDuration = std::make_unique<ControlProxy>(m_group, QStringLiteral("duration"), this);
    m_pPlayPosition =
            std::make_unique<ControlProxy>(m_group, QStringLiteral("playposition"), this);

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
}

void WProLinkPhaseMeter::paintRow(QPainter* pPainter,
        const QRectF& rect,
        double phase,
        const QColor& colour,
        const QString& label) {
    const QRectF barRect = rect;

    // The baseline, always drawn, so an idle row still reads as a row.
    pPainter->setPen(QColor(0x33, 0x33, 0x33));
    pPainter->drawLine(QPointF(barRect.left(), barRect.center().y()),
            QPointF(barRect.right(), barRect.center().y()));

    if (phase < 0.0) {
        drawOverlayLabel(pPainter, barRect, label);
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
        pen.setWidth(isDownbeat ? kDownbeatWidth : kBeatWidth);
        pPainter->setPen(pen);
        // The downbeat is full height and the others half, so the bar can be
        // counted as well as the beat.
        const double inset = isDownbeat ? 0.0 : rect.height() * 0.28;
        pPainter->drawLine(QPointF(x, barRect.top() + inset),
                QPointF(x, barRect.bottom() - inset));
    }

    drawOverlayLabel(pPainter, barRect, label);
}

void WProLinkPhaseMeter::drawOverlayLabel(
        QPainter* pPainter, const QRectF& rect, const QString& label) {
    if (label.isEmpty()) {
        return;
    }
    // Over the ticks, at the left, with a slab of background behind it so a
    // tick passing underneath cannot make it unreadable.
    QFont small = font();
    small.setPixelSize(static_cast<int>(rect.height() * 0.62));
    small.setBold(true);
    pPainter->setFont(small);

    const QRectF textRect(rect.left() + 2, rect.top(), rect.width() - 4, rect.height());
    const QRectF box = pPainter->boundingRect(
            textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
    pPainter->fillRect(box.adjusted(-3, 0, 3, 0), QColor(0x0c, 0x0c, 0x0c));
    pPainter->setPen(QColor(0xdd, 0xdd, 0xdd));
    pPainter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
}

void WProLinkPhaseMeter::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(0x0c, 0x0c, 0x0c));

    const int masterDevice = static_cast<int>(m_pMasterDevice->get());
    const double masterPhase = m_pMasterBarPhase->get();

    // Our own bar phase, from the *playhead* rather than from counting beats.
    // The elapsed track time times the file's tempo is which beat we are on, so
    // a loop replays the same beats instead of marching on through the bar --
    // which is what a counter did, and why a one-beat loop used to walk the
    // marker around the whole bar while the audio repeated two beats.
    const double ourBeatDistance = m_pBeatDistance->get();
    const double fileBpm = m_pFileBpm->get();
    const double duration = m_pDuration->get();
    const bool ourTrackRunning = m_pBpm->get() > 0.0 && fileBpm > 0.0 && duration > 0.0;
    double ourPhase = -1.0;
    if (ourTrackRunning) {
        // Two sources, and they must not be allowed to disagree.
        //
        // `beat_distance` is the engine's own sub-beat phase and is exact. The
        // beat *index* has to come from the playhead, because nothing counts
        // bars. But the two are independent estimates of the same quantity:
        // the grid does not start at zero -- a real track's first beat is 23 ms
        // in -- so floor(beatsElapsed) flips to the next integer at a slightly
        // different instant than beat_distance wraps to 0. For that moment the
        // index says beat 3 while the fraction still says 0.98, the marker
        // leaps a whole beat forward, and a frame later it leaps back. That is
        // the jumping.
        //
        // Subtracting the fraction before rounding pins the index to whatever
        // beat_distance currently believes: the difference is near-integral by
        // construction, so the index can only change at the exact moment the
        // fraction wraps, and the two can no longer contradict each other.
        const double trackSeconds = m_pPlayPosition->get() * duration;
        const double beatsElapsed = trackSeconds * fileBpm / 60.0;
        const auto beatIndex =
                static_cast<long long>(std::llround(beatsElapsed - ourBeatDistance));
        const int beatInBar =
                static_cast<int>(((beatIndex % kBeatsPerBar) + kBeatsPerBar) %
                        kBeatsPerBar);
        ourPhase = (beatInBar + ourBeatDistance) / kBeatsPerBar;
    }

    // **Each row keeps its own colour, always.** Recolouring an aligned pair was
    // a distraction rather than information: the rows lining up already says
    // they are aligned, and a colour that changes underneath makes the meter
    // look like it is switching modes.

    const double margin = 4.0;
    const double rowHeight = (height() - 3 * margin) / 2.0;
    const QRectF masterRect(margin, margin, width() - 2 * margin, rowHeight);
    const QRectF ourRect(margin, margin * 2 + rowHeight, width() - 2 * margin, rowHeight);

    // Only the top row is labelled, with the number of the player it is
    // following. The bottom row is always this deck, so saying so costs width
    // and tells nobody anything.
    const QString masterLabel =
            masterDevice > 0 ? QString::number(masterDevice) : QStringLiteral("-");
    paintRow(&painter, masterRect, masterPhase, m_masterColour, masterLabel);
    paintRow(&painter, ourRect, ourPhase, m_ourColour, QString());

}
