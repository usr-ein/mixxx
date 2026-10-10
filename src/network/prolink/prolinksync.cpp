#include "network/prolink/prolinksync.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "moc_prolinksync.cpp"
#include "network/prolink/audiblebeatclock.h"
#include "network/prolink/automaster.h"
#include "network/prolink/prolinkbeatposition.h"
#include "network/prolink/prolinkbridge.h"
#include "network/prolink/prolinkcontrols.h"
#include "network/prolink/syncsource.h"
#include "network/prolink/synctempo.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkSync");

/// How close two tempos have to be before the phase is worked on at all.
///
/// Also the tempo-matched test for the hold: the master's effective tempo is
/// its centi-BPM times a fixed-point pitch, and this deck's is whatever the
/// rate slider quantises to, so the two converge to about a hundredth of a BPM
/// and then stop.
constexpr double kTempoMatchedBpm = 0.05;

/// Below this the phase is left alone: about 1.4 ms at 128 BPM, under the
/// jitter in measuring it.
constexpr double kPhaseDeadbandBeats = 0.003;
/// Above this the phase is put back with a seek rather than eased back with
/// the trim: a slip this large (a jog jump on the master, a missed landing)
/// would take the trim longer to close than a DJ will wait.
constexpr double kPhaseSeekBeats = 0.1;
/// How long the trim aims to take to close an error. Two seconds closes 0.02
/// beat at 128 BPM with a 0.5% trim.
constexpr double kPhaseTrimHorizonSeconds = 2.0;
/// The largest trim asked for. The engine clamps to the same.
constexpr double kMaxPhaseTrim = 0.01;

/// How long a fresh claim on tempo master is left unchallenged.
///
/// A deck handing over keeps claiming mastership until its successor has picked
/// it up, and its status drops within ~70 ms of that (S28). This is twenty
/// times that: long enough that a handover is never mistaken for a rival, short
/// enough that a DJ pressing MASTER on the CDJ and watching its light does not
/// notice the gap.
constexpr int kMasterSettleMs = 1500;

/// The shortest gap between two seeks.
///
/// A seek has to have taken effect *and* been measured before the next one is
/// considered, or one error is chased by a burst of overlapping seeks.
constexpr int kPhaseHoldMs = 1500;

/// How many phase-error samples the hold decides on: about half a second of
/// polls.
///
/// One sample is a poor witness. Ours is read up to a buffer after the engine
/// wrote it, the other deck's is extrapolated from a packet whose arrival
/// jitters, and a single sample over the threshold used to trigger a seek of
/// its own size -- turning noise into a real error, corrected back 1.5 s later.
constexpr std::size_t kPhaseSamples = 15;
/// How many of them a periodic correction waits for, and a landing.
constexpr std::size_t kPhaseSamplesToHold = 8;
constexpr std::size_t kPhaseSamplesToLand = 3;
/// Below this a landing is not worth a seek: about 2 ms at 128 BPM.
constexpr double kPhaseLandBeats = 0.005;
/// The median of *samples*, which must not be empty.
double medianOf(std::vector<double> samples) {
    const auto middle = samples.begin() + samples.size() / 2;
    std::nth_element(samples.begin(), middle, samples.end());
    return *middle;
}

/// How long after the last jog bend the phase hold stays off.
///
/// A bend arrives as a stream of encoder deltas with no "released" message,
/// so the hold resumes once the stream has been quiet this long.
constexpr int kBendQuietMs = 600;

/// How long after the DJ lets go before the deck is put back on the beat.
///
/// Long enough for the engine to have processed what the release itself
/// queued -- a slip return, a seek out of a loop -- and for the next poll to
/// read the position that produced, rather than the one from before it.
constexpr int kSettleMs = 150;

/// *player* as SYNC and the phase meter see it; see
/// mixxx::prolink::chooseSyncSource() and chooseMeterDeck().
mixxx::prolink::SyncPeer syncPeerOf(const ::prolink::Player& player) {
    mixxx::prolink::SyncPeer peer;
    peer.number = static_cast<int>(player.number);
    peer.hasStatus = player.has_status;
    peer.statusAgeMs = player.status_age_ms;
    peer.isMaster = player.is_master;
    peer.yieldingTo = static_cast<int>(player.yielding_to);
    peer.isSynced = player.is_synced;
    peer.playingFlag = player.is_playing;
    switch (player.play_state) {
    case ::prolink::PlayState::Playing:
    case ::prolink::PlayState::Looping:
    // The medium was pulled and the deck is looping what it had. Still making
    // sound and still on the grid -- the emergency is the medium's, not the
    // music's.
    case ::prolink::PlayState::Emergency:
        peer.playing = true;
        break;
    case ::prolink::PlayState::CuePlay:
        peer.auditioning = true;
        break;
    default:
        // Paused, cued, searching, spun down, loading, nothing loaded.
        break;
    }
    peer.beatBpm = player.effective_bpm;
    peer.beatAgeMs = player.beat_age_ms;
    peer.statusBpm = player.has_status && player.track_bpm > 0.0
            ? player.track_bpm * (1.0 + player.pitch_percent / 100.0)
            : -1.0;
    peer.barPhase = player.bar_phase;
    peer.barPosition = player.bar_position;
    return peer;
}
} // namespace

