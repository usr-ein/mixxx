#pragma once

#include <QColor>
#include <memory>

#include "skin/legacy/skincontext.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class ControlProxy;

class WaveformRenderBeat : public WaveformRendererAbstract {
  public:
    explicit WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer);
    virtual ~WaveformRenderBeat();

    virtual bool init();
    virtual void setup(const QDomNode& node, const SkinContext& context);
    virtual void draw(QPainter* painter, QPaintEvent* event);

  private:
    QColor m_beatColor;
    QColor m_downbeatColor;
    /// Beats and downbeats alike, in pixels before the skin's scale: the
    /// skin's optional <BeatWidth>, 0 when unset (one pixel, as stock).
    double m_beatWidth = 0.0;
    QVector<QLineF> m_beats;
    QVector<QLineF> m_downbeats;
    std::unique_ptr<ControlProxy> m_pIntroStartPosition;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderBeat);
};
