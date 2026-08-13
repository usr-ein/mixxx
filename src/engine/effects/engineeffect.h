#pragma once

#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>
#include <memory>

#include "audio/types.h"
#include "control/controlobject.h"
#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectprocessor.h"
#include "engine/channelhandle.h"
#include "engine/effects/message.h"
#include "util/types.h"

/// EngineEffect is a generic wrapper around an EffectProcessor which intermediates
/// between an EffectSlot and the EffectProcessor. It implements the logic to handle
/// changes of state (enable switch, chain routing switches, parameters' state) so
/// so EffectProcessor subclasses only need to implement their specific DSP logic.
class EngineEffect final : public EffectsRequestHandler {
  public:
    /// Called in main thread by EffectSlot
    /// *pOutputLevel* is the slot's `output_level` control, owned by the
    /// EffectSlot that creates this and written from the audio thread. May be
    /// null. The slot outlives the engine's deletion of this object, which is
    /// what makes holding the bare pointer safe.
    EngineEffect(EffectManifestPointer pManifest,
            EffectsBackendManagerPointer pBackendManager,
            const QSet<ChannelHandleAndGroup>& activeInputChannels,
            const QSet<ChannelHandleAndGroup>& registeredInputChannels,
            const QSet<ChannelHandleAndGroup>& registeredOutputChannels,
            ControlObject* pOutputLevel = nullptr);
    /// Called in main thread by EffectSlot
    ~EngineEffect();

    /// Called from the main thread to make sure that the channel already has states
    void initalizeInputChannel(ChannelHandle inputChannel);

    /// Called in audio thread
    bool processEffectsRequest(
            const EffectsRequest& message,
            EffectsResponsePipe* pResponsePipe) override;

    /// Called in audio thread
    bool process(const ChannelHandle& inputHandle,
            const ChannelHandle& outputHandle,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const unsigned int numSamples,
            const mixxx::audio::SampleRate sampleRate,
            const EffectEnableState chainEnableState,
            const GroupFeatureState& groupFeatures);

    /// Publish what this slot is putting out, with VU ballistics: instant
    /// attack so a transient is never missed, exponential release so it is
    /// still there to see when the GUI next looks. Called from the audio
    /// thread by EngineEffectChain, which measures *after* the per-slot wet
    /// blend -- what the module contributes, not what its DSP produced.
    void publishOutputLevel(CSAMPLE peak, SINT frames, mixxx::audio::SampleRate sampleRate);

    const EffectManifestPointer getManifest() const {
        return m_pManifest;
    }

    const QString& name() const {
        return m_pManifest->name();
    }

    SINT getGroupDelayFrames() {
        return m_pProcessor->getGroupDelayFrames();
    }

    /// How much of this effect's output replaces its input, 0..1.
    ///
    /// Read by EngineEffectChain, which does the blending -- an effect cannot
    /// do it itself, because "its input" is the previous effect's output and
    /// only the chain knows what that was.
    CSAMPLE_GAIN wet() const {
        return m_wet;
    }

  private:
    QString debugString() const {
        return QString("EngineEffect(%1)").arg(m_pManifest->name());
    }

    EffectManifestPointer m_pManifest;
    std::unique_ptr<EffectProcessor> m_pProcessor;
    ChannelHandleMap<ChannelHandleMap<EffectEnableState>> m_effectEnableStateForChannelMatrix;
    CSAMPLE_GAIN m_wet;
    /// Owned by the EffectSlot; may be null. See the constructor.
    ControlObject* m_pOutputLevel;
    bool m_effectRampsFromDry;
    // Must not be modified after construction.
    QVector<EngineEffectParameterPointer> m_parameters;
    QMap<QString, EngineEffectParameterPointer> m_parametersById;

    DISALLOW_COPY_AND_ASSIGN(EngineEffect);
};
