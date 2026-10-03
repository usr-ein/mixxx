#include "waveformrendererrgb.h"

#include <QElapsedTimer>

#include "util/math.h"
#include "util/painterscope.h"
#include "util/xml.h"
#include "waveform/waveform.h"
#include "waveformwidgetrenderer.h"

WaveformRendererRGB::WaveformRendererRGB(
        WaveformWidgetRenderer* waveformWidgetRenderer)
        : WaveformRendererSignalBase(waveformWidgetRenderer) {
}

WaveformRendererRGB::~WaveformRendererRGB() {
}

void WaveformRendererRGB::onSetup(const QDomNode& node) {
    // Optional. Absent, every column is the three bands mixed, as stock.
    m_stacked = XmlParse::selectNodeBool(node, QStringLiteral("SignalStacked"));
}

namespace {
/// Say why nothing was drawn, at most once a second.
///
/// A waveform that does not appear looks identical whatever the reason, and
/// all five reasons below are silent. Rate-limited because this is a render
/// path: a warning per frame would be sixty a second.
void declined(const char* why) {
    static QElapsedTimer since;
    if (since.isValid() && since.elapsed() < 1000) {
        return;
    }
    since.start();
    qWarning() << "waveform not drawn:" << why;
}

/// How far out the track's deepest stack reaches, as a share of the
/// half-height. The rest is headroom, so the waveform never meets the edge.
constexpr float kPeakFill = 0.9f;

/// Columns either side that smoothing reaches; the weights fall off linearly
/// with distance, 1 2 3 2 1.
constexpr int kSmoothRadius = 2;

/// A triangular moving average: each column becomes a mean of its
/// neighbours, weighted by closeness. It rounds off the flat-topped runs left
/// where loud material saturates the analyser and the column-to-column
/// jitter of per-pixel peaks, while keeping a hit within a few pixels.
void smoothTriangular(const std::vector<float>& in, std::vector<float>* pOut) {
    const int count = static_cast<int>(in.size());
    pOut->resize(in.size());
    for (int x = 0; x < count; ++x) {
        float sum = 0.0f;
        float weights = 0.0f;
        for (int k = -kSmoothRadius; k <= kSmoothRadius; ++k) {
            const int i = x + k;
            if (i < 0 || i >= count) {
                continue;
            }
            const auto weight = static_cast<float>(kSmoothRadius + 1 - (k < 0 ? -k : k));
            sum += weight * in[i];
            weights += weight;
        }
        (*pOut)[x] = sum / weights;
    }
}
} // namespace

