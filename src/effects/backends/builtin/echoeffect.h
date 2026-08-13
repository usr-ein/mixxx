#pragma once

#include <QMap>

#include "effects/backends/effectprocessor.h"
#include "engine/engine.h"
#include "util/class.h"
#include "util/samplebuffer.h"

class EchoGroupState : public EffectState {
  public:
    // 6 seconds max, which is the full 8-beat range down to 80 BPM and the
    // 4-beat one down to 40. Beyond that the delay is clamped to the buffer
    // rather than refusing: a bar-long echo at 70 BPM comes out a little
    // short, which is a far better failure than silence.
    //
    // The cost is the buffer, allocated per effect instance per input channel:
    // 6 s stereo at 44.1 kHz is 2.1 MB. That is why this is 6 and not 16.
    static constexpr int kMaxDelaySeconds = 6;

    EchoGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        audioParametersChanged(engineParameters);
        clear();
    }
    ~EchoGroupState() override = default;

    void audioParametersChanged(const mixxx::EngineParameters& engineParameters) {
        delay_buf = mixxx::SampleBuffer(kMaxDelaySeconds *
                engineParameters.sampleRate() *
                engineParameters.channelCount());
    };

    void clear() {
        delay_buf.clear();
        prev_send = 0.0f;
        prev_feedback = 0.0f;
        prev_delay_samples = 0;
        write_position = 0;
        ping_pong = 0;
    };

    mixxx::SampleBuffer delay_buf;
    CSAMPLE_GAIN prev_send;
    CSAMPLE_GAIN prev_feedback;
    int prev_delay_samples;
    int write_position;
    int ping_pong;
};

class EchoEffect : public EffectProcessorImpl<EchoGroupState> {
  public:
    EchoEffect() = default;
    ~EchoEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            EchoGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pDelayParameter;
    EngineEffectParameterPointer m_pSendParameter;
    EngineEffectParameterPointer m_pFeedbackParameter;
    EngineEffectParameterPointer m_pPingPongParameter;
    EngineEffectParameterPointer m_pQuantizeParameter;
    EngineEffectParameterPointer m_pTripletParameter;

    DISALLOW_COPY_AND_ASSIGN(EchoEffect);
};
