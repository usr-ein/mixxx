#include "network/prolink/prolinknetworkservice.h"

#include <QDateTime>
#include <limits>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTimer>
#include <exception>

#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "moc_prolinknetworkservice.cpp"
#include "network/prolink/prolinkbeatposition.h"
#include "network/prolink/prolinkcontrols.h"
#include "network/prolink/synctempo.h"
#include "prolink-cxx/src/lib.rs.h"
#include "util/assert.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkNetworkService");

/// How often the library's events are drained and its tables re-read.
///
/// 33 ms, which is what the phase publisher this replaced used: the bar-phase
/// marker is animated from `[ProLink] master_bar_phase`, so this is the rate
/// that marker moves at. A beat at 145 BPM is 414 ms apart, so events are far
/// less demanding than the marker is.
constexpr int kPollIntervalMs = 33;

/// How close two tempos have to be before the one-shot landing runs.
constexpr double kTempoMatchedBpm = 0.05;

/// How far out of phase the deck may drift before it is nudged back.
///
/// A fiftieth of a beat, which at 120 BPM is 10 ms -- below what anyone can
/// hear as a flam, and comfortably above the jitter in measuring the master's
/// phase from packets that arrive every 200 ms.
constexpr double kPhaseSlipBeats = 0.02;

/// How long a fresh claim on tempo master is left unchallenged.
///
/// A deck handing over keeps claiming mastership until its successor has picked
/// it up, and its status drops within ~70 ms of that (S28). This is twenty
/// times that: long enough that a handover is never mistaken for a rival, short
/// enough that a DJ pressing MASTER on the CDJ and watching its light does not
/// notice the gap.
constexpr int kMasterSettleMs = 1500;

/// The shortest gap between two phase corrections.
///
/// A correction has to have taken effect *and* been measured before the next
/// one is considered, or one error is chased by a burst of overlapping seeks.
constexpr int kPhaseHoldMs = 1500;

/// Is this deck one to line the phase meter up against?
///
/// **Two questions, and the second one used to be skipped for the master.**
///
///  1. *Is there a phase to draw?* `bar_phase` is negative for a player that
///     has sent no beat or whose last beat has gone stale, so this one
///     question covers both.
///  2. *Is it playing?* Asked of the status packet, which a deck sends every
///     ~200 ms whatever it is doing — including while paused, which is the
///     whole point. Beats are the other way round: they simply stop, and
///     "stopped" is indistinguishable from "the packet was dropped" until
///     three seconds have passed.
///
/// The master was exempt from both, and that is the bug this exists to close:
/// a CDJ that holds tempo master and is then paused goes on saying it is
/// master, so the meter went on drawing a deck standing still — and a beat
/// phase extrapolated from a beat that never came kept the ticks walking.
///
/// A deck we have no status for at all is judged on its beats alone. Fresh
/// beats *are* playing — they stop when the platter does — so this is the same
/// answer arrived at by the only route left.
bool isWorthFollowing(const ::prolink::Player& player) {
    if (player.bar_phase < 0.0) {
        return false;
    }
    if (!player.has_status) {
        return true;
    }
    switch (player.play_state) {
    case ::prolink::PlayState::Playing:
    case ::prolink::PlayState::Looping:
    case ::prolink::PlayState::CuePlay:
        return true;
    case ::prolink::PlayState::Emergency:
        // The medium was pulled and the deck is looping what it had. Still
        // making sound, still on the grid, and still a deck a DJ is mixing
        // against -- the emergency is the medium's, not the music's.
        return true;
    default:
        // Paused, cued, searching, spun down, loading, nothing loaded. All of
        // them are a deck that is not keeping time for anybody.
        return false;
    }
}

mixxx::prolink::MediaSlot toMixxxSlot(::prolink::Slot slot) {
    switch (slot) {
    case ::prolink::Slot::Cd:
        return mixxx::prolink::MediaSlot::Cd;
    case ::prolink::Slot::Sd:
        return mixxx::prolink::MediaSlot::Sd;
    case ::prolink::Slot::Usb:
        return mixxx::prolink::MediaSlot::Usb;
    case ::prolink::Slot::Rekordbox:
        return mixxx::prolink::MediaSlot::Rekordbox;
    default:
        return mixxx::prolink::MediaSlot::Empty;
    }
}

::prolink::Slot toRustSlot(mixxx::prolink::MediaSlot slot) {
    switch (slot) {
    case mixxx::prolink::MediaSlot::Cd:
        return ::prolink::Slot::Cd;
    case mixxx::prolink::MediaSlot::Sd:
        return ::prolink::Slot::Sd;
    case mixxx::prolink::MediaSlot::Rekordbox:
        return ::prolink::Slot::Rekordbox;
    case mixxx::prolink::MediaSlot::Usb:
    default:
        // USB for anything unnamed: it is the slot a deck browses first, and
        // the one a caller means when it has not thought about it.
        return ::prolink::Slot::Usb;
    }
}

mixxx::prolink::DeviceKind toMixxxKind(::prolink::DeviceKind kind) {
    switch (kind) {
    case ::prolink::DeviceKind::Mixer:
        return mixxx::prolink::DeviceKind::Mixer;
    case ::prolink::DeviceKind::Rekordbox:
        return mixxx::prolink::DeviceKind::RekordboxOrCdj3000;
    case ::prolink::DeviceKind::Cdj:
    default:
        return mixxx::prolink::DeviceKind::Cdj;
    }
}