void WaveformRendererRGB::draw(
        QPainter* painter,
        QPaintEvent* /*event*/) {
    ConstWaveformPointer pWaveform = m_waveformRenderer->getWaveform();
    if (pWaveform.isNull()) {
        declined("the track has no waveform");
        return;
    }

    const double audioVisualRatio = pWaveform->getAudioVisualRatio();
    if (audioVisualRatio <= 0) {
        declined("the waveform has no audio-to-visual ratio");
        return;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const int dataSize = pWaveform->getDataSize();
    if (dataSize <= 1) {
        declined("the waveform is empty");
        return;
    }

    const WaveformData* data = pWaveform->data();
    if (data == nullptr) {
        declined("the waveform has no data");
        return;
    }

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        declined("the engine has not published a track length");
        return;
    }

    PainterScope PainterScope(painter);

    painter->setRenderHints(QPainter::Antialiasing, false);
    painter->setRenderHints(QPainter::SmoothPixmapTransform, false);
    painter->setWorldMatrixEnabled(false);
    painter->resetTransform();

    // Rotate if drawing vertical waveforms
    // and revert devicePixelRatio scaling in x direction.
    if (m_waveformRenderer->getOrientation() == Qt::Vertical) {
        painter->setTransform(QTransform(0, 1 / devicePixelRatio, 1, 0, 0, 0));
    } else {
        painter->setTransform(QTransform(1 / devicePixelRatio, 0, 0, 1, 0, 0));
    }

    const double firstVisualIndex =
            m_waveformRenderer->getFirstDisplayedPosition() * trackSamples /
            audioVisualRatio;
    const double lastVisualIndex =
            m_waveformRenderer->getLastDisplayedPosition() * trackSamples /
            audioVisualRatio;

    const double offset = firstVisualIndex;

    const float length = m_waveformRenderer->getLength() * devicePixelRatio;

    // Represents the # of waveform data points per horizontal pixel.
    const double gain = (lastVisualIndex - firstVisualIndex) / length;

    // Per-band gain from the EQ knobs.
    float allGain(1.0), lowGain(1.0), midGain(1.0), highGain(1.0);
    getGains(&allGain, true, &lowGain, &midGain, &highGain);

    QColor color;

    QPen pen;
    pen.setCapStyle(Qt::FlatCap);
    pen.setWidthF(math_max(1.0, 1.0 / m_waveformRenderer->getVisualSamplePerPixel()));

    const int breadth = m_waveformRenderer->getBreadth();
    const float halfBreadth = static_cast<float>(breadth) / 2.0f;

    const float heightFactor = allGain * halfBreadth / sqrtf(255 * 255 * 3);

    // Draw reference line
    painter->setPen(m_pColors->getAxesColor());
    painter->drawLine(QLineF(0, halfBreadth, m_waveformRenderer->getLength(), halfBreadth));

    if (m_stacked) {
        const float bandGains[3] = {lowGain, midGain, highGain};
        drawStacked(painter,
                *pWaveform,
                offset,
                gain,
                static_cast<int>(length),
                bandGains,
                pen);
        return;
    }

    for (int x = 0; x < static_cast<int>(length); ++x) {
        // Width of the x position in visual indices.
        const double xSampleWidth = gain * x;

        // Effective visual index of x
        const double xVisualSampleIndex = xSampleWidth + offset;

        // Our current pixel (x) corresponds to a number of visual samples
        // (visualSamplerPerPixel) in our waveform object. We take the max of
        // all the data points on either side of xVisualSampleIndex within a
        // window of 'maxSamplingRange' visual samples to measure the maximum
        // data point contained by this pixel.
        double maxSamplingRange = gain / 2.0;

        // Since xVisualSampleIndex is in visual-samples (e.g. R,L,R,L) we want
        // to check +/- maxSamplingRange frames, not samples. To do this, divide
        // xVisualSampleIndex by 2. Since frames indices are integers, we round
        // to the nearest integer by adding 0.5 before casting to int.
        int visualFrameStart = int(xVisualSampleIndex / 2.0 - maxSamplingRange + 0.5);
        int visualFrameStop = int(xVisualSampleIndex / 2.0 + maxSamplingRange + 0.5);
        const int lastVisualFrame = dataSize / 2 - 1;

        // We now know that some subset of [visualFrameStart, visualFrameStop]
        // lies within the valid range of visual frames. Clamp
        // visualFrameStart/Stop to within [0, lastVisualFrame].
        visualFrameStart = math_clamp(visualFrameStart, 0, lastVisualFrame);
        visualFrameStop = math_clamp(visualFrameStop, 0, lastVisualFrame);

        int visualIndexStart = visualFrameStart * 2;
        int visualIndexStop  = visualFrameStop * 2;

        unsigned char maxLow  = 0;
        unsigned char maxMid  = 0;
        unsigned char maxHigh = 0;
        float maxAll = 0.;
        float maxAllNext = 0.;

        for (int i = visualIndexStart;
             i >= 0 && i + 1 < dataSize && i + 1 <= visualIndexStop; i += 2) {
            const WaveformData& waveformData = data[i];
            const WaveformData& waveformDataNext = data[i + 1];

            maxLow  = math_max3(maxLow,  waveformData.filtered.low,  waveformDataNext.filtered.low);
            maxMid  = math_max3(maxMid,  waveformData.filtered.mid,  waveformDataNext.filtered.mid);
            maxHigh = math_max3(maxHigh, waveformData.filtered.high, waveformDataNext.filtered.high);
            float all = static_cast<float>(pow(waveformData.filtered.low * lowGain, 2) +
                    pow(waveformData.filtered.mid * midGain, 2) +
                    pow(waveformData.filtered.high * highGain, 2));
            maxAll = math_max(maxAll, all);
            float allNext = static_cast<float>(pow(waveformDataNext.filtered.low * lowGain, 2) +
                    pow(waveformDataNext.filtered.mid * midGain, 2) +
                    pow(waveformDataNext.filtered.high * highGain, 2));
            maxAllNext = math_max(maxAllNext, allNext);
        }

        float maxLowF = maxLow * lowGain;
        float maxMidF = maxMid * midGain;
        float maxHighF = maxHigh * highGain;

        float red = maxLowF * m_rgbLowColor_r + maxMidF * m_rgbMidColor_r +
                maxHighF * m_rgbHighColor_r;
        float green = maxLowF * m_rgbLowColor_g + maxMidF * m_rgbMidColor_g +
                maxHighF * m_rgbHighColor_g;
        float blue = maxLowF * m_rgbLowColor_b + maxMidF * m_rgbMidColor_b +
                maxHighF * m_rgbHighColor_b;

        // Compute maximum (needed for value normalization)
        float max = math_max3(red, green, blue);

        // Prevent division by zero
        if (max > 0.0f) {
            // Set color
            color.setRgbF(red / max, green / max, blue / max);

            pen.setColor(color);

            painter->setPen(pen);
            switch (m_alignment) {
                case Qt::AlignBottom:
                case Qt::AlignRight:
                    painter->drawLine(
                        x, breadth,
                        x, breadth - (int)(heightFactor * sqrtf(math_max(maxAll, maxAllNext))));
                    break;
                case Qt::AlignTop:
                case Qt::AlignLeft:
                    painter->drawLine(
                        x, 0,
                        x, (int)(heightFactor * sqrtf(math_max(maxAll, maxAllNext))));
                    break;
                default:
                    painter->drawLine(
                        x, (int)(halfBreadth - heightFactor * sqrtf(maxAll)),
                        x, (int)(halfBreadth + heightFactor * sqrtf(maxAllNext)));
            }
        }
    }
}

