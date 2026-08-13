#include "engine/effects/engineeffectchain.h"

#include "engine/effects/engineeffect.h"
#include "util/defs.h"
#include "util/math.h"
#include "util/sample.h"

#include <cmath>

namespace {
// Same shape as a slot's meter, and the same reasoning: see
// EngineEffect::publishOutputLevel().
constexpr double kOutputReleaseSeconds = 0.25;
} // namespace

EngineEffectChain::EngineEffectChain(const QString& group,
        const QSet<ChannelHandleAndGroup>& registeredInputChannels,
        const QSet<ChannelHandleAndGroup>& registeredOutputChannels)
        : m_group(group),
          m_enableState(true),
          m_mixMode(EffectChainMixMode::DrySlashWet),
          m_dMix(0),
          m_dMakeup(1.0f),
          m_outputLevel(ConfigKey(group, QStringLiteral("output_level"))),
          m_buffer1(kMaxEngineSamples),
          m_buffer2(kMaxEngineSamples) {
    // Nothing outside the engine may drive a meter: a level a skin can set is
    // a level that can lie about the audio.
    m_outputLevel.setReadOnly();

    // Try to prevent memory allocation.
    m_effects.reserve(256);

    // We may need to add inputs later on, eg. when skins request more samplers,
    // which means we need to append to m_chainStatusForChannelMatrix.
    // Let's store the output map so we can reuse it, eg. in enableForInputChannel()
    for (const ChannelHandleAndGroup& outputChannel : registeredOutputChannels) {
        m_outputChannelMap.insert(outputChannel.handle(), ChannelStatus());
    }
    for (const ChannelHandleAndGroup& inputChannel : registeredInputChannels) {
        m_chainStatusForChannelMatrix.insert(inputChannel.handle(), m_outputChannelMap);
    }
}

EngineEffectChain::~EngineEffectChain() {
}

bool EngineEffectChain::addEffect(EngineEffect* pEffect, int iIndex) {
    if (iIndex < 0) {
        if (kEffectDebugOutput) {
            qDebug() << debugString()
                     << "WARNING: ADD_EFFECT_TO_CHAIN message with invalid index:"
                     << iIndex;
        }
        return false;
    }
    if (m_effects.contains(pEffect)) {
        if (kEffectDebugOutput) {
            qDebug() << debugString() << "WARNING: effect already added to EngineEffectChain:"
                     << pEffect->name();
        }
        return false;
    }

    while (iIndex >= m_effects.size()) {
        m_effects.append(nullptr);
    }
    m_effects.replace(iIndex, pEffect);
    return true;
}

bool EngineEffectChain::removeEffect(EngineEffect* pEffect, int iIndex) {
    if (iIndex < 0) {
        if (kEffectDebugOutput) {
            qDebug() << debugString()
                     << "WARNING: REMOVE_EFFECT_FROM_CHAIN message with invalid index:"
                     << iIndex;
        }
        return false;
    }
    if (m_effects.at(iIndex) != pEffect) {
        qDebug() << debugString()
                 << "WARNING: REMOVE_EFFECT_FROM_CHAIN consistency error"
                 << m_effects.at(iIndex) << "loaded but received request to remove"
                 << pEffect;
        return false;
    }

    m_effects.replace(iIndex, nullptr);
    return true;
}

// this is called from the engine thread onCallbackStart()
bool EngineEffectChain::updateParameters(const EffectsRequest& message) {
    // TODO(rryan): Parameter interpolation.
    m_mixMode = message.SetEffectChainParameters.mix_mode;
    m_dMix = static_cast<CSAMPLE>(message.SetEffectChainParameters.mix);
    m_dMakeup = static_cast<CSAMPLE>(message.SetEffectChainParameters.makeup);
    m_enableState = message.SetEffectParameters.enabled;
    return true;
}