namespace mixxx {
namespace prolink {

ProLinkSync::ProLinkSync(const QString& deckGroup,
        ProLinkControls* pControls,
        QObject* pParent)
        : QObject(pParent),
          m_deckGroup(deckGroup),
          m_pControls(pControls) {
    VERIFY_OR_DEBUG_ASSERT(m_pControls) {
        // Nothing to hang the buttons off, and nowhere to publish the master.
        // The network half still runs; the UI half simply does not exist.
        kLogger.warning() << "the [ProLink] controls were never created;"
                          << "the phase meter and the SYNC and MASTER buttons will do nothing";
        return;
    }

    // MASTER **takes and never gives back**, which is how the button behaves on
    // a CDJ: pressing it when you are not master makes you master, pressing it
    // when you are does nothing at all. Mastership only ever leaves a deck
    // because another deck asked for it — there is no "nobody is master" state
    // to toggle back into, and offering one would be a button that silently
    // unsynced every follower on the network.
    //
    // Taking it is a request rather than a decision, because whoever holds it
    // has to hand over first. So `is_master` is published separately and
    // read-only, and it is the one the button lights from.
    connect(m_pControls->takeMaster(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                if (value <= 0) {
                    return;
                }
                // Consumed immediately, because this is a request and not a
                // state. A skin PushButton computes what to emit from the
                // control's *current* value -- (value + 1) % 2 -- so a request
                // left latched at 1 makes the next press emit 0, and MASTER
                // works on every other tap.
                m_pControls->takeMaster()->set(0.0);
                if (!m_pSession) {
                    return;
                }
                if (!m_pSession->is_tempo_master()) {
                    m_pSession->take_tempo_master();
                }
            });

    connect(m_pControls->syncEnabled(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                // Published either way, so the rest of the network can see
                // that this deck is following rather than flying its own
                // tempo -- that is what a CDJ lights its SYNC button from.
                if (m_pSession) {
                    m_pSession->set_synced(value > 0);
                }
                // Once, and only after the tempo has caught up: see
                // followMaster(). Pressing SYNC is "match them and land on
                // their beat", and the two halves cannot happen at once.
                m_alignWhenTempoMatches = value > 0;
            });

    // The deck's own state. Proxies rather than reads, because this runs
    // thirty times a second and a lookup by name each time is a lookup by name
    // thirty times a second.
    const auto deck = [this](const char* item) {
        return std::make_unique<ControlProxy>(m_deckGroup,
                QString::fromLatin1(item),
                this,
                ControlFlag::NoWarnIfMissing);
    };
    m_pDeckBpm = deck("bpm");
    m_pDeckFileBpm = deck("file_bpm");
    m_pDeckPlay = deck("play");
    m_pDeckDuration = deck("duration");
    m_pDeckBeatJump = deck("beatjump");
    m_pDeckPhaseTrim = deck("phase_trim");
    m_pOurBeat = std::make_unique<AudibleBeatClock>(m_deckGroup, this);

    // The DJ's hands on the deck, which the phase hold must leave alone. See
    // holdSuspended().
    m_pDeckPlayLatched = deck("play_latched");
    m_pDeckScratching = deck("scratch2_enable");
    m_pDeckLoopEnabled = deck("loop_enabled");
    m_pDeckSlipEnabled = deck("slip_enabled");
    m_pDeckReverse = deck("reverse");
    // A bend is a turn of the jog's side without touching its top: no state
    // control says it is happening, only `jog` receiving deltas. The mapping
    // writes a delta per encoder message and the engine reads it back to 0, so
    // only a non-zero value is a hand on the wheel.
    m_pDeckJog = deck("jog");
    m_pDeckJog->connectValueChanged(this, [this](double value) {
        if (value != 0.0) {
            m_lastBend.start();
        }
    });
}

ProLinkSync::~ProLinkSync() = default;

void ProLinkSync::setSession(::prolink::Session* pSession) {
    m_pSession = pSession;
}

void ProLinkSync::update() {
    // The master first: what SYNC follows is settled there.
    publishMaster();
    publishPlayback();
    followMaster();
}

void ProLinkSync::sessionStopped() {
    // **The published master goes with the session.** Only update() ever
    // rewrote these, and the service stops polling before it closes the
    // session, so a refresh -- or a restart that then failed to bind -- left
    // the meter drawing the last master it had seen, frozen, under that deck's
    // number; MASTER lit for a claim the closed session no longer held; and
    // SYNC following a tempo nobody was playing.
    m_masterSince.invalidate();
    m_alignWhenTempoMatches = false;
    // A deck left playing a touch fast for a network that is gone.
    setPhaseTrim(0.0);
    m_autoClaimed = false;
    m_eligibleForAutoClaim.invalidate();
    if (m_pControls) {
        clearMaster();
    }
}

void ProLinkSync::setLoadedTrack(int sourcePlayer,
        MediaSlot slot,
        quint32 rekordboxId) {
    // Kept, and stated again on every poll by publishPlayback(), rather than
    // handed to the session once. **Once was lost more often than not.** The
    // session drops the call until it holds a player number, which takes about
    // five seconds after start(); and it builds a new virtual CDJ -- with
    // nothing loaded -- every time it rebinds, which on the deck is every time
    // the link cable goes in after boot. Either way the status went on stating
    // a tempo and a playing deck with no track, and a CDJ neither follows nor
    // draws a phase for that.
    m_loadedTrack.sourcePlayer = sourcePlayer;
    m_loadedTrack.slot = slot;
    m_loadedTrack.rekordboxId = rekordboxId;
}

void ProLinkSync::publishLoadedTrack() {
    // Our own number is looked up now rather than when the track was loaded:
    // it is not known for the first seconds, and a rebind may change it.
    int player = m_loadedTrack.sourcePlayer;
    if (player == kThisPlayer) {
        player = static_cast<int>(m_pSession->device_number());
    }
    if (player <= 0 || m_loadedTrack.rekordboxId == 0) {
        m_pSession->set_loaded_track(0, ::prolink::Slot::None, 0);
        return;
    }
    m_pSession->set_loaded_track(static_cast<quint8>(qBound(0, player, 255)),
            toRustSlot(m_loadedTrack.slot),
            m_loadedTrack.rekordboxId);
}

void ProLinkSync::publishPlayback() {
    if (!m_pControls) {
        return;
    }

    // Restated every poll for the same reason as the loaded track: a rebuilt
    // session starts unsynced, and the button did not change to say otherwise.
    publishLoadedTrack();
    m_pSession->set_synced(m_pControls->syncEnabled()->get() > 0.0);

    const double fileBpm = m_pDeckFileBpm->get();
    const double duration = m_pDeckDuration->get();
    if (fileBpm <= 0.0 || duration <= 0.0) {
        // No track, or one with no grid. Saying nothing is right: a tempo we
        // cannot state is not a tempo of zero, and a stale one would leave
        // followers locked to a ghost.
        //
        // **Unless the deck has a tempo all the same**, which is a track whose
        // BPM never reached `file_bpm` -- the state every rekordbox track
        // the browser loaded was in, silently, until Track learnt to recompute
        // its BPM once the duration is known. Said once, because it takes this
        // deck off the network, and nothing else would say so.
        if (m_pDeckBpm->get() > 0.0 && !m_warnedNoFileBpm) {
            m_warnedNoFileBpm = true;
            kLogger.warning() << "the deck has a tempo of" << m_pDeckBpm->get()
                              << "BPM but file_bpm is" << fileBpm << "and duration"
                              << duration << "-- telling the network nothing";
        }
        m_pSession->clear_playback();
        return;
    }
    m_warnedNoFileBpm = false;
    // The fader as a percentage, derived from the two tempos rather than read
    // off `rate`: rate has to be combined with the range and the direction, and
    // getting any of the three wrong is a pitch that is silently inverted.
    const double effectiveBpm = m_pDeckBpm->get();
    const double pitchPercent = effectiveBpm > 0.0 ? (effectiveBpm / fileBpm - 1.0) * 100.0 : 0.0;

    // As heard, not as the engine has it: a CDJ following us lines its beat
    // up with our packets, so they have to leave on our *audible* beat.
    const mixxx::prolink::BeatPosition position = m_pOurBeat->now();
    m_pSession->set_playback(fileBpm,
            pitchPercent,
            m_pDeckPlay->get() > 0.0,
            // `play` stays up while the jog is held to scratch.
            m_pDeckScratching->get() > 0.0,
            position.number,
            position.fraction);
}

void ProLinkSync::followMaster() {
    if (!m_pControls) {
        return;
    }

    // Whether the DJ has the deck. Tracked whatever the sync state, so the
    // release is seen even if SYNC was pressed in the meantime.
    const bool suspended = holdSuspended();
    if (m_holdWasSuspended && !suspended) {
        // **Back on the beat after the DJ lets go** -- of the jog, a loop,
        // slip, a preview, or the deck's stop. Not at once: see kSettleMs.
        m_alignWhenTempoMatches = true;
        m_holdSettle.start();
    }
    m_holdWasSuspended = suspended;

    // Which of the eight states this is, and therefore whether the tempo comes
    // off the wire or off the fader. The table and the reasoning are in
    // docs/tempo-sync.md; the decision itself is in SyncTempo so that every row
    // of that table is a test rather than a branch nobody can find.
    //
    // Nothing to do in the Fader case: not writing the tempo *is* letting the
    // fader have it. Catching up afterwards is the mapping's pickup
    // (TriMixxx.scripts.js, "Tempo fader"), which reads [ProLink],following.
    SyncTempo::State state;
    state.syncEnabled = m_pControls->syncEnabled()->get() > 0.0;
    state.isMaster = m_pControls->isMaster()->get() > 0.0;
    state.masterBpm = m_syncSource.bpm;

    // **The master pausing ends the follow** (owner decision 15). The tempo it
    // was being followed at carries on as this deck's own, the fader has to
    // catch up with it, and SYNC goes dark: the deck follows again only when
    // the DJ presses SYNC. Following on would mean following whatever that
    // master does while nobody can hear it -- a new track loaded, its pitch
    // fader moved -- and jumping to it on air the moment it plays again.
    if (state.syncEnabled && !state.isMaster && m_syncSource.masterStopped &&
            m_followingDevice != 0 && m_followingDevice == m_syncSource.device) {
        kLogger.info() << "player" << m_followingDevice
                       << "stopped while we followed it; releasing SYNC";
        m_pControls->syncEnabled()->set(0.0);
        if (m_pSession) {
            m_pSession->set_synced(false);
        }
        state.syncEnabled = false;
    }

    const bool following = SyncTempo::decide(state) == SyncTempo::Source::Master;
    m_followingDevice = following ? m_syncSource.device : 0;
    m_pControls->following()->forceSet(following ? 1.0 : 0.0);
    if (!following) {
        setPhaseTrim(0.0);
        return;
    }
    const double masterBpm = state.masterBpm;
    const double ours = m_pDeckBpm->get();
    if (ours <= 0.0) {
        return;
    }
    // **The tempo is the master's, exactly, and is not used to steer.**
    //
    // An earlier version held the phase by trimming the `bpm` a fraction of a
    // percent. It rewrote the tempo thirty times a second, and the deck's own
    // BPM readout jittered around the master's value for as long as SYNC was
    // lit -- worse than the drift, because it is the number a DJ reads to
    // decide whether the two decks agree. So `bpm` is set once and left alone.
    // The phase is landed with a beat jump and held with the engine's
    // phase_trim, which the readout never sees; see below.
    //
    // A deadband rather than equality, because the master's effective tempo is
    // its centi-BPM times a fixed-point pitch and this deck's is whatever the
    // rate slider quantises to: the two converge to about a hundredth of a BPM
    // and then stop.
    const bool tempoMatched = std::abs(masterBpm - ours) < kTempoMatchedBpm;
    if (!tempoMatched) {
        m_pDeckBpm->set(masterBpm);
    }

    // **Only while the deck is running on its own.** A paused deck's playhead
    // does not move, so the master walks away from it and the error grows
    // without bound; and a deck under the DJ's hands -- scratched, bent,
    // looped, slipping, previewed -- is being moved on purpose. See
    // holdSuspended().
    const bool settled = !m_holdSettle.isValid() || m_holdSettle.elapsed() > kSettleMs;
    if (!tempoMatched || suspended || !settled) {
        // Whatever was measured before is about a deck that has since been
        // moved, by hand or by a tempo change; and nothing is eased while the
        // DJ has the deck.
        m_phaseSamples.clear();
        setPhaseTrim(0.0);
        reportPhaseDrift();
        return;
    }
    // **A sync that aligns once is not a sync.** Landing on the beat when SYNC
    // is pressed or the deck starts is the easy half; the two then drift apart
    // whenever the master's tempo is nudged, and something has to close that
    // gap again. A CDJ holds the phase for as long as SYNC is lit.
    double error = 0.0;
    if (phaseErrorBeats(&error)) {
        if (m_phaseSamples.size() == kPhaseSamples) {
            m_phaseSamples.erase(m_phaseSamples.begin());
        }
        m_phaseSamples.push_back(error);
    }
    const bool due = !m_phaseHold.isValid() || m_phaseHold.elapsed() > kPhaseHoldMs;
    const auto seek = [this](double beats) {
        setPhaseTrim(0.0);
        m_phaseHold.start();
        // What was measured is from before the move.
        m_phaseSamples.clear();
        alignPhaseToMaster(beats);
    };
    if (m_alignWhenTempoMatches) {
        // **Landing: one exact seek.** Pressing SYNC or starting the deck is
        // the moment a jump is expected, and the error can be half a beat.
        if (m_phaseSamples.size() >= kPhaseSamplesToLand) {
            m_alignWhenTempoMatches = false;
            const double median = medianOf(m_phaseSamples);
            if (std::abs(median) > kPhaseLandBeats) {
                seek(median);
            }
        }
    } else if (m_phaseSamples.size() >= kPhaseSamplesToHold) {
        // **Holding: ease it back.** A seek mid-mix is a flam of its own, so a
        // small error is closed by playing a touch fast or slow for a moment --
        // inside the engine, where the BPM read-out never sees it. Only a slip
        // too large for that gets a seek.
        const double median = medianOf(m_phaseSamples);
        if (std::abs(median) > kPhaseSeekBeats && due) {
            seek(median);
        } else if (std::abs(median) > kPhaseDeadbandBeats && ours > 0.0) {
            // Positive error: the followed deck is ahead, so play faster.
            const double beatSeconds = 60.0 / ours;
            setPhaseTrim(std::clamp(median * beatSeconds / kPhaseTrimHorizonSeconds,
                    -kMaxPhaseTrim,
                    kMaxPhaseTrim));
        } else {
            setPhaseTrim(0.0);
        }
    }
    reportPhaseDrift();
}

void ProLinkSync::setPhaseTrim(double trim) {
    if (trim == m_phaseTrim) {
        return;
    }
    m_phaseTrim = trim;
    if (m_pDeckPhaseTrim) {
        m_pDeckPhaseTrim->set(trim);
    }
}

bool ProLinkSync::holdSuspended() const {
    // `play_latched`, not `play`: a cue or hot cue held while paused plays a
    // preview with `play` up and `play_latched` down. Landing on the beat
    // then would move the preview off the cue the DJ is listening to -- and
    // could, before the cue's own seek had been processed, send it back to
    // where the deck stood before the press.
    if (m_pDeckPlayLatched->get() <= 0.0) {
        return true;
    }
    if (m_pDeckScratching->get() > 0.0 || m_pDeckLoopEnabled->get() > 0.0 ||
            m_pDeckSlipEnabled->get() > 0.0 || m_pDeckReverse->get() > 0.0) {
        return true;
    }
    return m_lastBend.isValid() && m_lastBend.elapsed() < kBendQuietMs;
}

bool ProLinkSync::phaseErrorBeats(double* pBeats) const {
    // **Only against a deck that is playing.** A stopped one is drawn where its
    // status says it stands, to the nearest beat: a place to show, not a phase
    // to lock to. Measuring a playing deck against it reads as an error that
    // grows without bound, and correcting it would drag our playhead every
    // second and a half towards a deck standing still.
    if (!m_syncSource.phaseLive) {
        return false;
    }
    // Its next beat is due and not here yet: its phase is standing still at
    // the end of the beat while ours moves on.
    if (m_syncSource.beatOverdue) {
        return false;
    }
    const double masterPhase = m_syncSource.barPhase;
    // Ours as heard, against theirs as heard: see AudibleBeatClock.
    const double ourPhase = mixxx::prolink::barPhaseOf(m_pOurBeat->now());
    if (masterPhase < 0.0 || ourPhase < 0.0) {
        return false;
    }
    // Wrapped to the nearest **beat**, not the nearest bar (owner decision
    // 14): lining the bars up would drag the track by up to two beats, and
    // which bar the mix starts on is the DJ's call.
    double beats = (masterPhase - ourPhase) * mixxx::prolink::kBeatsPerBar;
    beats -= std::floor(beats);
    if (beats > 0.5) {
        beats -= 1.0;
    }
    *pBeats = beats;
    return true;
}

void ProLinkSync::reportPhaseDrift() {
    // **Whether the sync is actually holding**, once a second, in the only
    // units that mean anything here: milliseconds between our beat and the
    // master's. A tempo that matches and a phase that does not is the failure
    // this whole path exists to prevent, and it is invisible from the numbers
    // on screen -- both decks show the same BPM while sounding like a flam.
    if (m_driftReport.isValid() && m_driftReport.elapsed() < 1000) {
        return;
    }
    double beats = 0.0;
    const double effectiveBpm = m_pDeckBpm->get();
    if (!phaseErrorBeats(&beats) || effectiveBpm <= 0.0) {
        return;
    }
    m_driftReport.start();
    // Both sides of the subtraction, not only the difference. A drift that will
    // not close is either a master phase that is not moving or a correction
    // that is not landing, and the difference alone cannot tell those apart --
    // which cost three rounds of reasoning about a number that turned out to be
    // measured against the wrong tempo.
    kLogger.debug() << "phase drift" << beats * 60000.0 / effectiveBpm << "ms ("
                    << beats << "beats ) -- followed" << m_syncSource.barPhase
                    << "ours"
                    << mixxx::prolink::barPhaseOf(m_pOurBeat->now());
}

void ProLinkSync::alignPhaseToMaster(double beats) {
    // **A beat jump, not a seek.** The correction used to be written to
    // `playposition`, and the deck never moved: a `playposition` write is a
    // standard seek, quantize (on, on this deck) turns it into a
    // phase-preserving one, and with no other Mixxx deck playing the engine
    // matches the deck against its own phase from before the seek -- so every
    // correction under half a beat landed exactly where it started.
    //
    // `beatjump` is Mixxx's own exact move by a number of beats, fractions
    // included, on the deck's grid, and it does not consult quantize. Inside a
    // loop it would move the loop instead; holdSuspended() keeps the hold off
    // in a loop, so it is never asked to.
    //
    // **Beats, not bars.** *beats* is already wrapped into half a beat either
    // way (phaseErrorBeats()): bar alignment across devices is not something
    // this corrects, so the playhead never moves further than that.
    m_pDeckBeatJump->set(beats);
    kLogger.debug() << "phase align: jumping" << beats
                    << "beats onto the followed deck's beat";
}

bool ProLinkSync::reconcileMastership(int rivalMaster) {
    if (!m_pSession->is_tempo_master()) {
        if (m_masterSince.isValid()) {
            // Lost since the last poll: handed over, or a rebind. Either way
            // not to be taken straight back by an auto-claim.
            m_lastStoodDown.start();
        }
        m_masterSince.invalidate();
        m_autoClaimed = false;
        return false;
    }
    if (!m_masterSince.isValid()) {
        m_masterSince.start();
    }
    if (rivalMaster == 0) {
        return true;
    }
    // **An auto-claim yields at once**, to anyone: it was only ever filling an
    // empty mastership, and a deck that claims it -- a CDJ whose MASTER was
    // pressed, another TriMixxx that got there first -- has the better claim.
    // A collision makes the next auto-claim wait longer; see automaster.
    if (m_autoClaimed) {
        kLogger.info() << "player" << rivalMaster << "claims tempo master; our auto-claim yields";
        m_autoClaimCollisions = std::min(m_autoClaimCollisions + 1, automaster::kMaxCollisions);
        standDown();
        return false;
    }
    // **Not during a handover.** A deck handing mastership over keeps claiming
    // it until its successor has picked it up, so for a moment after winning a
    // takeover the deck we took it FROM is still saying it is master. Standing
    // down on that would abandon, one poll later, the takeover just won.
    if (m_masterSince.elapsed() < kMasterSettleMs) {
        return true;
    }
    // Past the handover window with somebody else still claiming it: the
    // network has settled on a master and it is not this deck.
    kLogger.info() << "player" << rivalMaster << "holds tempo master; standing down";
    standDown();
    return false;
}

void ProLinkSync::standDown() {
    m_pSession->release_tempo_master();
    m_masterSince.invalidate();
    m_autoClaimed = false;
    m_lastStoodDown.start();
}

bool ProLinkSync::manageMasterLikeACdj(
        const std::vector<mixxx::prolink::SyncPeer>& peers, int ours, bool weAreMaster) {

    // Owner decision 13, first half: a deck handing master to us unasked --
    // a CDJ master stopping while our synced deck plays on -- is taken up.
    if (!weAreMaster &&
            std::any_of(peers.begin(), peers.end(), [ours](const auto& peer) {
                return mixxx::prolink::isHeardMasterClaim(peer, ours) &&
                        peer.yieldingTo == ours;
            }) &&
            m_pSession->accept_tempo_master()) {
        return true;
    }

    // `play_latched`, not `play`: a cue held as a preview is not the deck
    // playing, and must not hand master over or claim it.
    const bool deckPlaying = m_pDeckPlayLatched->get() > 0.0;
    // Second half: while our deck is stopped and we are master, hand master to
    // a deck that plays, as a CDJ does: a synced one only if we are synced
    // (automaster::successorWhenStopped). Held for as long as we stay
    // stopped, so a deck that starts later, or our SYNC going off, hands it
    // over too; but never offered twice to one deck in one stop. If nobody
    // picks it up we keep it -- an empty mastership is worse.
    if (deckPlaying) {
        m_stopOffers.reset();
    } else if (weAreMaster) {
        const bool weAreSynced = m_pControls->syncEnabled()->get() > 0.0;
        const int successor = m_stopOffers.next(peers, ours, weAreSynced);
        if (successor != 0 &&
                m_pSession->offer_tempo_master(static_cast<std::uint8_t>(successor))) {
            kLogger.info() << "our deck stopped; offering tempo master to player" << successor;
        }
    }

    // Owner decisions 2 and 3: with no master on the network, a deck that is
    // playing and following nobody takes it, after a delay that orders decks
    // by number. See automaster::claimDelayMs().
    const bool anyClaim = std::any_of(peers.begin(), peers.end(), [ours](const auto& peer) {
        return mixxx::prolink::isHeardMasterClaim(peer, ours);
    });
    automaster::ClaimInputs inputs;
    inputs.ours = ours;
    inputs.weAreMaster = weAreMaster;
    inputs.anyClaim = anyClaim;
    inputs.playingWithTempo = deckPlaying && m_pDeckFileBpm->get() > 0.0;
    inputs.following = m_followingDevice != 0;
    inputs.holdingOff = m_lastStoodDown.isValid() &&
            m_lastStoodDown.elapsed() < automaster::kHoldOffMs;
    if (anyClaim && !weAreMaster) {
        // A settled master: past collisions no longer say anything.
        m_autoClaimCollisions = 0;
    }
    if (!automaster::mayClaim(inputs)) {
        m_eligibleForAutoClaim.invalidate();
        return weAreMaster;
    }
    if (!m_eligibleForAutoClaim.isValid()) {
        m_eligibleForAutoClaim.start();
    }
    if (m_eligibleForAutoClaim.elapsed() <
            automaster::claimDelayMs(ours, m_autoClaimCollisions)) {
        return weAreMaster;
    }
    kLogger.info() << "nobody holds tempo master and our deck is playing; taking it";
    m_eligibleForAutoClaim.invalidate();
    m_pSession->take_tempo_master();
    m_autoClaimed = m_pSession->is_tempo_master();
    return m_autoClaimed;
}

void ProLinkSync::publishMaster() {
    if (!m_pControls) {
        return;
    }

    if (!m_pSession) {
        clearMaster();
        return;
    }

    const ::rust::Vec<::prolink::Player> players = m_pSession->players();

    // **Exactly one device is tempo master** -- invariant 1 of
    // docs/tempo-sync.md, and until now nothing enforced it.
    //
    // Our claim was ours to set and nobody else's to clear, so it outlived
    // every way a handover can fail to reach us: a master request lost on the
    // wire, a `0x27` reply the requester never heard, or a CDJ that simply
    // asserts mastership instead of asking for it. In each case the network has
    // settled on a master and it is not this deck -- but this deck went on
    // saying it was, which from the booth is "the CDJ cannot take master back",
    // and which never healed on its own because nothing ever asked again.
    //
    // So it is asked every poll: who else is claiming it?
    const int ours = static_cast<int>(m_pSession->device_number());
    std::vector<mixxx::prolink::SyncPeer> peers;
    peers.reserve(players.size());
    for (const ::prolink::Player& player : players) {
        peers.push_back(syncPeerOf(player));
    }
    int rivalMaster = 0;
    for (const auto& peer : peers) {
        // **Only a claim we are still hearing.** A deck that has gone -- cable
        // pulled, powered off -- keeps its last status, mastership included,
        // until it is forgotten some 30 s later. Believing it, MASTER asked a
        // deck that was not there and KEY SYNC offered its key.
        if (!mixxx::prolink::isHeardMasterClaim(peer, ours)) {
            continue;
        }
        if (peer.yieldingTo == ours) {
            // Naming us its successor: a takeover of ours in flight, not a
            // rival claim (F52).
            continue;
        }
        // Handing over to another deck: that deck is the master, a packet or
        // two before its own status says so.
        rivalMaster = peer.yieldingTo != 0 ? peer.yieldingTo : peer.number;
        break;
    }
    bool weAreMaster = reconcileMastership(rivalMaster);
    weAreMaster = manageMasterLikeACdj(peers, ours, weAreMaster);
    // Published here rather than beside the playback, because that returns
    // early when no track is loaded -- and holding tempo master with the deck
    // stopped is an ordinary state whose button must not go dark.
    m_pControls->isMaster()->forceSet(weAreMaster ? 1.0 : 0.0);

    // What the master is playing, for KEY SYNC. Literal, and unrelated to the
    // deck chosen for the phase meter below: a key is borrowed from whoever
    // the room is following, and if that is nobody there is no key to borrow.
    MasterTrack masterTrack;
    if (!weAreMaster && rivalMaster != 0) {
        for (const ::prolink::Player& player : players) {
            if (static_cast<int>(player.number) != rivalMaster) {
                continue;
            }
            masterTrack.masterPlayer = rivalMaster;
            // The track's home, which is not the same player: on a linked rig
            // one stick feeds four decks, and the id is a row in *that*
            // medium's database and means nothing anywhere else.
            masterTrack.sourcePlayer = static_cast<int>(player.track_source_player);
            masterTrack.slot = toMixxxSlot(player.track_source_slot);
            // Only a rekordbox track's id is a row in that medium's database.
            // An unanalysed file's is the player's own numbering, and looked
            // up there it named an unrelated track whose key KEY SYNC then
            // offered and latched. No id: no key, KEY SYNC stays dark.
            masterTrack.trackId = player.track_is_rekordbox ? player.track_id : 0;
            break;
        }
    }
    publishMasterTrack(masterTrack);

    // **What SYNC follows, chosen on its own.** It used to be whatever the
    // phase meter drew, and the meter's choice is a different question -- it
    // will show a deck being cued up, which nobody asked to follow. See
    // chooseSyncSource() for the rules.
    m_syncSource = mixxx::prolink::chooseSyncSource(peers, ours);
    if (m_syncSource.device != m_sampledDevice) {
        // A different deck's phase: what was measured against the last one
        // says nothing about this one.
        m_sampledDevice = m_syncSource.device;
        m_phaseSamples.clear();
    }

    // **Who to draw is "the deck I am mixing against", not literally "the
    // master".** The two are the same until this deck takes mastership, and
    // then they stop being: the master becomes us, and the meter would go
    // blank at exactly the moment a DJ has just declared they are the one
    // being followed. See chooseMeterDeck() for the rules.
    publishMeter(mixxx::prolink::chooseMeterDeck(peers, ours));
}

void ProLinkSync::publishMeter(const mixxx::prolink::MeterDeck& deck) {
    m_pControls->masterDevice()->forceSet(deck.device);
    m_pControls->meterIsMaster()->forceSet(deck.isMaster ? 1.0 : 0.0);
    m_pControls->meterLive()->forceSet(deck.live ? 1.0 : 0.0);
    m_pControls->masterBarPhase()->forceSet(deck.barPhase);
}

void ProLinkSync::clearMaster() {
    m_syncSource = mixxx::prolink::SyncSource();
    m_sampledDevice = 0;
    m_pControls->following()->forceSet(0.0);
    m_phaseSamples.clear();
    m_pControls->isMaster()->forceSet(0.0);
    publishMeter(mixxx::prolink::MeterDeck());
    publishMasterTrack(MasterTrack());
}

void ProLinkSync::publishMasterTrack(const MasterTrack& track) {
    if (track == m_publishedMasterTrack) {
        return;
    }
    m_publishedMasterTrack = track;
    // On a change and not on a poll: resolving this to a key means a database
    // lookup, and thirty of those a second for an answer that moves once a
    // mix would be thirty times a second of nothing.
    emit masterTrackChanged(track.masterPlayer,
            track.sourcePlayer,
            track.slot,
            track.trackId);
}

} // namespace prolink
} // namespace mixxx
