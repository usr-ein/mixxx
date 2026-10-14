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
/// So they are created here, by DeckServices, which CoreServices makes before
/// any skin exists, and handed to what uses them.
class ProLinkControls {
  public:
    ProLinkControls();
    ~ProLinkControls();

    /// Fetch this player's database again, on request from a mapping.
    ControlPushButton* pullDatabase() const {
        return m_pPullDb.get();
    }

    /// The player the phase meter draws, `0` for none: the master when there
    /// is one, else a deck worth drawing. Not necessarily what SYNC follows;
    /// see chooseMeterDeck() and chooseSyncSource().
    ControlObject* masterDevice() const {
        return m_pMasterDevice.get();
    }
    /// Where the deck the phase meter draws is in its bar, `0..1`, or `-1`
    /// for none. Held where its status says it stands while it is paused; see
    /// chooseMeterDeck().
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

    /// KEY SYNC pressed: put the deck on the master's Camelot number, or back
    /// in its own key if it is there already or there is nothing to match.
    /// See KeySync for the rules.
    ControlPushButton* keySync() const {
        return m_pKeySync.get();
    }
    /// The shift on the track on the deck, in semitones, -5..+6; 0 for its
    /// own key. Writable: the key list picks a key by writing its shift
    /// here, and ProLinkKeySync applies whatever lands, folded into range.
    ///
    /// Here rather than in ProLinkKeySync because these controls are made
    /// before anything that writes or watches it, and outlive it.
    ControlObject* keyShift() const {
        return m_pKeyShift.get();
    }
    /// Whether pressing KEY SYNC would change the deck's key. Read-only.
    ///
    /// What lights the pad dim. While the deck is shifted a press always does
    /// something, matching the master or going back, and the pad is lit for
    /// the shift anyway.
    ControlObject* keySyncAvailable() const {
        return m_pKeySyncAvailable.get();
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
    std::unique_ptr<ControlPushButton> m_pKeySync;
    std::unique_ptr<ControlObject> m_pKeyShift;
    std::unique_ptr<ControlObject> m_pKeySyncAvailable;
    /// `[ProLink] phase_trim_ms`: how much later this deck is heard than
    /// Mixxx's own latency accounts for, against a CDJ's beat packets. From
    /// mixxx.cfg, measured once per rig; read by AudibleBeatClock.
    std::unique_ptr<ControlObject> m_pPhaseTrimMs;
};

} // namespace prolink
} // namespace mixxx
