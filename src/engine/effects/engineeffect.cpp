#include "engine/effects/engineeffect.h"

#include "effects/backends/effectsbackendmanager.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/engine.h"
#include "util/defs.h"
#include "util/math.h"
#include "util/sample.h"

#include <cmath>

namespace {

// Used during initialization where the SoundSevice is not set up
constexpr auto kInitalSampleRate = mixxx::audio::SampleRate(96000);

// How long the meter takes to fall by 1/e. Slow enough to read a percussive
// tail at a 10 Hz repaint, fast enough that a module going silent looks like it
// went silent rather than like the meter stuck.
constexpr double kReleaseSeconds = 0.25;

} // namespace

void EngineEffect::publishOutputLevel(
        CSAMPLE peak, SINT frames, mixxx::audio::SampleRate sampleRate) {
    if (!m_pOutputLevel || frames <= 0 || !sampleRate.isValid()) {
        return;
    }
    // Release is computed from the callback's real duration rather than being
    // a constant per callback, so the meter falls at the same rate whatever
    // the buffer size is set to -- the deck's `latency` index changes it by
    // factors of two.
    const double seconds = static_cast<double>(frames) / sampleRate;
    const auto release = static_cast<CSAMPLE>(std::exp(-seconds / kReleaseSeconds));
    const auto previous = static_cast<CSAMPLE>(m_pOutputLevel->get());
    // forceSet, not set. The control is marked read-only so that nothing
    // outside the engine can drive a meter, and setReadOnly() implements that
    // by installing a change-request handler that drops the value -- which
    // set() goes through, so it would silently discard the engine's own writes
    // too and leave every meter dark forever. forceSet bypasses the handler.
    m_pOutputLevel->forceSet(math_max(previous * release, peak));
}

EngineEffect::EngineEffect(EffectManifestPointer pManifest,
        EffectsBackendManagerPointer pBackendManager,
        const QSet<ChannelHandleAndGroup>& activeInputChannels,
        const QSet<ChannelHandleAndGroup>& registeredInputChannels,
        const QSet<ChannelHandleAndGroup>& registeredOutputChannels,
        ControlObject* pOutputLevel)
        : m_pManifest(pManifest),
          m_pProcessor(pBackendManager->createProcessor(pManifest)),
          // Fully wet until told otherwise, so an effect that never receives a
          // wet message behaves exactly as it did before this existed.
          m_wet(1.0f),
          m_pOutputLevel(pOutputLevel),
          m_parameters(pManifest->parameters().size()) {
    const QList<EffectManifestParameterPointer>& parameters = m_pManifest->parameters();
    for (int i = 0; i < parameters.size(); ++i) {
        EffectManifestParameterPointer param = parameters.at(i);
        EngineEffectParameterPointer pParameter(new EngineEffectParameter(param));
        m_parameters[i] = pParameter;
        m_parametersById[param->id()] = pParameter;
    }

    for (const ChannelHandleAndGroup& inputChannel : registeredInputChannels) {
        ChannelHandleMap<EffectEnableState> outputChannelMap;
        for (const ChannelHandleAndGroup& outputChannel : registeredOutputChannels) {
            outputChannelMap.insert(outputChannel.handle(), EffectEnableState::Disabled);
        }
        m_effectEnableStateForChannelMatrix.insert(inputChannel.handle(), outputChannelMap);
    }

    m_pProcessor->loadEngineEffectParameters(m_parametersById);

    // At this point the SoundDevice is not set up so we use the kInitalSampleRate.
    const mixxx::EngineParameters engineParameters(
            kInitalSampleRate,
            kMaxEngineFrames);
    m_pProcessor->initialize(activeInputChannels, registeredOutputChannels, engineParameters);
    m_effectRampsFromDry = pManifest->effectRampsFromDry();
}

EngineEffect::~EngineEffect() {
    if (kEffectDebugOutput) {
        qDebug() << debugString() << "destroyed";
    }
    m_parametersById.clear();
    m_parameters.clear();
}

void EngineEffect::initalizeInputChannel(ChannelHandle inputChannel) {
    if (m_pProcessor->hasStatesForInputChannel(inputChannel)) {
        // already initialized for this input channel
        return;
    }

    // At this point the SoundDevice is not set up so we use the kInitalSampleRate.
    const mixxx::EngineParameters engineParameters(
            kInitalSampleRate,
            kMaxEngineFrames);
    m_pProcessor->initializeInputChannel(inputChannel, engineParameters);
}

