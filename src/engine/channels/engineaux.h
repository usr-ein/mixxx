#pragma once

#include <QScopedPointer>
#include <memory>

#include "engine/channels/enginechannel.h"
#include "soundio/soundmanagerutil.h"

class ControlAudioTaperPot;
class ControlProxy;

/// EngineAux is an EngineChannel that implements a mixing source whose
/// samples are fed directly from the SoundManager
class EngineAux : public EngineChannel, public AudioDestination {
    Q_OBJECT
  public:
    EngineAux(const ChannelHandleAndGroup& handleGroup, EffectsManager* pEffectsManager);
    ~EngineAux() override;

    ActiveState updateActiveState() override;

    /// Called by EngineMixer whenever is requesting a new buffer of audio.
    void process(CSAMPLE* pOutput, const int iBufferSize) override;
    void collectFeatures(GroupFeatureState* pGroupFeatures) const override;

    /// This is called by SoundManager whenever there are new samples from the
    /// configured input to be processed. This is run in the callback thread of
    /// the soundcard this AudioDestination was registered for! Beware, in the
    /// case of multiple soundcards, this method is not re-entrant but it may be
    /// concurrent with EngineMixer processing.
    void receiveBuffer(const AudioInput& input,
            const CSAMPLE* pBuffer,
            unsigned int nFrames) override;

    /// Called by SoundManager whenever the aux input is connected to a
    /// soundcard input.
    void onInputConfigured(const AudioInput& input) override;

    /// Called by SoundManager whenever the aux input is disconnected from
    /// a soundcard input.
    void onInputUnconfigured(const AudioInput& input) override;

  private:
    QScopedPointer<ControlObject> m_pInputConfigured;
    ControlAudioTaperPot* m_pPregain;
    /// The tempo the effect rack runs at, chosen from whichever deck has been
    /// playing longest. See ProLinkNetworkService::publishEffectTempo.
    ///
    /// A live input has no beatgrid of its own -- there is nothing to analyse
    /// and nothing to seek -- so without this every beat-aware effect on the
    /// aux falls back to wall-clock seconds. Echo's Quantize and Triplets read
    /// as doing nothing at all, because the branch they need is only taken when
    /// `beat_length` is set.
    std::unique_ptr<ControlProxy> m_pFxBpm;
};
