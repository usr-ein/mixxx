#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <memory>
#include <vector>

#include "network/prolink/prolinktypes.h"
#include "network/prolink/syncsource.h"

class ControlProxy;

namespace prolink {
/// The Rust session, opaque here as it is to every header.
struct Session;
} // namespace prolink

namespace mixxx {
namespace prolink {

class AudibleBeatClock;
class ProLinkControls;

/// Tempo sync, as a CDJ does it.
///
/// Who holds tempo master (reconciled with the network, given up, claimed
/// when nobody holds it, handed over when our deck stops); what this deck
/// tells the network it is playing (tempo, beat, loaded track); SYNC
/// following the master (the tempo, then the phase: a landing, a hold, a
/// trim); and the deck the phase meter draws. docs/tempo-sync.md is its
/// specification, and syncsource, synctempo and automaster hold the rules it
/// decides with.
///
/// ProLinkNetworkService owns the session and drives this: it hands the
/// session over when it opens one, calls update() on every poll, and takes
/// the session back and calls sessionStopped() when it closes it. All of it
/// on the GUI thread.
class ProLinkSync : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkSync(QObject* pParent = nullptr);
    ~ProLinkSync() override;

    /// The session to act on, or null when there is none. Not owned.
    void setSession(::prolink::Session* pSession);

    /// Settle tempo master, tell the network what this deck plays, and follow
    /// the master: once per poll, while there is a session.
    void update();

    /// The session has closed: publish that there is no master, and let go of
    /// everything that was about it.
    void sessionStopped();

    /// Say which track this deck has loaded, and whose medium it came from.
    ///
    /// A player number of zero means nothing is loaded, and `kThisPlayer`
    /// means our own medium, under whatever number we hold when it is
    /// published. See `MediaRegistry::announceLoadedTrack` for why a tempo
    /// without this is ignored by every other player.
    ///
    /// Remembered rather than passed straight through, and safe to call with
    /// no session: see publishLoadedTrack().
    void setLoadedTrack(int sourcePlayer, MediaSlot slot, quint32 rekordboxId);

  signals:
    /// What the deck holding tempo master has loaded, whenever it changes.
    ///
    /// *masterPlayer* is the player number of the deck holding it, or **0 when
    /// that deck is us or nobody holds it**. It is not `[ProLink] master_device`
    /// and must not be confused with it: that one falls back to other decks,
    /// because a phase meter with nothing to draw is worse than one drawing
    /// the deck you are actually mixing against. This one is literal,
    /// because KEY SYNC is about the master and only about the master.
    ///
    /// The other three name the track: which player's medium it came from,
    /// which of that player's slots, and its rekordbox id. All zero when the
    /// master has nothing loaded — a real state, and one nothing can be synced
    /// to.
    void masterTrackChanged(int masterPlayer,
            int sourcePlayer,
            mixxx::prolink::MediaSlot slot,
            quint32 trackId);

  private:
    /// Settle who holds tempo master, then publish what follows from it: our
    /// MASTER button, the master's track for KEY SYNC, what SYNC follows,
    /// and the deck the phase meter draws.
    void publishMaster();
    /// The deck the phase meter draws: `[ProLink] master_device`,
    /// `meter_is_master`, `meter_live` and `master_bar_phase`, which the
    /// phase-meter widget reads. Read-only, because nothing in Mixxx may tell
    /// a CDJ what phase it is at.
    void publishMeter(const mixxx::prolink::MeterDeck& deck);

    /// Publish that there is no master and we are not it. For a session that
    /// has stopped, which has no players to say so.
    void clearMaster();

    /// State the track setLoadedTrack() was last given. Every poll, because
    /// the session forgets it: see setLoadedTrack().
    void publishLoadedTrack();

    /// What setLoadedTrack() was last given.
    struct LoadedTrack {
        int sourcePlayer = 0;
        MediaSlot slot = MediaSlot::Empty;
        quint32 rekordboxId = 0;
    };
    LoadedTrack m_loadedTrack;

