#include "waveform/renderers/waveformrenderbeat.h"

#include <QPainter>

#include "control/controlproxy.h"
#include "track/track.h"
#include "util/painterscope.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

class QPaintEvent;

namespace {
// Mixxx has no concept of a time signature, so assume 4/4 like Rekordbox does.
constexpr int kBeatsPerBar = 4;
} // namespace

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer)
        : WaveformRendererAbstract(waveformWidgetRenderer) {
    m_beats.resize(128);
    m_downbeats.resize(128 / kBeatsPerBar);
}

WaveformRenderBeat::~WaveformRenderBeat() {
}

bool WaveformRenderBeat::init() {
    m_pIntroStartPosition = std::make_unique<ControlProxy>(
            m_waveformRenderer->getGroup(), "intro_start_position");
    return true;
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& context) {
    m_beatColor = QColor(context.selectString(node, "BeatColor"));
    m_beatColor = WSkinColor::getCorrectColor(m_beatColor).toRgb();

    // Optional. When unset, every beat is drawn in BeatColor as before.
    m_downbeatColor = QColor(context.selectString(node, "DownbeatColor"));
    if (m_downbeatColor.isValid()) {
        m_downbeatColor = WSkinColor::getCorrectColor(m_downbeatColor).toRgb();
    }
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* /*event*/) {
    TrackPointer pTrackInfo = m_waveformRenderer->getTrackInfo();

    if (!pTrackInfo) {
        return;
    }

    mixxx::BeatsPointer trackBeats = pTrackInfo->getBeats();
    if (!trackBeats) {
        return;
    }

    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return;
    }

    const bool showDownbeats = m_downbeatColor.isValid();
#ifdef MIXXX_USE_QOPENGL
    // Using alpha transparency with drawLines causes a graphical issue when
    // drawing with QPainter on the QOpenGLWindow: instead of individual lines
    // a large rectangle encompassing all beatlines is drawn.
    m_beatColor.setAlphaF(1.f);
    if (showDownbeats) {
        m_downbeatColor.setAlphaF(1.f);
    }
#else
    m_beatColor.setAlphaF(alpha/100.0);
    if (showDownbeats) {
        m_downbeatColor.setAlphaF(alpha/100.0);
    }
#endif

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        return;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition();
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition();

    // qDebug() << "trackSamples" << trackSamples
    //          << "firstDisplayedPosition" << firstDisplayedPosition
    //          << "lastDisplayedPosition" << lastDisplayedPosition;

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);
    auto it = trackBeats->iteratorFrom(startPosition);

    // if no beat do not waste time saving/restoring painter
    if (it == trackBeats->cend() || *it > endPosition) {
        return;
    }

    // The intro cue doubles as the downbeat anchor: the beat closest to it is a
    // downbeat, and so is every kBeatsPerBar'th beat before and after it. A
    // track without an intro cue has no known bar phase, so nothing is
    // highlighted.
    bool highlightDownbeats = false;
    int beatIndex = 0;
    if (showDownbeats) {
        const auto introStartPosition = mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                m_pIntroStartPosition->get());
        if (introStartPosition.isValid()) {
            const auto anchorPosition = trackBeats->findClosestBeat(introStartPosition);
            if (anchorPosition.isValid()) {
                // Index of the first displayed beat, counted from the anchor.
                // Negative when the anchor is further into the track.
                beatIndex = it - trackBeats->iteratorFrom(anchorPosition);
                highlightDownbeats = true;
            }
        }
    }

    PainterScope PainterScope(painter);

    painter->setRenderHint(QPainter::Antialiasing);

    QPen beatPen(m_beatColor);
    beatPen.setWidthF(std::max(1.0, scaleFactor()));
    painter->setPen(beatPen);

    const Qt::Orientation orientation = m_waveformRenderer->getOrientation();
    const float rendererWidth = m_waveformRenderer->getWidth();
    const float rendererHeight = m_waveformRenderer->getHeight();

    int beatCount = 0;
    int downbeatCount = 0;

    for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(beatPosition);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        // beatIndex can be negative, so this is not a plain modulo.
        const bool isDownbeat = highlightDownbeats &&
                ((beatIndex % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar == 0;
        QVector<QLineF>& lines = isDownbeat ? m_downbeats : m_beats;
        int& lineCount = isDownbeat ? downbeatCount : beatCount;

        // If we don't have enough space, double the size.
        if (lineCount >= lines.size()) {
            lines.resize(lines.size() * 2);
        }

        if (orientation == Qt::Horizontal) {
            lines[lineCount++].setLine(xBeatPoint, 0.0f, xBeatPoint, rendererHeight);
        } else {
            lines[lineCount++].setLine(0.0f, xBeatPoint, rendererWidth, xBeatPoint);
        }
    }

    // Make sure to use constData to prevent detaches!
    painter->drawLines(m_beats.constData(), beatCount);

    if (downbeatCount > 0) {
        QPen downbeatPen(m_downbeatColor);
        downbeatPen.setWidthF(std::max(1.0, scaleFactor()));
        painter->setPen(downbeatPen);
        painter->drawLines(m_downbeats.constData(), downbeatCount);
    }
}
