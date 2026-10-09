#pragma once

#include <memory>

#include "util/class.h"
#include "waveformrenderersignalbase.h"

class StackedWaveform;

class WaveformRendererRGB : public WaveformRendererSignalBase {
  public:
    explicit WaveformRendererRGB(
        WaveformWidgetRenderer* waveformWidget);
    virtual ~WaveformRendererRGB();

    virtual void onSetup(const QDomNode& node);
    virtual void draw(QPainter* painter, QPaintEvent* event);

  private:
    /// The skin's optional <SignalStacked>; null draws the stock mix.
    std::unique_ptr<StackedWaveform> m_pStacked;

    DISALLOW_COPY_AND_ASSIGN(WaveformRendererRGB);
};
