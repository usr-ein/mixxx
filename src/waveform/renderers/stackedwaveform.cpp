#include "waveform/renderers/stackedwaveform.h"

#include <QPainter>

#include "util/math.h"
#include "waveform/renderers/waveformsignalcolors.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"

namespace {
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

StackedWaveform::StackedWaveform(const WaveformWidgetRenderer* pRenderer,
        Qt::Alignment alignment,
        const WaveformSignalColors* pColors)
        : m_pRenderer(pRenderer),
          m_alignment(alignment),
          m_pColors(pColors) {
}

void StackedWaveform::updateTrackPeak(const Waveform& waveform) {
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

void StackedWaveform::draw(QPainter* painter,
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
    const int breadth = m_pRenderer->getBreadth();
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
