#pragma once

#include <QLineF>
#include <QList>
#include <QPen>
#include <Qt>
#include <vector>

class QPainter;
class Waveform;
class WaveformSignalColors;
class WaveformWidgetRenderer;

/// The scrolling waveform's `<SignalStacked>` look, drawn for
/// WaveformRendererRGB.
///
/// The three bands as stacked bars, Rekordbox's 3-band look: highs at the
/// centre, mids on top of them, bass outermost, each as thick as there is of
/// it at that moment, so all three always show. Smoothed across columns and
/// scaled to the track's own loudest moment. Made by the renderer when the
/// skin asks for it; without it the renderer is stock.
class StackedWaveform {
  public:
    /// Drawn into *pRenderer*'s area, aligned as *alignment* says, in
    /// *pColors*' three band colours. Both are the renderer's, used and not
    /// owned.
    StackedWaveform(const WaveformWidgetRenderer* pRenderer,
            Qt::Alignment alignment,
            const WaveformSignalColors* pColors);

    /// *length* columns of *waveform*, from *firstVisualIndex* on and
    /// *visualIndicesPerPixel* apart, with the EQ's *bandGains* (low, mid,
    /// high) applied. *pen* is the renderer's, its colour set here per band.
    void draw(QPainter* painter,
            const Waveform& waveform,
            double firstVisualIndex,
            double visualIndicesPerPixel,
            int length,
            const float bandGains[3],
            QPen pen);

  private:
    /// Brings m_trackPeak up to date: the deepest stack -- low + mid + high --
    /// anywhere in the waveform so far, read as analysis fills it in.
    void updateTrackPeak(const Waveform& waveform);

    const WaveformWidgetRenderer* const m_pRenderer;
    const Qt::Alignment m_alignment;
    const WaveformSignalColors* const m_pColors;
    /// Per column, indexed [band * 2 + channel]: as read, then smoothed.
    /// Kept between frames so a frame does not allocate.
    std::vector<float> m_columns[6];
    std::vector<float> m_smoothed[6];
    /// One batch of lines per band, likewise kept: three drawLines() calls
    /// instead of a pen change per column.
    QList<QLineF> m_bandLines[3];
    /// The waveform m_trackPeak was read from, its size, and how much of it
    /// has been read. Compared, never dereferenced.
    const Waveform* m_peakWaveform = nullptr;
    int m_peakDataSize = 0;
    int m_peakScanned = 0;
    float m_trackPeak = 0.0f;
};