bool EngineEffectChain::processEffectsRequest(const EffectsRequest& message,
        EffectsResponsePipe* pResponsePipe) {
    EffectsResponse response(message);
    switch (message.type) {
    case EffectsRequest::ADD_EFFECT_TO_CHAIN:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << this << "ADD_EFFECT_TO_CHAIN"
                     << message.AddEffectToChain.pEffect
                     << message.AddEffectToChain.iIndex;
        }
        response.success = addEffect(message.AddEffectToChain.pEffect,
                message.AddEffectToChain.iIndex);
        break;
    case EffectsRequest::REMOVE_EFFECT_FROM_CHAIN:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << this << "REMOVE_EFFECT_FROM_CHAIN"
                     << message.RemoveEffectFromChain.pEffect
                     << message.RemoveEffectFromChain.iIndex;
        }
        response.success = removeEffect(message.RemoveEffectFromChain.pEffect,
                message.RemoveEffectFromChain.iIndex);
        break;
    case EffectsRequest::SET_EFFECT_CHAIN_PARAMETERS:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << this << "SET_EFFECT_CHAIN_PARAMETERS"
                     << "enabled =" << message.SetEffectChainParameters.enabled
                     << "mix =" << message.SetEffectChainParameters.mix
                     << "mix_mode =" << static_cast<int>(message.SetEffectChainParameters.mix_mode);
        }
        response.success = updateParameters(message);
        break;
    case EffectsRequest::ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << this
                     << "ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL"
                     << message.pTargetChain
                     << message.EnableInputChannelForChain.channelHandle;
        }
        response.success = enableForInputChannel(
                message.EnableInputChannelForChain.channelHandle);
        break;
    case EffectsRequest::DISABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL:
        if (kEffectDebugOutput) {
            qDebug() << debugString() << this
                     << "DISABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL"
                     << message.pTargetChain
                     << message.DisableInputChannelForChain.channelHandle;
        }
        response.success = disableForInputChannel(
                message.DisableInputChannelForChain.channelHandle);
        break;
    default:
        return false;
    }
    pResponsePipe->writeMessage(response);
    return true;
}

bool EngineEffectChain::enableForInputChannel(ChannelHandle inputHandle) {
    if (kEffectDebugOutput) {
        qDebug() << "EngineEffectChain::enableForInputChannel" << this << inputHandle;
    }

    if (m_chainStatusForChannelMatrix[inputHandle].isEmpty()) {
        // Apparently a request to enable an unregistered input.
        // ChannelHandleMap's operator[] does maybeExpand(), so we now have an
        // inputHandle key and we can assign our outputmap to it.
        // Now request the map reference again and we're ready to roll...
        m_chainStatusForChannelMatrix[inputHandle] = m_outputChannelMap;
    }
    auto& outputMap = m_chainStatusForChannelMatrix[inputHandle];

    for (auto&& outputChannelStatus : outputMap) {
        DEBUG_ASSERT(outputChannelStatus.enableState != EffectEnableState::Enabled);
        outputChannelStatus.enableState = EffectEnableState::Enabling;
    }
    return true;
}

bool EngineEffectChain::disableForInputChannel(ChannelHandle inputHandle) {
    auto& outputMap = m_chainStatusForChannelMatrix[inputHandle];
    for (auto&& outputChannelStatus : outputMap) {
        if (outputChannelStatus.enableState == EffectEnableState::Enabling) {
            // Channel has never been processed and can be disabled immediately
            outputChannelStatus.enableState = EffectEnableState::Disabled;
        } else if (outputChannelStatus.enableState == EffectEnableState::Enabled) {
            // Channel was enabled, fade effect out via Disabling state
            outputChannelStatus.enableState = EffectEnableState::Disabling;
        }
    }
    return true;
}

