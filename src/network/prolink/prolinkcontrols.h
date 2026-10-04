#pragma once

#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"

namespace mixxx {
namespace prolink {

/// The `[ProLink]` controls, created **before the skin is parsed**.
///
/// This class exists for one reason, and it is a sharp edge in Mixxx rather
/// than a preference. `ControlDoublePrivate::getControl()` returns **null** to
/// a second creator of the same key — the first owner wins and the loser gets
/// an object wired to nothing, whose `get()` is always 0 and whose
/// `valueChanged` never fires. Meanwhile `LegacySkinParser` *creates* a control
/// for any `<Connection>` whose key does not exist yet, and logs it as
/// "Creating skin control object".
///
/// Put those together and the ownership of a control is decided by whichever
/// runs first. The Pro DJ Link controls used to be created by
/// ProLinkNetworkService, which is created by the MediaRegistry, which is
/// created by WDeckBrowser — a widget in the same skin, built *after* the
/// header. So the skin won every race, the service's own buttons were dead, and
/// the failure was silent in both directions: the SYNC button latched on screen
/// and did nothing, and MASTER lit while held and released to nothing, because
/// the objects the service was listening to and writing were not connected to
/// anything at all.
///
/// So they are created here, from CoreServices, before any skin exists.
/// Everything else reaches them through this one object.
class ProLinkControls {
  public:
    ProLinkControls();
    ~ProLinkControls();

    /// The one that exists, or null before CoreServices has built it.
    static ProLinkControls* instance();

    /// Fetch this player's database again, on request from a mapping.
    ControlPushButton* pullDatabase() const {
        return m_pPullDb.get();
    }

    /// The player the phase meter draws, `0` for none: the master when there
    /// is one, else a deck worth drawing. Not necessarily what SYNC follows;
    /// see ProLinkNetworkService::publishMaster() and chooseSyncSource().
    ControlObject* masterDevice() const {
        return m_pMasterDevice.get();
    }
    /// Where the deck the phase meter follows is in its bar, `0..1`, or `-1`
    /// for none. The master first, held where its status says it stands while
    /// it is paused; see ProLinkNetworkService::publishMaster().
    ///
    /// `-1` rather than `0`, because a master sitting exactly on its downbeat
    /// is a real and common state and must not read as an absent one.
    ControlObject* masterBarPhase() const {
        return m_pMasterBarPhase.get();
    }

    /// Whether the deck the meter draws (masterDevice()) is the tempo master,
    /// rather than a deck drawn because nobody is. Read-only.
    ControlObject* meterIsMaster() const {
        return m_pMeterIsMaster.get();
    }
    /// Whether the meter's deck is playing, its phase drawn from its beats;
    /// 0 for a deck held where its status says it stands. Read-only.
    ControlObject* meterLive() const {
        return m_pMeterLive.get();
    }

    /// Pressed to take tempo master. A request, not a decision.
    ControlPushButton* takeMaster() const {
        return m_pTakeMaster.get();
    }
    /// Whether we hold it. Read-only: this is what the network settled.
    ControlObject* isMaster() const {
        return m_pIsMaster.get();
    }
    /// Whether SYNC is following a deck right now: lit, not master, and with
    /// a tempo to follow. While it is, the tempo fader is connected to
    /// nothing. Read-only.
    ControlObject* following() const {
        return m_pFollowing.get();
    }
    /// SYNC: follow the network master's tempo, and say so on the wire.
    ControlPushButton* syncEnabled() const {
        return m_pSyncEnabled.get();
    }

    /// KEY SYNC: hold this deck at the key the network master was in.
    ///
    /// Writable, because it is a button, and a *latch* rather than a follower:
    /// what it holds is decided once, when it is pressed. See ProLinkKeySync.
    ControlPushButton* keySyncEnabled() const {
        return m_pKeySyncEnabled.get();
    }
    /// Whether pressing KEY SYNC would do anything: another player holds tempo
    /// master and we know what key its track is in. Read-only.
    ///
    /// **Not the same as "the button is off".** An engaged sync stays engaged
    /// when this goes back to 0 — it is holding a key it already has, and a
    /// key does not stop being a key because the deck that named it stopped
    /// being master.
    ControlObject* keySyncAvailable() const {
        return m_pKeySyncAvailable.get();
    }
    /// The key KEY SYNC is holding, as a ChromaticKey, or 0 while released.
    /// Read-only.
    ///
    /// Kept here rather than only in ProLinkKeySync because that lives with
    /// the skin and these controls do not: a skin reload rebuilt it released
    /// while `key_sync_enabled` stayed lit and the deck stayed shifted, and
    /// the press that should have let go found nothing to release.
    ControlObject* keySyncTarget() const {
        return m_pKeySyncTarget.get();
    }

  private:
    std::unique_ptr<ControlPushButton> m_pPullDb;
    std::unique_ptr<ControlObject> m_pMasterDevice;
    std::unique_ptr<ControlObject> m_pMasterBarPhase;
    std::unique_ptr<ControlObject> m_pMeterIsMaster;
    std::unique_ptr<ControlObject> m_pMeterLive;
    std::unique_ptr<ControlPushButton> m_pTakeMaster;
    std::unique_ptr<ControlObject> m_pIsMaster;
    std::unique_ptr<ControlPushButton> m_pSyncEnabled;
    std::unique_ptr<ControlObject> m_pFollowing;
    std::unique_ptr<ControlPushButton> m_pKeySyncEnabled;
    std::unique_ptr<ControlObject> m_pKeySyncAvailable;
    std::unique_ptr<ControlObject> m_pKeySyncTarget;
    /// `[ProLink] phase_trim_ms`: how much later this deck is heard than
    /// Mixxx's own latency accounts for, against a CDJ's beat packets. From
    /// mixxx.cfg, measured once per rig; read by AudibleBeatClock.
    std::unique_ptr<ControlObject> m_pPhaseTrimMs;
};

} // namespace prolink
} // namespace mixxx
