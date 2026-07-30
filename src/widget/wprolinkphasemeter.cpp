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
constexpr double kLabelWidth = 78.0;

/// Beat marks are blocks rather than hairlines: this is read at a glance from
/// arm's length, and a one-pixel line disappears against a waveform.
constexpr int kDownbeatWidth = 9;
constexpr int kBeatWidth = 5;

/// Frames the phase error must stay on one side of the threshold before the
/// in-sync colour follows it. Six at 30 Hz is a fifth of a second: long enough
/// that a deck hovering on the boundary does not flash, short enough to feel
/// immediate.
constexpr int kSyncLatchFrames = 6;

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
    readColour("InSyncColor", &m_inSyncColour);
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
        pen.setWidth(isDownbeat ? kDownbeatWidth : kBeatWidth);
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

    // Aligned when the *beat* phases agree. Comparing bar phases would demand
    // the bars line up too, which our arbitrary downbeat cannot promise.
    //
    // Latched rather than taken frame by frame: a deck matched by hand sits
    // right on the threshold, and recolouring the moment it crosses made the
    // whole meter flash. It has to hold for a few frames either way to count.
    if (masterPhase >= 0.0 && ourPhase >= 0.0) {
        const double masterBeatPhase = std::fmod(masterPhase * kBeatsPerBar, 1.0);
        const bool close =
                std::abs(wrapPhase(masterBeatPhase - ourBeatDistance)) < kInSyncTolerance;
        m_inSyncFrames = close ? m_inSyncFrames + 1 : 0;
        m_outOfSyncFrames = close ? 0 : m_outOfSyncFrames + 1;
        if (m_inSyncFrames >= kSyncLatchFrames) {
            m_inSync = true;
        } else if (m_outOfSyncFrames >= kSyncLatchFrames) {
            m_inSync = false;
        }
    } else {
        m_inSync = false;
        m_inSyncFrames = 0;
        m_outOfSyncFrames = 0;
    }
    const bool inSync = m_inSync;

    const QColor masterColour = inSync ? m_inSyncColour : m_masterColour;
    const QColor ourColour = inSync ? m_inSyncColour : m_ourColour;

    const double margin = 4.0;
    const double rowHeight = (height() - 3 * margin) / 2.0;
    const QRectF masterRect(margin, margin, width() - 2 * margin, rowHeight);
    const QRectF ourRect(margin, margin * 2 + rowHeight, width() - 2 * margin, rowHeight);

    const QString masterLabel = masterDevice > 0
            ? tr("Deck %1").arg(masterDevice)
            : tr("No master");
    paintRow(&painter, masterRect, masterPhase, masterColour, masterLabel);
    paintRow(&painter, ourRect, ourPhase, ourColour, tr("This Deck"));

}