    /// What `masterTrackChanged` last carried, so it is emitted on a change
    /// rather than thirty times a second.
    struct MasterTrack {
        int masterPlayer = 0;
        int sourcePlayer = 0;
        MediaSlot slot = MediaSlot::Empty;
        quint32 trackId = 0;

        bool operator==(const MasterTrack& other) const {
            return masterPlayer == other.masterPlayer &&
                    sourcePlayer == other.sourcePlayer && slot == other.slot &&
                    trackId == other.trackId;
        }
    };
    MasterTrack m_publishedMasterTrack;

    /// Emit `masterTrackChanged` if *track* is not what was last said.
    void publishMasterTrack(const MasterTrack& track);

    /// Reconcile our claim on tempo master with the network's, and return
    /// whether we really hold it.
    ///
    /// **Exactly one device is tempo master** (docs/tempo-sync.md, invariant 1)
    /// and this is the only thing that enforces it. Our claim was ours alone to
    /// clear, so it survived every way a handover can fail to reach us — and a
    /// deck that goes on claiming mastership the network has moved on from is
    /// a deck the CDJ cannot take master back from.
    ///
    /// *rivalMaster* is the number of another player claiming it, or zero.
    /// A number rather than the player, so the Rust bridge's types stay out of
    /// this header as everything else about them does.
    bool reconcileMastership(int rivalMaster);

    /// Give up our claim on tempo master, and remember when.
    void standDown();

    /// Take and hand over tempo master on our own, as a CDJ does: take up a
    /// handover another deck offers us, offer it to a synced deck playing on
    /// when our deck stops, and claim it when the network has none. Returns
    /// whether we now hold it. See automaster.
    bool manageMasterLikeACdj(const std::vector<SyncPeer>& peers, int ours, bool weAreMaster);
    /// Whether our claim was an auto-claim, which yields to any rival at once.
    bool m_autoClaimed = false;
    /// Auto-claims that met a rival, for the back-off; reset by a settled
    /// master.
    int m_autoClaimCollisions = 0;
    /// Since every condition for an auto-claim has held; invalid otherwise.
    QElapsedTimer m_eligibleForAutoClaim;
    /// Since we last stood down or handed over; see automaster::kHoldOffMs.
    QElapsedTimer m_lastStoodDown;
    /// Whether this stop of our deck has already offered master to someone.
    bool m_offeredSinceStop = false;

    /// Tell the network what this deck is playing.
    ///
    /// **The only thing that makes us a tempo other players can see.**
    /// Everything else here reads the network; without this a CDJ lists us as a
    /// device with no tempo, has nothing to beat-match to, and draws no phase
    /// for us however well the rest works.
    void publishPlayback();

    /// Hold this deck's tempo to the network master's, while SYNC is on.
    void followMaster();

    /// Say how far our beat is from the master's, once a second, while synced.
    void reportPhaseDrift();

    /// The master's beat minus ours, in beats, wrapped to plus or minus half a
    /// beat. False when there is nothing to compare: no master, or no grid
    /// here.
    bool phaseErrorBeats(double* pBeats) const;

    /// Move the playhead by *beats* so our beat lands on the followed deck's.
    ///
    /// *beats* is a median of phaseErrorBeats()'s answers: within half a beat
    /// either way. The landing when SYNC is pressed or the deck starts, and a
    /// slip too large for the trim to ease back; smaller errors are eased with
    /// setPhaseTrim() instead. SYNC holds the phase for as long as it is lit.
    void alignPhaseToMaster(double beats);

    /// The deck the browser and the network both mean by "this deck".
    static const char* kDeckGroup;