QString toQString(const ::rust::String& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// Whether two serve statuses would draw the same page.
///
/// Field by field rather than a memcmp or an operator==: the struct is the
/// library feature's vocabulary, and adding a comparison to it would put a
/// definition of "changed" somewhere nothing else looks.
bool sameServeStatus(const mixxx::prolink::server::ServeStatus& left,
        const mixxx::prolink::server::ServeStatus& right) {
    if (left.active != right.active || left.deviceNumber != right.deviceNumber ||
            left.address != right.address || left.interfaceName != right.interfaceName ||
            left.portmapPort != right.portmapPort || left.mountdPort != right.mountdPort ||
            left.nfsdPort != right.nfsdPort || left.dbserverPort != right.dbserverPort ||
            left.media.size() != right.media.size() ||
            left.consumers.size() != right.consumers.size()) {
        return false;
    }
    for (int i = 0; i < left.media.size(); ++i) {
        const mixxx::prolink::server::ServedSlot& a = left.media.at(i);
        const mixxx::prolink::server::ServedSlot& b = right.media.at(i);
        if (a.slot != b.slot || a.volumeName != b.volumeName ||
                a.localPath != b.localPath || a.trackCount != b.trackCount ||
                a.phantom != b.phantom) {
            return false;
        }
    }
    for (int i = 0; i < left.consumers.size(); ++i) {
        const mixxx::prolink::server::ServeConsumer& a = left.consumers.at(i);
        const mixxx::prolink::server::ServeConsumer& b = right.consumers.at(i);
        if (a.deviceNumber != b.deviceNumber || a.slot != b.slot ||
                a.trackId != b.trackId || a.playing != b.playing) {
            return false;
        }
    }
    return true;
}

mixxx::prolink::ProLinkDevice toMixxxDevice(const ::prolink::Device& device) {
    mixxx::prolink::ProLinkDevice out;
    out.mac = toQString(device.mac).toLatin1();
    out.address = QHostAddress(toQString(device.address));
    out.name = toQString(device.name);
    out.nameRaw = out.name.toUtf8();
    out.deviceNumber = device.number;
    out.kind = toMixxxKind(device.kind);
    out.online = device.online;
    return out;
}
} // namespace

namespace mixxx {
namespace prolink {

/// Everything that would otherwise drag the generated bridge header into every
/// translation unit that includes ours.
struct ProLinkNetworkService::Impl {
    /// Null until `start()`, and again after `shutdown()`.
    ///
    /// A `rust::Box` has no empty state — it is a non-null owning pointer by
    /// construction — so the optionality lives here rather than in the Box.
    std::unique_ptr<::rust::Box<::prolink::Session>> pSession;

    void stop() {
        // Dropping the Box drops the session, which releases the device number
        // and closes the sockets.
        pSession.reset();
    }
};

ProLinkNetworkService::ProLinkNetworkService(QObject* parent)
        : QObject(parent),
          m_pImpl(std::make_unique<Impl>()),
          m_pControls(ProLinkControls::instance()) {
    VERIFY_OR_DEBUG_ASSERT(m_pControls) {
        // Nothing to hang the buttons off, and nowhere to publish the master.
        // The network half still runs; the UI half simply does not exist.
        kLogger.warning() << "the [ProLink] controls were never created;"
                          << "the phase meter and the SYNC and MASTER buttons will do nothing";
        return;
    }
    connect(m_pControls->pullDatabase(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                if (value > 0) {
                    pullDatabase();
                }
            });

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
                if (!m_pImpl->pSession) {
                    return;
                }
                if (!(*m_pImpl->pSession)->is_tempo_master()) {
                    (*m_pImpl->pSession)->take_tempo_master();
                }
            });

