#include "engine/channels/engineaux.h"

#include <QtDebug>

#include "control/controlaudiotaperpot.h"
#include "control/controlproxy.h"
#include "effects/effectsmanager.h"
#include "engine/effects/engineeffectsmanager.h"
#include "engine/effects/groupfeaturestate.h"
#include "moc_engineaux.cpp"
#include "util/sample.h"

EngineAux::EngineAux(const ChannelHandleAndGroup& handleGroup, EffectsManager* pEffectsManager)
        : EngineChannel(handleGroup, EngineChannel::CENTER, pEffectsManager,
                  /*isTalkoverChannel*/ false,
                  /*isPrimaryDeck*/ false),
          m_pInputConfigured(new ControlObject(ConfigKey(getGroup(), "input_configured"))),
          m_pPregain(new ControlAudioTaperPot(ConfigKey(getGroup(), "pregain"), -12, 12, 0.5)) {
    // Make input_configured read-only.
    m_pInputConfigured->setReadOnly();
    m_pInputConfigured->addAlias(ConfigKey(getGroup(), QStringLiteral("enabled")));

    m_pFxBpm = std::make_unique<ControlProxy>(
            QStringLiteral("[EffectTempo]"), QStringLiteral("bpm"));

    // by default Aux is disabled on the main and disabled on PFL. User
    // can over-ride by setting the "pfl" or "main_mix" controls.
    // Skins can change that during initialisation, if the main control is not provided.
    setMainMix(false);
}

EngineAux::~EngineAux() {
    delete m_pPregain;
}

EngineChannel::ActiveState EngineAux::updateActiveState() {
    bool enabled = m_pInputConfigured->toBool();
    if (enabled && m_sampleBuffer) {
        m_active = true;
        return ActiveState::Active;
    }
    if (m_active) {
        m_vuMeter.reset();
        m_active = false;
        return ActiveState::WasActive;
    }
    return ActiveState::Inactive;
}

void EngineAux::onInputConfigured(const AudioInput& input) {
    if (input.getType() != AudioPathType::Auxiliary) {
        // This is an error!
        qDebug() << "WARNING: EngineAux connected to AudioInput for a non-auxiliary type!";
        return;
    }
    m_sampleBuffer = nullptr;
    m_pInputConfigured->forceSet(1.0);
}

void EngineAux::onInputUnconfigured(const AudioInput& input) {
    if (input.getType() != AudioPathType::Auxiliary) {
        // This is an error!
        qDebug() << "WARNING: EngineAux connected to AudioInput for a non-auxiliary type!";
        return;
    }
    m_sampleBuffer = nullptr;
    m_pInputConfigured->forceSet(0.0);
}

void EngineAux::receiveBuffer(
        const AudioInput& input, const CSAMPLE* pBuffer, unsigned int nFrames) {
    Q_UNUSED(input);
    Q_UNUSED(nFrames);
    m_sampleBuffer = pBuffer;
}

void EngineAux::process(CSAMPLE* pOut, const int iBufferSize) {
    const CSAMPLE* sampleBuffer = m_sampleBuffer; // save pointer on stack
    CSAMPLE_GAIN pregain = static_cast<CSAMPLE_GAIN>(m_pPregain->get());
    if (sampleBuffer) {
        SampleUtil::copyWithGain(pOut, sampleBuffer, pregain, iBufferSize);
        EngineEffectsManager* pEngineEffectsManager = m_pEffectsManager->getEngineEffectsManager();
        if (pEngineEffectsManager != nullptr) {
            // Prefader only, which for this channel is the equalizer chain and
            // nothing else. The effect RACK is a StandardEffectChain and runs
            // postfader, in ChannelMixer -- which collects this channel's
            // features on the way and hands them to the effects, so the beat
            // length below reaches the Echo without anything extra here.
            pEngineEffectsManager->processPreFaderInPlace(m_group.handle(),
                    m_pEffectsManager->getMainHandle(),
                    pOut,
                    iBufferSize,
                    mixxx::audio::SampleRate::fromDouble(m_sampleRate.get()));
        }
        m_sampleBuffer = nullptr;
    } else {
        SampleUtil::clear(pOut, iBufferSize);
    }

    // Update VU meter
    m_vuMeter.process(pOut, iBufferSize);
}

void EngineAux::collectFeatures(GroupFeatureState* pGroupFeatures) const {
    m_vuMeter.collectFeatures(pGroupFeatures);

    // Lend the aux a beatgrid it cannot have of its own. What arrives here is
    // several decks summed by a mixer; there is no track, no analysis and no
    // playposition, so the only honest tempo is the one some deck reports it is
    // playing at.
    //
    // Only the length, not the phase: `beat_fraction_buffer_end` is left unset
    // deliberately. Knowing how long a beat is makes Echo's Quantize snap the
    // delay to a musical division, which is the useful part. Knowing *where*
    // the beat is would need the phase of a signal that is a mix of several
    // decks, arriving 32 ms late -- and there is no single right answer to
    // give, so none is given.
    const double bpm = m_pFxBpm ? m_pFxBpm->get() : 0.0;
    if (bpm > 0.0) {
        pGroupFeatures->beat_length = {60.0 / bpm, 1.0};
    }
}