bool EngineEffect::processEffectsRequest(const EffectsRequest& message,
        EffectsResponsePipe* pResponsePipe) {
    EngineEffectParameterPointer pParameter;
    EffectsResponse response(message);

    switch (message.type) {
    case EffectsRequest::SET_EFFECT_PARAMETERS:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << "SET_EFFECT_PARAMETERS"
                     << "enabled" << message.SetEffectParameters.enabled;
        }

        m_wet = static_cast<CSAMPLE_GAIN>(message.SetEffectParameters.wet);

        for (auto& outputMap : m_effectEnableStateForChannelMatrix) {
            for (auto& enableState : outputMap) {
                if (enableState != EffectEnableState::Disabled &&
                        !message.SetEffectParameters.enabled) {
                    enableState = EffectEnableState::Disabling;
                    // If an input is not routed to the chain, and the effect gets
                    // a message to disable, then the effect gets the message to enable,
                    // process() will not have executed, so the enableState will still be
                    // DISABLING instead of DISABLED.
                } else if ((enableState == EffectEnableState::Disabled ||
                                   enableState ==
                                           EffectEnableState::Disabling) &&
                        message.SetEffectParameters.enabled) {
                    enableState = EffectEnableState::Enabling;
                }
            }
        }

        response.success = true;
        pResponsePipe->writeMessage(response);
        return true;
        break;
    case EffectsRequest::SET_PARAMETER_PARAMETERS:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << "SET_PARAMETER_PARAMETERS"
                     << "parameter" << message.SetParameterParameters.iParameter
                     << "value" << message.value;
        }
        pParameter = m_parameters.value(
                message.SetParameterParameters.iParameter, EngineEffectParameterPointer());
        if (pParameter) {
            pParameter->setValue(message.value);
            response.success = true;
        } else {
            response.success = false;
            response.status = EffectsResponse::NO_SUCH_PARAMETER;
        }
        pResponsePipe->writeMessage(response);
        return true;
    default:
        break;
    }
    return false;
}

bool EngineEffect::process(const ChannelHandle& inputHandle,
        const ChannelHandle& outputHandle,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const unsigned int numSamples,
        const mixxx::audio::SampleRate sampleRate,
        const EffectEnableState chainEnableState,
        const GroupFeatureState& groupFeatures) {
    // Compute the effective enable state from the combination of the effect's state
    // for the channel and the state passed from the EngineEffectChain.

    // When the chain's input routing switch or chain enable switches are changed,
    // the chain sends an intermediate enabling/disabling signal. The chain also sends
    // intermediate enabling/disabling signals when its dry/wet knob is turned down to
    // fully dry then turned back up to let some wet signal through.

    // Analagously, when the Effect is switched on/off, it sends this EngineEffect an
    // intermediate enabling/disabling signal.

    // The effective enable state is then passed down to the EffectProcessor, which is
    // responsible for taking appropriate action when it gets an intermediate
    // enabling/disabling signal. For example, the Echo effect clears its
    // internal buffer for the channel when it gets the intermediate disabling signal.

    EffectEnableState effectiveEffectEnableState =
            m_effectEnableStateForChannelMatrix[inputHandle][outputHandle];

    // If the EngineEffect is fully disabled, do not let
    // intermediate enabling/disabling signals from the chain override
    // the EngineEffect's state.
    if (effectiveEffectEnableState != EffectEnableState::Disabled) {
        if (chainEnableState == EffectEnableState::Disabled) {
            // If the chain is fully disabled, skip calling the EffectProcessor.
            effectiveEffectEnableState = EffectEnableState::Disabled;
        } else if (chainEnableState == EffectEnableState::Disabling) {
            // If the chain happens to be in the intermediate disabling state
            // in the same callback as the effect is in the intermediate enabling
            // state, the EffectProcessor should get the disabling signal, not the
            // enabling signal.
            effectiveEffectEnableState = EffectEnableState::Disabling;
        } else if (chainEnableState == EffectEnableState::Enabling) {
            // If the chain happens to be in the intermediate enabling state
            // in the same callback as the effect is in the intermediate disabling
            // state, the EffectProcessor should get the disabling signal, not the
            // enabling signal.
            if (effectiveEffectEnableState != EffectEnableState::Disabling) {
                effectiveEffectEnableState = EffectEnableState::Enabling;
            }
        }
    }

    bool processingOccured = false;

    if (effectiveEffectEnableState != EffectEnableState::Disabled) {
        //TODO: refactor rest of audio engine to use mixxx::AudioParameters
        const mixxx::EngineParameters engineParameters(
                sampleRate,
                numSamples / mixxx::kEngineChannelCount);

        m_pProcessor->process(inputHandle,
                outputHandle,
                pInput,
                pOutput,
                engineParameters,
                effectiveEffectEnableState,
                groupFeatures);

        processingOccured = true;

        if (!m_effectRampsFromDry) {
            // the effect does not fade, so we care for it
            if (effectiveEffectEnableState == EffectEnableState::Disabling) {
                DEBUG_ASSERT(pInput != pOutput); // Fade to dry only works if pInput is not touched by pOutput
                // Fade out (fade to dry signal)
                SampleUtil::linearCrossfadeBuffersOut(
                        pOutput,
                        pInput,
                        numSamples);
            } else if (effectiveEffectEnableState == EffectEnableState::Enabling) {
                DEBUG_ASSERT(pInput != pOutput); // Fade to dry only works if pInput is not touched by pOutput
                // Fade in (fade to wet signal)
                SampleUtil::linearCrossfadeBuffersIn(
                        pOutput,
                        pInput,
                        numSamples);
            }
        }
    }

    // Now that the EffectProcessor has been sent the intermediate enabling/disabling
    // signal, set the channel state to fully enabled/disabled for the next engine callback.
    EffectEnableState& effectOnChannelState = m_effectEnableStateForChannelMatrix[inputHandle][outputHandle];
    if (effectOnChannelState == EffectEnableState::Disabling) {
        effectOnChannelState = EffectEnableState::Disabled;
    } else if (effectOnChannelState == EffectEnableState::Enabling) {
        effectOnChannelState = EffectEnableState::Enabled;
    }

    return processingOccured;
}
