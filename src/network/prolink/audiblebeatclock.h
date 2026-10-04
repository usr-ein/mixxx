#pragma once

#include <QSharedPointer>
#include <QString>
#include <memory>

#include "network/prolink/prolinkbeatposition.h"

class ControlProxy;
class QObject;
class VisualPlayPosition;

namespace mixxx {
namespace prolink {

/// Where a deck is on its beat grid **as it is heard**, now.
///
/// `playposition` is the engine's: written at the end of each audio callback,
/// and ahead of what comes out of the speakers by the output latency -- some
/// 10-20 ms on the deck. Another player's beat packets arrive about when that
/// player's beat is heard. Comparing the two directly put this deck's audio
/// that far behind a CDJ while the phase meter showed them aligned, and told a
/// CDJ following us to play that far ahead of us.
///
/// So the comparison, the meter and what we publish all read this: the
/// engine's position moved to the sample the DAC is playing this instant
/// (VisualPlayPosition, the same arithmetic the waveform uses), less a
/// per-rig trim for what Mixxx cannot see -- the USB codec, and the other
/// player's own offset between its beat packet and its sound. The trim is
/// `[ProLink] phase_trim_ms`, measured once with a two-channel recording of
/// both decks on the same kick: positive when this deck is heard later.
///
/// The beat is found on the track's grid itself, and numbered so that the
/// rekordbox downbeat -- imported as the intro cue -- is beat 1 of a bar; see
/// barPhaseOf().
///
/// GUI thread only.
class AudibleBeatClock {
  public:
    AudibleBeatClock(const QString& group, QObject* pParent);
    ~AudibleBeatClock();

    /// Our place on the grid as heard now. Invalid without a grid.
    BeatPosition now() const;

  private:
    const QString m_group;
    QSharedPointer<VisualPlayPosition> m_pVisualPlayPosition;
    std::unique_ptr<ControlProxy> m_pPlayPosition;
    std::unique_ptr<ControlProxy> m_pDuration;
    std::unique_ptr<ControlProxy> m_pBpm;
    /// Where the intro cue is, which is rekordbox's first downbeat.
    std::unique_ptr<ControlProxy> m_pIntroStart;
    std::unique_ptr<ControlProxy> m_pRateRatio;
    std::unique_ptr<ControlProxy> m_pTrimMs;
};

} // namespace prolink
} // namespace mixxx