    /// Set when SYNC is engaged, and when the DJ lets go of the deck (which
    /// includes the deck starting to play): see holdSuspended(). Cleared once
    /// the landing is decided, with a jump or without one. A SYNC press while
    /// the deck is under the DJ's hands waits for the release.
    ///
    /// The two halves of a sync cannot happen at the same moment: matching the
    /// tempo takes a poll or two, and a phase alignment applied before that has
    /// landed is measured at the old tempo and walked away from by the new one.
    bool m_alignWhenTempoMatches = false;
    /// The last few phase errors, which a correction is decided on. See
    /// kPhaseSamples. Cleared whenever they stop describing the deck: a
    /// correction, the DJ's hands, a tempo change, another deck followed.
    std::vector<double> m_phaseSamples;
    /// The player the samples in m_phaseSamples were measured against, or 0;
    /// when the sync source moves to another, they are dropped.
    int m_sampledDevice = 0;
    /// What a lit SYNC follows, chosen each poll by chooseSyncSource().
    SyncSource m_syncSource;
    /// The player SYNC was following on the last poll, or 0; so that one
    /// stopping can be told from a SYNC that never followed it.
    int m_followingDevice = 0;
    /// Ease the phase: play *trim* (a fraction, +0.005 = 0.5% fast) off the
    /// deck's own speed until told otherwise. 0 stops. Written only on a
    /// change.
    void setPhaseTrim(double trim);
    /// Whether the DJ is moving the deck by hand, so the phase hold must not.
    ///
    /// The jog touched (scratching) or bent, a loop or slip active, reverse,
    /// a cue or hot cue preview, or the deck not playing at all. When this
    /// clears the deck is landed on the beat again, after kSettleMs.
    bool holdSuspended() const;
    /// What holdSuspended() said on the last poll, to see it clear.
    bool m_holdWasSuspended = true;
    /// Since the hold last stopped being suspended; see kSettleMs.
    QElapsedTimer m_holdSettle;
    /// Since the last jog bend; see kBendQuietMs.
    QElapsedTimer m_lastBend;
    /// Rate-limits reportPhaseDrift() to once a second.
    QElapsedTimer m_driftReport;
    /// Since the last phase correction; see kPhaseHoldMs.
    QElapsedTimer m_phaseHold;
    /// Set once publishPlayback() has warned that the deck has a tempo but no
    /// `file_bpm`, so it says so once rather than thirty times a second.
    bool m_warnedNoFileBpm = false;
    /// Since this deck's claim on tempo master began, so a handover is not
    /// mistaken for a rival claim; see kMasterSettleMs. Invalid while we are
    /// not claiming it.
    QElapsedTimer m_masterSince;

    /// The session, while the service has one open.
    ::prolink::Session* m_pSession = nullptr;

    /// The `[ProLink]` controls, which this class **uses and does not own**.
    ///
    /// They belong to ProLinkControls, created by CoreServices long before any
    /// skin exists — see that class for why owning them here does not work.
    /// Null only in a test or a build with no core services, and every use
    /// below is guarded accordingly.
    ProLinkControls* m_pControls = nullptr;
    /// The deck's own controls, read every poll to publish what we are doing.
    std::unique_ptr<ControlProxy> m_pDeckBpm;
    std::unique_ptr<ControlProxy> m_pDeckFileBpm;
    std::unique_ptr<ControlProxy> m_pDeckPlay;
    std::unique_ptr<ControlProxy> m_pDeckDuration;
    /// `beatjump`, Mixxx's exact move by a number of beats.
    std::unique_ptr<ControlProxy> m_pDeckBeatJump;
    /// `phase_trim`, the engine's small temporary change of speed that SYNC
    /// eases the phase back with; see setPhaseTrim().
    std::unique_ptr<ControlProxy> m_pDeckPhaseTrim;
    /// What setPhaseTrim() last wrote.
    double m_phaseTrim = 0.0;
    /// This deck's place on its grid as heard; what SYNC compares, the meter
    /// draws and the network is told.
    std::unique_ptr<AudibleBeatClock> m_pOurBeat;
    std::unique_ptr<ControlProxy> m_pDeckPlayLatched;
    std::unique_ptr<ControlProxy> m_pDeckScratching;
    std::unique_ptr<ControlProxy> m_pDeckLoopEnabled;
    std::unique_ptr<ControlProxy> m_pDeckSlipEnabled;
    std::unique_ptr<ControlProxy> m_pDeckReverse;
    std::unique_ptr<ControlProxy> m_pDeckJog;
};

} // namespace prolink
} // namespace mixxx