    connect(m_pControls->syncEnabled(),
            &ControlPushButton::valueChanged,
            this,
            [this](double value) {
                // Published either way, so the rest of the network can see
                // that this deck is following rather than flying its own
                // tempo -- that is what a CDJ lights its SYNC button from.
                if (m_pImpl->pSession) {
                    (*m_pImpl->pSession)->set_synced(value > 0);
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
        return std::make_unique<ControlProxy>(QString::fromLatin1(kDeckGroup),
                QString::fromLatin1(item),
                this,
                ControlFlag::NoWarnIfMissing);
    };
    m_pDeckBpm = deck("bpm");
    m_pDeckFileBpm = deck("file_bpm");
    m_pDeckPlay = deck("play");
    m_pDeckDuration = deck("duration");
    m_pDeckPlayPosition = deck("playposition");
    m_pDeckBeatDistance = deck("beat_distance");

    // **Dropping in on the beat.** Pressing SYNC aligns the phase once, but a
    // track loaded afterwards starts wherever its cue happens to sit -- tempo
    // matched, beat not, which is the one combination that sounds worse than
    // no sync at all. A CDJ aligns when the deck starts playing, so this does
    // too: the rising edge of `play` asks for an alignment, and followMaster()
    // performs it as soon as the tempos agree.
    m_pDeckPlay->connectValueChanged(this, [this](double value) {
        if (value > 0.0 && m_pControls && m_pControls->syncEnabled()->get() > 0.0) {
            m_alignWhenTempoMatches = true;
        }
    });

    // Poll from the start, network or not: the effect rack needs a beat length
    // whether or not there is a CDJ to take it from.
    startPolling();
}

/*static*/ const char* ProLinkNetworkService::kDeckGroup = "[Channel1]";

void ProLinkNetworkService::setLoadedTrack(int sourcePlayer,
        MediaSlot slot,
        quint32 rekordboxId) {
    if (!m_pImpl->pSession) {
        return;
    }
    (*m_pImpl->pSession)
            ->set_loaded_track(static_cast<quint8>(qBound(0, sourcePlayer, 255)),
                    toRustSlot(slot),
                    rekordboxId);
}

void ProLinkNetworkService::publishPlayback() {
    if (!m_pControls) {
        return;
    }

    const double fileBpm = m_pDeckFileBpm->get();
    const double duration = m_pDeckDuration->get();
    if (fileBpm <= 0.0 || duration <= 0.0) {
        // No track, or one with no grid. Saying nothing is right: a tempo we
        // cannot state is not a tempo of zero, and a stale one would leave
        // followers locked to a ghost.
        (*m_pImpl->pSession)->clear_playback();
        return;
    }
    // The fader as a percentage, derived from the two tempos rather than read
    // off `rate`: rate has to be combined with the range and the direction, and
    // getting any of the three wrong is a pitch that is silently inverted.
    const double effectiveBpm = m_pDeckBpm->get();
    const double pitchPercent = effectiveBpm > 0.0 ? (effectiveBpm / fileBpm - 1.0) * 100.0 : 0.0;

    const mixxx::prolink::BeatPosition position =
            mixxx::prolink::beatPositionOf(m_pDeckPlayPosition->get(),
                    duration,
                    fileBpm,
                    m_pDeckBeatDistance->get());
    (*m_pImpl->pSession)
            ->set_playback(fileBpm,
                    pitchPercent,
                    m_pDeckPlay->get() > 0.0,
                    position.number,
                    position.fraction);
}

void ProLinkNetworkService::followMaster() {
    if (!m_pControls) {
        return;
    }

    // Which of the eight states this is, and therefore whether the tempo comes
    // off the wire or off the fader. The table and the reasoning are in
    // docs/tempo-sync.md; the decision itself is in SyncTempo so that every row
    // of that table is a test rather than a branch nobody can find.
    //
    // Nothing to do in the Fader case: not writing the tempo *is* letting the
    // fader have it. Catching up afterwards is `soft-takeover` in the mapping,
    // which is Mixxx's own.
    SyncTempo::State state;
    state.syncEnabled = m_pControls->syncEnabled()->get() > 0.0;
    state.isMaster = m_pControls->isMaster()->get() > 0.0;
    state.masterBpm = m_pControls->masterBpm()->get();
    if (SyncTempo::decide(state) != SyncTempo::Source::Master) {
        return;
    }
    const double masterBpm = state.masterBpm;
    const double ours = m_pDeckBpm->get();
    if (ours <= 0.0) {
        return;
    }
    // **The tempo is the master's, exactly, and is not used to steer.**
    //
    // An earlier version held the phase by trimming the tempo a fraction of a
    // percent. It worked -- the error closed smoothly -- but it rewrote the
    // tempo on every one of thirty polls a second, and the deck's own BPM
    // readout jittered around the master's value for as long as SYNC was lit.
    // A tempo display that will not sit still is worse than the drift it was
    // correcting, because it is the number a DJ reads to decide whether the two
    // decks agree at all.
    //
    // So the tempo is set once and left alone, and the phase is corrected by
    // moving the playhead: a few tens of milliseconds, which is what a nudge on
    // a jog wheel is, and inaudible on anything but a solo drum hit.
    //
    // A deadband rather than equality, because the master's effective tempo is
    // its centi-BPM times a fixed-point pitch and this deck's is whatever the
    // rate slider quantises to: the two converge to about a hundredth of a BPM
    // and then stop.
    const bool tempoMatched = std::abs(masterBpm - ours) < kTempoMatchedBpm;
    if (!tempoMatched) {
        ControlObject::set(ConfigKey(QString::fromLatin1(kDeckGroup),
                                   QStringLiteral("bpm")),
                masterBpm);
    }

    // **Only while playing.** A paused deck's playhead does not move, so the
    // master walks away from it and the error grows without bound; there is
    // nothing to correct until the deck is running.
    if (tempoMatched && m_pDeckPlay->get() > 0.0) {
        // **A sync that aligns once is not a sync.** Landing on the beat when
        // the button is pressed is the easy half; the two then drift apart
        // whenever the master's tempo is nudged, and nothing was closing that
        // gap again. A CDJ holds the phase for as long as SYNC is lit.
        //
        // Rate-limited, so a correction always has time to take effect and be
        // measured before the next one is considered -- otherwise a single
        // error is chased by a burst of overlapping seeks.
        const bool pending = m_alignWhenTempoMatches;
        const bool due = !m_phaseHold.isValid() || m_phaseHold.elapsed() > kPhaseHoldMs;
        double error = 0.0;
        if ((pending || due) && phaseErrorBeats(&error) &&
                (pending || std::abs(error) > kPhaseSlipBeats)) {
            m_alignWhenTempoMatches = false;
            m_phaseHold.start();
            alignPhaseToMaster();
        }
    }
    reportPhaseDrift();
}

bool ProLinkNetworkService::phaseErrorBeats(double* pBeats) const {
    const double masterPhase = m_pControls->masterBarPhase()->get();
    const double duration = m_pDeckDuration->get();
    const double fileBpm = m_pDeckFileBpm->get();
    if (masterPhase < 0.0 || duration <= 0.0 || fileBpm <= 0.0) {
        return false;
    }
    const double ourPhase = mixxx::prolink::barPhaseOf(
            mixxx::prolink::beatPositionOf(m_pDeckPlayPosition->get(),
                    duration,
                    fileBpm,
                    m_pDeckBeatDistance->get()));
    if (ourPhase < 0.0) {
        return false;
    }
    // Wrapped to the nearest **beat**, not the nearest bar. Bar alignment
    // across devices is not knowable -- nothing in a Mixxx grid names a
    // downbeat -- so chasing it would drag the track by up to two beats.
    double beats = (masterPhase - ourPhase) * mixxx::prolink::kBeatsPerBar;
    beats -= std::floor(beats);
    if (beats > 0.5) {
        beats -= 1.0;
    }
    *pBeats = beats;
    return true;
}

void ProLinkNetworkService::reportPhaseDrift() {
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
                    << beats << "beats ) -- master" << m_pControls->masterBarPhase()->get()
                    << "ours"
                    << mixxx::prolink::barPhaseOf(
                               mixxx::prolink::beatPositionOf(m_pDeckPlayPosition->get(),
                                       m_pDeckDuration->get(),
                                       m_pDeckFileBpm->get(),
                                       m_pDeckBeatDistance->get()))
                    << "beat distance" << m_pDeckBeatDistance->get();
}

void ProLinkNetworkService::alignPhaseToMaster() {
    if (!m_pControls) {
        return;
    }

    const double masterPhase = m_pControls->masterBarPhase()->get();
    const double duration = m_pDeckDuration->get();
    const double fileBpm = m_pDeckFileBpm->get();
    const double effectiveBpm = m_pDeckBpm->get();
    if (masterPhase < 0.0 || duration <= 0.0 || fileBpm <= 0.0 || effectiveBpm <= 0.0) {
        // Said out loud: a silent decline here is a SYNC button that matches
        // the tempo, leaves the deck half a beat out, and reports nothing.
        kLogger.debug() << "no phase to align to -- master phase" << masterPhase
                        << "duration" << duration << "file bpm" << fileBpm
                        << "effective bpm" << effectiveBpm;
        return;
    }
    const double ourPhase = mixxx::prolink::barPhaseOf(
            mixxx::prolink::beatPositionOf(m_pDeckPlayPosition->get(),
                    duration,
                    fileBpm,
                    m_pDeckBeatDistance->get()));
    if (ourPhase < 0.0) {
        return;
    }
    // **Beats, not bars.** Bar alignment across devices is not knowable --
    // nothing in a Mixxx grid names a downbeat -- so aligning to the master's
    // bar would be a coin flip that moves the track by up to two beats. The
    // beat within the bar is real, so the correction is wrapped into half a
    // beat either way and never moves the playhead further than that.
    const double beatsApart = (masterPhase - ourPhase) * mixxx::prolink::kBeatsPerBar;
    double withinBeat = beatsApart - std::floor(beatsApart);
    if (withinBeat > 0.5) {
        withinBeat -= 1.0;
    }
    // **The track's own tempo, not the one being played.** `playposition` is a
    // fraction of the track, so moving it walks the beat grid at the grid's own
    // rate -- which is `file_bpm`, because that is the tempo the grid was laid
    // out at and the tempo beatPositionOf() reads it back with. Converting the
    // correction at the *playing* tempo instead made every correction wrong by
    // the pitch fader: at +6% it fell 6% short, and at the wide range it
    // undershot by half.
    const double seconds = withinBeat * 60.0 / fileBpm;
    const double position = m_pDeckPlayPosition->get() + seconds / duration;
    if (position < 0.0 || position > 1.0) {
        return;
    }
    const double before = m_pDeckPlayPosition->get();
    // **Through the proxy, which is how every other seek in Mixxx is written.**
    // `ControlObject::set(key, value)` looks the control up and writes it with a
    // null sender; a ControlProxy writes it with itself as the sender, which is
    // what WOverview and the waveform do when a click seeks. The two are
    // supposed to be equivalent and the deck says otherwise: the correction was
    // logged every 1.5 s for minutes on end and the playhead never moved by so
    // much as a millisecond, with the phase error sitting at a constant 0.23
    // beats through all of it.
    m_pDeckPlayPosition->set(position);
    kLogger.debug() << "phase align: moving" << seconds * 1000.0
                    << "ms onto the master's beat -- from" << before << "to" << position
                    << "now reads" << m_pDeckPlayPosition->get();
}

bool ProLinkNetworkService::reconcileMastership(int rivalMaster) {
    if (!(*m_pImpl->pSession)->is_tempo_master()) {
        m_masterSince.invalidate();
        return false;
    }
    if (!m_masterSince.isValid()) {
        m_masterSince.start();
    }
    if (rivalMaster == 0) {
        return true;
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
    (*m_pImpl->pSession)->release_tempo_master();
    m_masterSince.invalidate();
    return false;
}

void ProLinkNetworkService::publishMaster() {
    if (!m_pControls) {
        return;
    }

    if (!m_pImpl->pSession) {
        m_pControls->isMaster()->forceSet(0.0);
        m_pControls->masterDevice()->forceSet(0.0);
        m_pControls->masterBpm()->forceSet(0.0);
        m_pControls->masterBarPhase()->forceSet(-1.0);
        publishMasterTrack(MasterTrack());
        return;
    }

    const ::rust::Vec<::prolink::Player> players = (*m_pImpl->pSession)->players();

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
    const int ours = static_cast<int>((*m_pImpl->pSession)->device_number());
    int rivalMaster = 0;
    for (const ::prolink::Player& player : players) {
        if (!player.is_master || static_cast<int>(player.number) == ours) {
            continue;
        }
        if (static_cast<int>(player.yielding_to) == ours) {
            // Naming us its successor: a takeover of ours in flight, not a
            // rival claim (F52).
            continue;
        }
        rivalMaster = static_cast<int>(player.number);
        break;
    }
    const bool weAreMaster = reconcileMastership(rivalMaster);
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
            masterTrack.trackId = player.track_id;
            break;
        }
    }
    publishMasterTrack(masterTrack);

    // **Who to draw is "the deck I am mixing against", not literally "the
    // master".** The two are the same until this deck takes mastership, and
    // then they stop being: the master becomes us, the top row would be our own
    // phase drawn above our own phase, and the meter goes blank or useless at
    // exactly the moment a DJ has just declared they are the one being
    // followed. So the master is preferred, and any other playing deck is the
    // fallback -- which is the deck whose beats are worth lining up with either
    // way.
    const ::prolink::Player* pShow = nullptr;
    for (const ::prolink::Player& player : players) {
        if (!isWorthFollowing(player)) {
            continue;
        }
        if (player.is_master) {
            pShow = &player;
            break;
        }
        if (pShow == nullptr) {
            pShow = &player;
        }
    }
    if (pShow != nullptr) {
        m_pControls->masterDevice()->forceSet(pShow->number);
        // The tempo actually playing, with the pitch fader applied. The
        // library reports a negative for "not known", which the widget must
        // not draw as a tempo.
        m_pControls->masterBpm()->forceSet(pShow->effective_bpm);
        m_pControls->masterBarPhase()->forceSet(pShow->bar_phase);
        return;
    }
    // Nobody holds master. Not the same as a master at phase zero, which is
    // why the phase goes to -1 rather than to 0.
    m_pControls->masterDevice()->forceSet(0.0);
    m_pControls->masterBpm()->forceSet(0.0);
    m_pControls->masterBarPhase()->forceSet(-1.0);
}

void ProLinkNetworkService::publishEffectTempo() {
    if (!m_pControls) {
        return;
    }
    if (!m_pImpl->pSession) {
        // No network at all. This deck can still be playing, so fall through
        // with an empty player list rather than returning: its own tempo is a
        // perfectly good one to run the rack at.
        m_playingSince.remove(1);
        m_playingSince.remove(2);
        m_playingSince.remove(3);
        m_playingSince.remove(4);
    }
    const ::rust::Vec<::prolink::Player> players = m_pImpl->pSession
            ? (*m_pImpl->pSession)->players()
            : ::rust::Vec<::prolink::Player>();

    // Source codes are the ones [EffectTempo] source documents: 1-4 a Pro DJ
    // Link player, 5 this deck.
    constexpr int kOwnDeck = 5;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QHash<int, double> playing;
    for (const ::prolink::Player& player : players) {
        // The same test the phase meter follows, so the rack and the meter
        // never disagree about who is playing. Cued, paused, searching and
        // spun-down decks are excluded by it -- which is the whole point here,
        // since a deck sitting on its cue point is the one whose tempo must
        // not be taken.
        if (!isWorthFollowing(player) || player.effective_bpm <= 0.0) {
            continue;
        }
        const int number = static_cast<int>(player.number);
        if (number >= 1 && number <= 4) {
            playing.insert(number, player.effective_bpm);
        }
    }
    // This deck counts as a candidate on the same terms as any other.
    if (m_pDeckPlay && m_pDeckPlay->toBool() && m_pDeckBpm && m_pDeckBpm->get() > 0.0) {
        playing.insert(kOwnDeck, m_pDeckBpm->get());
    }

    // Forget anything that stopped. Done before the pick rather than after, so
    // a deck that pauses and restarts is correctly the youngest again rather
    // than keeping the seniority it had before it stopped.
    for (auto it = m_playingSince.begin(); it != m_playingSince.end();) {
        if (playing.contains(it.key())) {
            ++it;
        } else {
            it = m_playingSince.erase(it);
        }
    }
    for (auto it = playing.constBegin(); it != playing.constEnd(); ++it) {
        if (!m_playingSince.contains(it.key())) {
            m_playingSince.insert(it.key(), now);
        }
    }

    // Oldest wins, and keeps winning until it stops.
    int chosen = 0;
    qint64 oldest = std::numeric_limits<qint64>::max();
    for (auto it = m_playingSince.constBegin(); it != m_playingSince.constEnd(); ++it) {
        if (it.value() < oldest || (it.value() == oldest && it.key() < chosen)) {
            oldest = it.value();
            chosen = it.key();
        }
    }

    const double bpm = chosen == 0 ? 0.0 : playing.value(chosen);
    m_pControls->fxBpmSource()->forceSet(chosen);
    m_pControls->fxBpm()->forceSet(bpm);

    // Announced on a change, not on a poll. Which deck the effects are
    // quantised to is invisible from the audio -- a delay following the wrong
    // deck sounds like a broken delay, not like a delay following the wrong
    // deck -- so the one place it can be checked after the fact is here.
    // Half a BPM, because a pitch fader being nudged is not an event.
    if (chosen != m_loggedTempoSource || std::abs(bpm - m_loggedTempoBpm) > 0.5) {
        m_loggedTempoSource = chosen;
        m_loggedTempoBpm = bpm;
        if (chosen == 0) {
            kLogger.info() << "effect tempo: no deck playing";
        } else {
            kLogger.info() << "effect tempo:"
                           << (chosen == 5 ? QStringLiteral("this deck")
                                           : QStringLiteral("player %1").arg(chosen))
                           << "at" << bpm << "BPM"
                           << "(oldest of" << m_playingSince.size() << "playing)";
        }
    }
}

void ProLinkNetworkService::publishMasterTrack(const MasterTrack& track) {
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

/*static*/ ProLinkNetworkService* ProLinkNetworkService::s_pListening = nullptr;

ProLinkNetworkService::~ProLinkNetworkService() {
    // Signals blocked for the whole teardown. shutdown() reports every device
    // as lost and the serve status as gone, which is right when the network is
    // being stopped and wrong when the object is being destroyed: whoever is
    // listening is either already dead or, worse, dying -- a listener that is
    // itself mid-destruction will happily run the handler against members it
    // has already destroyed.
    //
    // This is the general form of the fix; MediaRegistry also disconnects
    // itself explicitly, because relying on one object to remember what
    // another one's destructor emits is how this went unnoticed for a day.
    blockSignals(true);
    shutdown();
}

void ProLinkNetworkService::start() {
    if (m_pImpl->pSession) {
        return;
    }
    // One session per process, and this is not a style rule.
    //
    // Two of these on one machine both bind UDP 50000-50002, both run a
    // portmapper on 111, and both enter the claim chain -- so they compete with
    // each other for a player number, and the one that loses becomes a passive
    // observer that cannot serve or be browsed. That is exactly what happened
    // here: the browser's registry created one and the old sidebar feature
    // created another, and the deck spent a session announcing "no player
    // number was free" against itself.
    //
    // Refused rather than allowed, because the failure is otherwise invisible:
    // everything logs success and the network simply does not work.
    if (s_pListening != nullptr && s_pListening != this) {
        m_lastError = tr("another Pro DJ Link session is already running");
        kLogger.warning() << "refusing to start a second session;"
                          << "the first one holds the sockets and the player number";
        emit listeningChanged(false, m_lastError);
        return;
    }
    // The library's own log, to stderr, which on the deck is where Mixxx's
    // own already goes. Without it the only evidence of what the protocol is
    // doing is the socket table, read over ssh.
    ::prolink::init_logging(::rust::Str("prolink=info"));

    try {
        ::prolink::Config config = ::prolink::default_config();
        // Announcing is what makes players unicast their status to us, and
        // status is the only place the loaded track, the play state and the
        // tempo master are published. Without it we would see beats and
        // nothing else.
        config.announce = true;
        // The number we held before a restart, so a refresh keeps the identity
        // the decks already know. Zero on a cold start, which means "negotiate
        // for whichever of 1-4 is free" -- and 1-4 is a requirement, not a
        // preference: at any other number a deck accepts our announcement in
        // full and then never offers us as a LINK source or asks us anything.
        config.preferred_number = static_cast<::std::uint8_t>(
                m_preferredNumber >= 1 && m_preferredNumber <= 4 ? m_preferredNumber : 0);
        m_pImpl->pSession = std::make_unique<::rust::Box<::prolink::Session>>(
                ::prolink::open(config));
    } catch (const std::exception& error) {
        m_lastError = QString::fromUtf8(error.what());
        m_listening = false;
        kLogger.warning() << "could not start:" << m_lastError;
        emit listeningChanged(false, m_lastError);
        return;
    }

    s_pListening = this;
    m_listening = true;
    m_lastError.clear();
    // Zero for now, and that is not a failure. Claiming a player number means
    // watching the network and then negotiating for it, about five seconds in
    // all, and open() deliberately returns before that so the GUI is not frozen
    // for the duration. poll() announces the number when it arrives.
    m_announcedNumber = 0;
    m_announceDetail = tr("joining the network...");
    kLogger.info() << "started;" << m_announceDetail;

    emit listeningChanged(true, QString());
    emit announceChanged(m_announcedNumber, m_announceDetail);

    startPolling();
}

void ProLinkNetworkService::shutdown() {
    if (m_pTimer != nullptr) {
        m_pTimer->stop();
    }
    const bool wasListening = m_listening;
    m_pImpl->stop();
    if (s_pListening == this) {
        s_pListening = nullptr;
    }
    m_listening = false;
    m_announcedNumber = 0;
    m_announceDetail.clear();

    // Report what is gone, so nothing above keeps drawing a device that is no
    // longer there.
    const QList<ProLinkDevice> had = m_devices;
    m_devices.clear();
    m_pending.clear();
    m_publishedNumber = 0;
    m_serveStatus = server::ServeStatus();
    emit serveStatusChanged(m_serveStatus);
    for (const ProLinkDevice& device : had) {
        emit deviceLost(device.mac);
    }

    if (wasListening) {
        emit listeningChanged(false, QString());
        emit announceChanged(0, QString());
    }
}

void ProLinkNetworkService::refresh() {
    if (!m_pImpl->pSession) {
        start();
        return;
    }

    // Restart, rather than only dropping the browse connections.
    //
    // The interface is chosen when the session opens, and a Mixxx started
    // before the ethernet was plugged in chose whatever was there — on the
    // deck that was the wireless interface, and the CDJs that appeared
    // afterwards were on a network we were not listening to. Nothing about
    // that resolves itself: no keep-alive can arrive on a socket bound to
    // another interface, however long the user waits.
    //
    // So the only honest thing a refresh can do is bind again. It costs the
    // device number and a second of re-discovery, which is what a user
    // clicking "refresh" is asking for.
    shutdown();
    start();
}

int ProLinkNetworkService::numberFor(const QByteArray& mac) const {
    if (!m_pImpl->pSession) {
        return 0;
    }
    return static_cast<int>(
            (*m_pImpl->pSession)->device_number_of(::rust::Str(mac.constData(), mac.size())));
}

void ProLinkNetworkService::fetchFile(const QByteArray& mac,
        MediaSlot slot,
        const QString& remotePath,
        const QString& localPath,
        bool priority) {
    Q_UNUSED(priority);
    if (!m_pImpl->pSession) {
        emit fileFetched(localPath, tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit fileFetched(localPath, tr("that player is no longer on the network"));
        return;
    }

    const QByteArray remote = remotePath.toUtf8();
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_file(static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           ::rust::Str(remote.constData(), remote.size()),
                                           ::rust::Str(local.constData(), local.size()));
        Pending pending;
        pending.isDatabase = false;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit fileFetched(localPath, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::fetchFileStreaming(const QByteArray& mac,
        MediaSlot slot,
        const QString& remotePath,
        const QString& localPath,
        quint32 headBytes) {
    if (!m_pImpl->pSession) {
        emit fileFetched(localPath, tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit fileFetched(localPath, tr("that player is no longer on the network"));
        return;
    }

    const QByteArray remote = remotePath.toUtf8();
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_file_streaming(
                                           static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           ::rust::Str(remote.constData(), remote.size()),
                                           ::rust::Str(local.constData(), local.size()),
                                           headBytes);
        Pending pending;
        pending.isDatabase = false;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
        kLogger.info() << "streaming" << remotePath << "from player" << number
                       << "with a" << headBytes << "byte head";
    } catch (const std::exception& error) {
        emit fileFetched(localPath, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::fetchDatabase(const QByteArray& mac, MediaSlot slot) {
    if (!m_pImpl->pSession) {
        emit databaseFetched(mac, slot, QByteArray(), tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit databaseFetched(
                mac, slot, QByteArray(), tr("that player is no longer on the network"));
        return;
    }

    // One file per (player, slot), so a second pull overwrites rather than
    // accumulating copies of a database that is often several megabytes.
    const QString localPath = QStringLiteral("%1/prolink-%2-%3.pdb")
                                      .arg(QDir::tempPath(),
                                              QString::fromLatin1(mac.toHex()),
                                              QString::number(static_cast<int>(slot)));
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_database(static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           ::rust::Str(local.constData(), local.size()));
        Pending pending;
        pending.isDatabase = true;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit databaseFetched(mac, slot, QByteArray(), QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::pullDatabase(MediaSlot slot) {
    if (!m_pImpl->pSession) {
        return;
    }
    // The first player that says it has something in that slot. Occupancy is
    // published in status packets and nowhere else, which is why this reads
    // the library's view rather than guessing from the device list.
    for (const ::prolink::MediaInfo& info : (*m_pImpl->pSession)->media()) {
        if (!info.has_media || toMixxxSlot(info.slot) != slot) {
            continue;
        }
        for (const ProLinkDevice& device : m_devices) {
            if (device.deviceNumber == static_cast<int>(info.device)) {
                fetchDatabase(device.mac, slot);
                return;
            }
        }
    }
    kLogger.info() << "no player has media in that slot yet";
}

void ProLinkNetworkService::fetchWaveformPreview(
        const QByteArray& mac, MediaSlot slot, quint32 trackId) {
    if (!m_pImpl->pSession) {
        emit previewFetched(mac, slot, trackId, QByteArray(),
                tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit previewFetched(mac, slot, trackId, QByteArray(),
                tr("that player is no longer on the network"));
        return;
    }

    // Over dbserver, on the connection artwork already uses. 900 bytes, and no
    // file at either end -- the bytes are taken off the session when the
    // transfer finishes.
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_waveform_preview(
                                           static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           trackId);
        Pending pending;
        pending.isPreview = true;
        pending.mac = mac;
        pending.slot = slot;
        pending.trackId = trackId;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit previewFetched(
                mac, slot, trackId, QByteArray(), QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::fetchArtwork(const QByteArray& mac,
        MediaSlot slot,
        quint32 artworkId,
        const QString& localPath) {
    if (!m_pImpl->pSession) {
        emit artworkFetched(localPath, tr("Pro DJ Link is not running"));
        return;
    }
    const int number = numberFor(mac);
    if (number == 0) {
        emit artworkFetched(localPath, tr("that player is no longer on the network"));
        return;
    }

    // Artwork comes over the dbserver connection rather than NFS. Asking NFS
    // for it instead churns the deck's filehandle table until it answers
    // NFSERR_STALE to everything, including the track a DJ is loading.
    //
    // Like a file fetch this returns an id and finishes later: the library
    // feature asks for every cover on a medium in one loop -- some six hundred
    // of them -- and a blocking round trip each would freeze the GUI for the
    // length of all six hundred.
    const QByteArray local = localPath.toUtf8();
    try {
        const quint32 id = (*m_pImpl->pSession)
                                   ->fetch_artwork(static_cast<::std::uint8_t>(number),
                                           toRustSlot(slot),
                                           artworkId,
                                           ::rust::Str(local.constData(), local.size()));
        Pending pending;
        pending.isArtwork = true;
        pending.mac = mac;
        pending.slot = slot;
        pending.localPath = localPath;
        m_pending.insert(id, pending);
    } catch (const std::exception& error) {
        emit artworkFetched(localPath, QString::fromUtf8(error.what()));
    }
}

void ProLinkNetworkService::startPolling() {
    // Independent of whether a session ever opened. poll() publishes the
    // effect tempo before it looks at the session, and this deck's own tempo
    // is worth publishing with no network present at all.
    if (m_pTimer == nullptr) {
        m_pTimer = new QTimer(this);
        connect(m_pTimer, &QTimer::timeout, this, &ProLinkNetworkService::poll);
    }
    if (!m_pTimer->isActive()) {
        m_pTimer->start(kPollIntervalMs);
    }
}

void ProLinkNetworkService::poll() {
    // Before the session check, because it does not need one. This deck can be
    // playing with no Pro DJ Link network at all -- nothing plugged into the
    // ethernet port, or open() lost the race for UDP 50000 -- and its own
    // tempo is a perfectly good one to run the rack at. publishEffectTempo()
    // was written to handle exactly that and could never be reached to do it:
    // the early return below skipped it, and the timer that calls poll() is
    // only started after a successful open(). So with no network the rack got
    // no beat length and every delay division was read as that many SECONDS.
    publishEffectTempo();

    if (!m_pImpl->pSession) {
        return;
    }
    // Startup is asynchronous, so a bind that fails -- another Pro DJ Link
    // program already holding a port is the usual reason -- surfaces here
    // rather than as an exception from open().
    if (m_listening && !(*m_pImpl->pSession)->is_ready()) {
        const QString error = toQString((*m_pImpl->pSession)->last_error());
        if (!error.isEmpty() && error != m_lastError) {
            m_lastError = error;
            m_listening = false;
            kLogger.warning() << "could not start:" << error;
            emit listeningChanged(false, error);
        }
    }

    for (const ::prolink::Event& event : (*m_pImpl->pSession)->drain_events()) {
        if (event.dropped > 0) {
            // The queue overflowed, so the running picture is stale and the
            // table has to be re-read rather than patched. Clearing it makes
            // the diff below re-announce everything.
            kLogger.debug() << "missed" << event.dropped << "events; re-reading";
            m_devices.clear();
        }
        switch (event.kind) {
        case ::prolink::EventKind::MediaInfo:
            syncMedia(static_cast<int>(event.device), toMixxxSlot(event.slot));
            break;
        case ::prolink::EventKind::TransferProgress: {
            const auto found = m_pending.constFind(event.transfer);
            if (found != m_pending.constEnd() && !found->isArtwork && !found->isPreview) {
                emit fileFetchProgress(found->localPath,
                        static_cast<quint64>(event.done),
                        static_cast<quint64>(event.total),
                        static_cast<quint64>(event.offset),
                        static_cast<quint64>(event.len));
            }
            break;
        }
        case ::prolink::EventKind::TransferDone: {
            const auto found = m_pending.constFind(event.transfer);
            if (found == m_pending.constEnd()) {
                // Not ours. A transfer the library started for its own reasons,
                // or one left over from a session that has since been
                // restarted — either way there is nobody waiting on a signal
                // for it, and emitting one with an empty path would abort a
                // fetch that is still running under the same empty key.
                break;
            }
            const Pending pending = m_pending.take(event.transfer);
            const QString error = event.ok ? QString() : toQString(event.detail);
            // A missing cover is not worth reporting as the connection's last
            // error: a medium has hundreds of them, a few are always absent,
            // and this string is what the UI shows about the network itself.
            if (!error.isEmpty() && !pending.isArtwork && !pending.isPreview) {
                m_lastError = error;
            }
            if (pending.isDatabase) {
                // The caller parses the bytes and never wants the file, so
                // the temp copy is read back and dropped here rather than
                // becoming something it has to clean up.
                QByteArray data;
                QString reason = error;
                if (reason.isEmpty()) {
                    QFile file(pending.localPath);
                    if (file.open(QIODevice::ReadOnly)) {
                        data = file.readAll();
                        file.close();
                    } else {
                        reason = tr("could not read the database back: %1")
                                         .arg(file.errorString());
                    }
                    QFile::remove(pending.localPath);
                }
                emit databaseFetched(pending.mac, pending.slot, data, reason);
            } else if (pending.isPreview) {
                // Taken rather than read back: the bytes never touched a
                // filesystem, which is the point of fetching a 900-byte blob
                // over dbserver instead of a 157 kB file over NFS.
                QByteArray blob;
                if (error.isEmpty()) {
                    const auto bytes =
                            (*m_pImpl->pSession)->take_waveform_preview(event.transfer);
                    blob = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<qsizetype>(bytes.size()));
                }
                emit previewFetched(
                        pending.mac, pending.slot, pending.trackId, blob, error);
            } else if (pending.isArtwork) {
                emit artworkFetched(pending.localPath, error);
            } else {
                emit fileFetched(pending.localPath, error);
            }
            break;
        }
        default:
            // Beats, player state and tempo master are read from the tables
            // below rather than acted on here.
            break;
        }
    }

    syncDevices();
    publishMaster();
    publishPlayback();
    followMaster();
    syncAnnouncement();
    syncServeStatus();
}

void ProLinkNetworkService::syncServeStatus() {
    const ::prolink::ServeStatus fresh = (*m_pImpl->pSession)->serve_status();

    server::ServeStatus status;
    status.active = fresh.active;
    status.deviceNumber = static_cast<int>(fresh.device_number);
    status.deviceName = tr("Mixxx (this machine)");
    status.address = QHostAddress(toQString(fresh.address));
    status.interfaceName = toQString(fresh.interface);
    status.portmapPort = fresh.portmap_port;
    status.mountdPort = fresh.mount_port;
    status.nfsdPort = fresh.nfs_port;
    status.dbserverPort = fresh.dbserver_port;
    for (const ::prolink::ServedSlot& slot : fresh.media) {
        server::ServedSlot served;
        served.slot = toMixxxSlot(slot.slot);
        served.exportPath = toQString(slot.export_path);
        served.volumeName = toQString(slot.volume_name);
        served.localPath = toQString(slot.local_path);
        served.trackCount = static_cast<int>(slot.track_count);
        served.playlistCount = static_cast<int>(slot.playlist_count);
        served.phantom = slot.phantom;
        status.media.append(served);
    }
    for (const ::prolink::ServeConsumer& reader : fresh.consumers) {
        server::ServeConsumer consumer;
        consumer.deviceNumber = static_cast<int>(reader.device_number);
        consumer.slot = toMixxxSlot(reader.slot);
        consumer.trackId = reader.track_id;
        consumer.playing = reader.playing;
        // Named from the device table, which the library fills from
        // keep-alives; a consumer we have not seen one from still counts,
        // because its status packet is what put it here.
        for (const ProLinkDevice& device : m_devices) {
            if (device.deviceNumber == consumer.deviceNumber) {
                consumer.deviceName = device.name;
                consumer.address = device.address;
                break;
            }
        }
        status.consumers.append(consumer);
    }

    // Emitted on a change rather than thirty times a second: the page it feeds
    // rebuilds its whole HTML, and a DJ scrolling it would fight the rebuild.
    if (sameServeStatus(status, m_serveStatus)) {
        return;
    }
    m_serveStatus = status;
    emit serveStatusChanged(m_serveStatus);
}

void ProLinkNetworkService::syncAnnouncement() {
    const int number = static_cast<int>((*m_pImpl->pSession)->device_number());
    if (number == m_publishedNumber) {
        return;
    }
    m_publishedNumber = number;
    m_announcedNumber = number;
    if (number >= 1 && number <= 4) {
        m_preferredNumber = number;
    }
    if (number >= 1 && number <= 4) {
        m_announceDetail = tr("announced as player %1").arg(number);
    } else if (number > 0) {
        // Every player number was defended, so the library settled for one
        // outside the range. Everything passive still works; being browsed does
        // not, and the detail has to say so rather than read like success.
        m_announceDetail = tr("watching as device %1; no player number was free").arg(number);
    } else if ((*m_pImpl->pSession)->is_ready()) {
        m_announceDetail = tr("listening without a player number");
    } else {
        m_announceDetail = tr("joining the network...");
    }
    kLogger.info() << "announcement changed:" << m_announceDetail;
    emit announceChanged(m_announcedNumber, m_announceDetail);
}

void ProLinkNetworkService::syncDevices() {
    QList<ProLinkDevice> fresh;
    for (const ::prolink::Device& device : (*m_pImpl->pSession)->devices()) {
        fresh.append(toMixxxDevice(device));
    }

    // Diffed rather than replaced wholesale, because the library feature
    // listens for found/changed/lost and rebuilding its tree on every poll
    // would collapse the user's selection twenty times a second.
    for (const ProLinkDevice& now : fresh) {
        bool seen = false;
        for (const ProLinkDevice& before : m_devices) {
            if (before.mac != now.mac) {
                continue;
            }
            seen = true;
            if (before.deviceNumber != now.deviceNumber || before.name != now.name ||
                    before.online != now.online || before.address != now.address) {
                emit deviceChanged(now);
            }
            break;
        }
        if (!seen) {
            emit deviceFound(now);
        }
    }
    for (const ProLinkDevice& before : m_devices) {
        bool still = false;
        for (const ProLinkDevice& now : fresh) {
            if (before.mac == now.mac) {
                still = true;
                break;
            }
        }
        if (!still) {
            emit deviceLost(before.mac);
        }
    }

    m_devices = fresh;
}

void ProLinkNetworkService::syncMedia(int deviceNumber, MediaSlot slot) {
    for (const ::prolink::MediaInfo& found : (*m_pImpl->pSession)->media()) {
        if (static_cast<int>(found.device) != deviceNumber ||
                toMixxxSlot(found.slot) != slot) {
            continue;
        }
        MediaInfo info;
        info.name = toQString(found.volume_name);
        info.trackCount = found.track_count;
        info.playlistCount = found.playlist_count;
        for (const ProLinkDevice& device : m_devices) {
            if (device.deviceNumber == deviceNumber) {
                emit mediaInfoFound(device.mac, slot, info);
                return;
            }
        }
        return;
    }
}

} // namespace prolink
} // namespace mixxx