bool EngineEffectChain::process(const ChannelHandle& inputHandle,
        const ChannelHandle& outputHandle,
        CSAMPLE* pIn,
        CSAMPLE* pOut,
        const unsigned int numSamples,
        const mixxx::audio::SampleRate sampleRate,
        const GroupFeatureState& groupFeatures,
        bool fadeout) {
    DEBUG_ASSERT(numSamples <= kMaxEngineSamples);

    // Compute the effective enable state from the channel input routing switch and
    // the chain's enable state. When either of these are turned on/off, send the
    // effects the intermediate enabling/disabling signal.
    // If the EngineEffect is not disabled for the channel, it will pass the
    // intermediate state down to the EffectProcessor, which is then responsible for reacting
    // appropriately, for example the Echo effect clears its internal buffer for the channel
    // when it gets the intermediate disabling signal.

    ChannelStatus& channelStatus = m_chainStatusForChannelMatrix[inputHandle][outputHandle];
    EffectEnableState effectiveChainEnableState = channelStatus.enableState;

    if (channelStatus.enableState == EffectEnableState::Disabling) {
        // Disabled via disableForInputChannel().
        channelStatus.enableState = EffectEnableState::Disabled;
    } else if (!m_enableState || fadeout) {
        if (channelStatus.enableState == EffectEnableState::Enabled) {
            // fadeout is true during the last callback before the track is paused.
            // The track is ramped to zero to avoid clicks.
            // It can started again without further notice.
            // Make sure the effect is paused as well.
            effectiveChainEnableState = EffectEnableState::Disabling;
            // Effect will be paused now, ramp up next callback which may happen later
            // (Enabling is a standby mode).
            channelStatus.enableState = EffectEnableState::Enabling;
        } else if (channelStatus.enableState == EffectEnableState::Enabling) {
            // effect is still disabled
            effectiveChainEnableState = EffectEnableState::Disabled;
        }
    } else if (channelStatus.enableState == EffectEnableState::Enabling) {
        channelStatus.enableState = EffectEnableState::Enabled;
    }

    CSAMPLE currentMixKnob = m_dMix;
    if (m_mixMode == EffectChainMixMode::WetOnly) {
        // The makeup gain rides on the mix knob rather than being applied
        // separately, which gets its ramping and its per-channel previous
        // value for free -- and there is no dry complement in this mode for a
        // gain above unity to break. It matters because a wet-only chain has
        // no gain in it anywhere: the output is the wet at unity, and a reverb
        // tail at unity is far quieter than the dry it sits beside in the
        // external mixer.
        currentMixKnob *= m_dMakeup;
    }
    CSAMPLE lastCallbackMixKnob = channelStatus.oldMixKnob;

    bool processingOccured = false;
    if (effectiveChainEnableState != EffectEnableState::Disabled) {
        // Ramping code inside the effects need to access the original samples
        // after writing to the output buffer. This requires not to use the same buffer
        // for in and output: Also, ChannelMixer::applyEffectsAndMixChannels
        // requires that the input buffer does not get modified.
        CSAMPLE* pIntermediateInput = pIn;
        CSAMPLE* pIntermediateOutput;
        SINT effectChainGroupDelayFrames = 0;
        bool firstAddDryToWetEffectProcessed = false;

        for (int slotIndex = 0; slotIndex < m_effects.size(); ++slotIndex) {
            EngineEffect* pEffect = m_effects.at(slotIndex);
            if (pEffect != nullptr) {
                // Select an unused intermediate buffer for the next output
                if (pIntermediateInput == m_buffer1.data()) {
                    pIntermediateOutput = m_buffer2.data();
                } else {
                    pIntermediateOutput = m_buffer1.data();
                }

                if (pEffect->process(inputHandle,
                            outputHandle,
                            pIntermediateInput,
                            pIntermediateOutput,
                            numSamples,
                            sampleRate,
                            effectiveChainEnableState,
                            groupFeatures)) {
                    if (pEffect->getManifest()->addDryToWet()) {
                        // Skip adding the dry signal to the effect's wet output
                        // when it is the first addDryToWet type effect in a
                        // chain that is not in DrySlashWet mode. This allows
                        // effects after it to process only the wet output. For
                        // example, when chaining Echo then Reverb in DryPlusWet
                        // mode, the Reverb effect will get only the wet output
                        // of Echo to process instead of the echoed signal mixed
                        // with the input to Echo. The dry signal that entered
                        // the first effect in the chain will be mixed back in
                        // below after all effects in the chain have been
                        // processed -- except in WetOnly mode, which never
                        // mixes it back, so skipping it here is what keeps the
                        // dry out of the output entirely.
                        bool skipAddingDry = !firstAddDryToWetEffectProcessed &&
                                m_mixMode != EffectChainMixMode::DrySlashWet;

                        if (!skipAddingDry) {
                            // `<`, not `<=`. Upstream reads and writes one
                            // sample past the end of BOTH buffers here. It is
                            // usually harmless because the buffers are sized
                            // for the largest callback the engine allows, but
                            // at that size it is a genuine heap overflow -- and
                            // this deck has been chasing shutdown heap
                            // corruption, so a known out-of-bounds write in the
                            // audio thread is not something to leave lying
                            // around. Worth offering upstream.
                            for (SINT i = 0; i < static_cast<SINT>(numSamples); ++i) {
                                pIntermediateOutput[i] += pIntermediateInput[i];
                            }
                        }

                        firstAddDryToWetEffectProcessed = true;
                    }

                    // Per-slot wet: how much of this effect's output replaces
                    // what went into it. WetOnly chains only -- this is what
                    // makes the deck's effect rack a chain of modules each with
                    // its own blend, and applying it anywhere else would change
                    // deck, QuickEffect and EQ chains for no reason.
                    //
                    // The dry a downstream slot blends back in is the previous
                    // slot's output, never the chain's input, so blending here
                    // cannot resurrect the original dry once some upstream
                    // effect has destroyed it. Which is exactly the guarantee
                    // the rack is built on: the first effect that generates new
                    // material runs at a wet of 1, and after that there is no
                    // dry left to leak.
                    if (m_mixMode == EffectChainMixMode::WetOnly &&
                            slotIndex < static_cast<int>(channelStatus.oldWet.size())) {
                        const CSAMPLE_GAIN wet = pEffect->wet();
                        const CSAMPLE_GAIN oldWet = channelStatus.oldWet[slotIndex];
                        channelStatus.oldWet[slotIndex] = wet;
                        const SINT frames = static_cast<SINT>(numSamples) / 2;
                        if ((wet < 1.0f || oldWet < 1.0f) && frames > 0) {
                            // Written out rather than handed to SampleUtil
                            // because this blend is in place: the destination
                            // is also one of the sources. SampleUtil's pointers
                            // are M_RESTRICT, which promises the compiler they
                            // do not alias, and breaking that promise in the
                            // audio thread is undefined behaviour rather than a
                            // wrong number. The dry-add above is a plain loop
                            // for the same reason.
                            //
                            // Ramped per frame, not per sample, so left and
                            // right always get the same gain.
                            const CSAMPLE_GAIN delta =
                                    (wet - oldWet) / static_cast<CSAMPLE_GAIN>(frames);
                            for (SINT frame = 0; frame < frames; ++frame) {
                                const CSAMPLE_GAIN w =
                                        oldWet + delta * static_cast<CSAMPLE_GAIN>(frame);
                                const CSAMPLE_GAIN d = 1.0f - w;
                                const SINT i = frame * 2;
                                pIntermediateOutput[i] = pIntermediateOutput[i] * w +
                                        pIntermediateInput[i] * d;
                                pIntermediateOutput[i + 1] = pIntermediateOutput[i + 1] * w +
                                        pIntermediateInput[i + 1] * d;
                            }
                        }
                    }

                    // Metered here rather than inside the effect, and after the
                    // blend rather than before it, because what a DJ wants from
                    // a module's meter is what the module *contributes* -- a
                    // slot at a wet of zero is passing its input through
                    // untouched and should read as doing nothing, however busy
                    // its DSP is.
                    //
                    // This is the instrument that was missing when a filter
                    // closed to 13 Hz silenced the whole rack and every module
                    // still looked healthy (PRD §17.3): the signal visibly dies
                    // at the module that killed it.
                    pEffect->publishOutputLevel(
                            SampleUtil::maxAbsAmplitude(pIntermediateOutput, numSamples),
                            static_cast<SINT>(numSamples) / 2,
                            sampleRate);

                    processingOccured = true;
                    effectChainGroupDelayFrames += pEffect->getGroupDelayFrames();

                    // Output of this effect becomes the input of the next effect
                    pIntermediateInput = pIntermediateOutput;
                }
            }
        }

        m_effectsDelay.setDelayFrames(effectChainGroupDelayFrames);
        m_effectsDelay.process(pIn, numSamples);

        if (processingOccured) {
            // pIntermediateInput is the output of the last processed effect. It would be the
            // intermediate input of the next effect if there was one.
            if (m_mixMode == EffectChainMixMode::DrySlashWet) {
                // Dry/Wet mode: output = (input * (1-mix knob)) + (wet * mix knob)
                SampleUtil::copy2WithRampingGain(
                        pOut,
                        pIn,
                        1.0f - lastCallbackMixKnob,
                        1.0f - currentMixKnob,
                        pIntermediateInput,
                        lastCallbackMixKnob,
                        currentMixKnob,
                        numSamples);
            } else if (m_mixMode == EffectChainMixMode::WetOnly) {
                // Wet-only mode: output = wet * mix knob. No dry at all.
                // The dry is expected to reach the listener by some other
                // route -- typically an external mixer carrying the same
                // source on its own channel, with this chain fed from an
                // aux send and returned alongside it. Mixing any dry in here
                // would put it in that mixer's sum a second time.
                SampleUtil::copyWithRampingGain(
                        pOut,
                        pIntermediateInput,
                        lastCallbackMixKnob,
                        currentMixKnob,
                        numSamples);
            } else {
                // Dry+Wet mode: output = input + (wet * mix knob)
                SampleUtil::copy2WithRampingGain(
                        pOut,
                        pIn,
                        1.0f,
                        1.0f,
                        pIntermediateInput,
                        lastCallbackMixKnob,
                        currentMixKnob,
                        numSamples);
            }
        }
    }

    if (!processingOccured && m_mixMode == EffectChainMixMode::WetOnly &&
            channelStatus.enableState != EffectEnableState::Disabled) {
        // A wet-only chain contributes wet or it contributes nothing, so with
        // nothing processed -- an empty chain, every effect slot cleared, or
        // the whole unit switched off -- the honest output is silence.
        //
        // Returning false here instead would leave the caller holding the
        // unprocessed input, and for the send/return case that is the worst
        // possible failure: the dry lands in the external mixer's sum a second
        // time, at full level, which is exactly the doubling this mode exists
        // to prevent. Clearing the last effect slot mid-set should go quiet,
        // not +6 dB.
        //
        // The enableState test is what keeps this from silencing channels the
        // chain is not routed to: those settle at Disabled, whereas a routed
        // channel whose unit is merely off settles at Enabling (standby).
        SampleUtil::clear(pOut, numSamples);
        processingOccured = true;
    }

    if (processingOccured) {
        // The master's meter, measured on the chain's actual output -- after
        // the mix and the makeup gain, so it reads what the rack is returning
        // and not what its last module produced.
        const SINT frames = static_cast<SINT>(numSamples) / 2;
        if (frames > 0 && sampleRate.isValid()) {
            const double seconds = static_cast<double>(frames) / sampleRate;
            const auto release = static_cast<CSAMPLE>(std::exp(-seconds / kOutputReleaseSeconds));
            const auto previous = static_cast<CSAMPLE>(m_outputLevel.get());
            // forceSet: see EngineEffect::publishOutputLevel().
            m_outputLevel.forceSet(math_max(previous * release,
                    SampleUtil::maxAbsAmplitude(pOut, numSamples)));
        }
    }

    channelStatus.oldMixKnob = currentMixKnob;

    return processingOccured;
}
