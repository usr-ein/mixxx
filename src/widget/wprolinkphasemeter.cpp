#include "widget/wprolinkphasemeter.h"

#include <QPainter>
#include <QTimer>

#include <cmath>

#include "control/controlproxy.h"
#include "moc_wprolinkphasemeter.cpp"
#include "network/prolink/audiblebeatclock.h"
#include "network/prolink/prolinkbeatposition.h"
#include "skin/legacy/skincontext.h"

namespace {
/// Beats per bar. Four everywhere in this protocol: rekordbox numbers beats 1-4
/// and the beat packet's bar field is 1-4.
constexpr int kBeatsPerBar = mixxx::prolink::kBeatsPerBar;

/// How often the meter repaints when nothing is being published: as often as
/// the service polls, which is as smooth as the data gets.
constexpr int kRepaintIntervalMs = 33;

/// Beat marks are blocks rather than hairlines: this is read at a glance from
/// arm's length, and a one-pixel line disappears against a waveform.
constexpr int kDownbeatWidth = 9;
constexpr int kBeatWidth = 5;

} // namespace

WProLinkPhaseMeter::WProLinkPhaseMeter(QWidget* pParent, const QString& group)
        : WWidget(pParent), m_group(group) {
    const auto proLink = [this](const char* item) {
        return std::make_unique<ControlProxy>(
                QStringLiteral("[ProLink]"), QString::fromLatin1(item), this);
    };
    m_pMasterDevice = proLink("master_device");
    m_pMasterBarPhase = proLink("master_bar_phase");
    m_pMeterIsMaster = proLink("meter_is_master");
    m_pMeterLive = proLink("meter_live");
    m_pWeAreMaster = proLink("is_master");
    m_pOurBeat = std::make_unique<mixxx::prolink::AudibleBeatClock>(m_group, this);

    // **Repainted when the other deck's phase is published**, so the two rows
    // are read a moment apart rather than up to a whole poll apart. The meter
    // used to repaint on its own 33 ms timer, beside the service's own 33 ms
    // poll: two clocks at a fixed, random offset for the session, and the top
    // row drawn up to 33 ms late -- up to 7% of a beat at 128 BPM, enough to
    // show a lined-up pair as us ahead.
    m_pMasterBarPhase->connectValueChanged(this, [this](double) {
        m_lastPublished.start();
        update();
    });
    // The timer only covers a top row standing still -- a paused deck held at
    // its beat, which is not republished -- while our own row moves; and
    // nothing at all while there is nobody to follow.
    auto* pTimer = new QTimer(this);
    connect(pTimer, &QTimer::timeout, this, [this]() {
        if (m_idle && m_pMasterBarPhase->get() < 0.0) {
            return;
        }
        if (m_lastPublished.isValid() && m_lastPublished.elapsed() < 2 * kRepaintIntervalMs) {
            return;
        }
        update();
    });
    pTimer->start(kRepaintIntervalMs);
    // Every pixel is painted, starting with the background, so Qt need not
    // repaint whatever is behind it first.
    setAttribute(Qt::WA_OpaquePaintEvent);
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
    // Over the ticks, at the right end, with a slab of background behind it
    // so a tick passing underneath cannot make it unreadable. Not the left:
    // that is where every tick lands on its beat, the one place the two rows
    // most need comparing.
    QFont small = font();
    small.setPixelSize(static_cast<int>(rect.height() * 0.62));
    small.setBold(true);
    pPainter->setFont(small);

    const QRectF textRect(rect.left() + 2, rect.top(), rect.width() - 4, rect.height());
    const QRectF box = pPainter->boundingRect(
            textRect, Qt::AlignRight | Qt::AlignVCenter, label);
    pPainter->fillRect(box.adjusted(-3, 0, 3, 0), QColor(0x0c, 0x0c, 0x0c));
    pPainter->setPen(QColor(0xdd, 0xdd, 0xdd));
    pPainter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, label);
}

void WProLinkPhaseMeter::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(0x0c, 0x0c, 0x0c));

    // -1 when there is nobody to follow. A proxy that never resolved reads
    // 0.0, which is a real phase, so it must not be drawn as one.
    const double masterPhase =
            m_pMasterBarPhase->valid() ? m_pMasterBarPhase->get() : -1.0;
    m_idle = masterPhase < 0.0;

    // Our own bar phase as it is *heard*, through the same clock SYNC and the
    // network publisher use -- so the row drawn here, the error SYNC corrects
    // and the bar a CDJ is told we are in cannot drift apart, and so two rows
    // lined up on the meter are two beats lined up in the room. Only
    // when there is something to compare it to: see the class comment.
    const double ourPhase = m_idle ? -1.0 : mixxx::prolink::barPhaseOf(m_pOurBeat->now());

    // **Each row keeps its own colour, always.** Recolouring an aligned pair was
    // a distraction rather than information: the rows lining up already says
    // they are aligned, and a colour that changes underneath makes the meter
    // look like it is switching modes.

    const double margin = 4.0;
    const double rowHeight = (height() - 3 * margin) / 2.0;
    const QRectF masterRect(margin, margin, width() - 2 * margin, rowHeight);
    const QRectF ourRect(margin, margin * 2 + rowHeight, width() - 2 * margin, rowHeight);

    // **What the top row is, at a glance.** "M3" is player 3 holding tempo
    // master; "3" alone is a deck drawn because nobody holds it; and a deck
    // held where its status says it stands -- paused, being cued -- is drawn
    // dim, because it is a place and not a phase. They used to look the same,
    // and a held deck looked like a deck sitting exactly on its beat.
    const int masterDevice = static_cast<int>(m_pMasterDevice->get());
    QString masterLabel = QStringLiteral("-");
    if (!m_idle && masterDevice > 0) {
        masterLabel = (m_pMeterIsMaster->get() > 0.0 ? QStringLiteral("M") : QString()) +
                QString::number(masterDevice);
    }
    QColor masterColour = m_masterColour;
    if (m_pMeterLive->get() <= 0.0) {
        masterColour.setAlphaF(0.4f);
    }
    paintRow(&painter, masterRect, masterPhase, masterColour, masterLabel);
    // The bottom row is always this deck; it is labelled only when we hold
    // tempo master, which is otherwise said nowhere near the meter.
    paintRow(&painter,
            ourRect,
            ourPhase,
            m_ourColour,
            m_pWeAreMaster->get() > 0.0 ? QStringLiteral("M") : QString());
}