void WaveformRendererRGB::updateTrackPeak(const Waveform& waveform) {
    const int dataSize = waveform.getDataSize();
    if (&waveform != m_peakWaveform || dataSize != m_peakDataSize) {
        m_peakWaveform = &waveform;
        m_peakDataSize = dataSize;
        m_peakScanned = 0;
        m_trackPeak = 0.0f;
    }
    // Analysis fills the waveform front to back and says how far it has got,
    // so a frame reads only what arrived since the last one. A waveform
    // loaded whole is read once.
    const WaveformData* data = waveform.data();
    const int completion = math_min(waveform.getCompletion(), dataSize);
    for (int i = m_peakScanned; i < completion; ++i) {
        const auto& filtered = data[i].filtered;
        m_trackPeak = math_max(m_trackPeak,
                static_cast<float>(filtered.low + filtered.mid + filtered.high));
    }
    m_peakScanned = math_max(m_peakScanned, completion);
}

void WaveformRendererRGB::drawStacked(QPainter* painter,
        const Waveform& waveform,
        double firstVisualIndex,
        double visualIndicesPerPixel,
        int length,
        const float bandGains[3],
        QPen pen) {
    updateTrackPeak(waveform);
    if (m_trackPeak <= 0.0f || length <= 0) {
        return;
    }
    const WaveformData* data = waveform.data();
    const int dataSize = waveform.getDataSize();
    const int breadth = m_waveformRenderer->getBreadth();
    const float halfBreadth = static_cast<float>(breadth) / 2.0f;
    const int lastVisualFrame = dataSize / 2 - 1;

    for (std::vector<float>& column : m_columns) {
        column.assign(length, 0.0f);
    }
    for (int x = 0; x < length; ++x) {
        // The same pixel-to-data mapping as the mixed view in draw(): every
        // frame within half a pixel either side of the column's centre.
        const double xVisualSampleIndex = visualIndicesPerPixel * x + firstVisualIndex;
        const double maxSamplingRange = visualIndicesPerPixel / 2.0;
        const int visualFrameStart = math_clamp(
                int(xVisualSampleIndex / 2.0 - maxSamplingRange + 0.5), 0, lastVisualFrame);
        const int visualFrameStop = math_clamp(
                int(xVisualSampleIndex / 2.0 + maxSamplingRange + 0.5), 0, lastVisualFrame);

        // [band][channel]: low, mid, high; left, right. The data interleaves
        // the channels, left on the even indices.
        unsigned char peak[3][2] = {};
        for (int i = visualFrameStart * 2;
                i >= 0 && i + 1 < dataSize && i + 1 <= visualFrameStop * 2;
                i += 2) {
            for (int channel = 0; channel < 2; ++channel) {
                const auto& filtered = data[i + channel].filtered;
                peak[0][channel] = math_max(peak[0][channel], filtered.low);
                peak[1][channel] = math_max(peak[1][channel], filtered.mid);
                peak[2][channel] = math_max(peak[2][channel], filtered.high);
            }
        }
        for (int band = 0; band < 3; ++band) {
            for (int channel = 0; channel < 2; ++channel) {
                m_columns[band * 2 + channel][x] = bandGains[band] * peak[band][channel];
            }
        }
    }
    for (int i = 0; i < 6; ++i) {
        smoothTriangular(m_columns[i], &m_smoothed[i]);
    }

    // The track's own deepest stack fills kPeakFill of the half-height,
    // whatever the track's level: three bands deep, no fixed scale suits a
    // quiet master and a loud one alike. The deck's gain and the visual gain
    // leave this view alone; the EQ band gains above still apply.
    const float scale = kPeakFill * halfBreadth / m_trackPeak;
    for (QList<QLineF>& lines : m_bandLines) {
        lines.clear();
    }
    for (int x = 0; x < length; ++x) {
        // How far out each band's outer edge is, per channel. Outward from
        // the centre: highs, then mids on top of them, then bass on top of
        // both, so a band's edge is everything inside it plus itself.
        float edge[3][2];
        for (int channel = 0; channel < 2; ++channel) {
            const float low = m_smoothed[0 * 2 + channel][x];
            const float mid = m_smoothed[1 * 2 + channel][x];
            const float high = m_smoothed[2 * 2 + channel][x];
            edge[2][channel] = math_min(halfBreadth, scale * high);
            edge[1][channel] = math_min(halfBreadth, scale * (high + mid));
            edge[0][channel] = math_min(halfBreadth, scale * (high + mid + low));
        }
        for (int band = 0; band < 3; ++band) {
            const float left = edge[band][0];
            const float right = edge[band][1];
            if (left <= 0.0f && right <= 0.0f) {
                continue;
            }
            switch (m_alignment) {
            case Qt::AlignBottom:
            case Qt::AlignRight:
                m_bandLines[band].append(QLineF(x, breadth, x, breadth - math_max(left, right)));
                break;
            case Qt::AlignTop:
            case Qt::AlignLeft:
                m_bandLines[band].append(QLineF(x, 0, x, math_max(left, right)));
                break;
            default:
                // The top half is the left channel and the bottom the right,
                // as in the mixed view.
                m_bandLines[band].append(QLineF(x, halfBreadth - left, x, halfBreadth + right));
            }
        }
    }

    // Outermost first: bass to the full depth of the stack, then mids over
    // it, then highs in the middle, so each shows as its own band.
    const QColor colors[3] = {m_pColors->getRgbLowColor(),
            m_pColors->getRgbMidColor(),
            m_pColors->getRgbHighColor()};
    for (int band = 0; band < 3; ++band) {
        pen.setColor(colors[band]);
        painter->setPen(pen);
        painter->drawLines(m_bandLines[band]);
    }
}
