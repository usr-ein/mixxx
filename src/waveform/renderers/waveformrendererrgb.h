#pragma once

#include <QLineF>
#include <QList>
#include <QPen>
#include <vector>

#include "util/class.h"
#include "waveformrenderersignalbase.h"

class Waveform;

class WaveformRendererRGB : public WaveformRendererSignalBase {
  public:
    explicit WaveformRendererRGB(
        WaveformWidgetRenderer* waveformWidget);
    virtual ~WaveformRendererRGB();

    virtual void onSetup(const QDomNode& node);
    virtual void draw(QPainter* painter, QPaintEvent* event);

  private:
    /// The three bands as stacked bars, Rekordbox's 3-band look: highs at the
    /// centre, mids on top of them, bass outermost, each as thick as there is
    /// of it at that moment, so all three always show. Smoothed across
    /// columns and scaled to the track's own loudest moment. Set by the skin's
    /// optional <SignalStacked>; without it the renderer is stock.
    void drawStacked(QPainter* painter,
            const Waveform& waveform,
            double firstVisualIndex,
            double visualIndicesPerPixel,
            int length,
            const float bandGains[3],
            QPen pen);
    /// Brings m_trackPeak up to date: the deepest stack -- low + mid + high --
    /// anywhere in the waveform so far, read as analysis fills it in.
    void updateTrackPeak(const Waveform& waveform);

    bool m_stacked = false;
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

    DISALLOW_COPY_AND_ASSIGN(WaveformRendererRGB);
};
