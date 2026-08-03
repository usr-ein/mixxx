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

    /// The tempo master's player number, `0` when nobody holds it.
    ControlObject* masterDevice() const {
        return m_pMasterDevice.get();
    }
    /// The tempo master's effective tempo, `0` when there is none.
    ControlObject* masterBpm() const {
        return m_pMasterBpm.get();
    }
    /// Where the tempo master is in its bar, `0..1`, or `-1` for no master.
    ///
    /// `-1` rather than `0`, because a master sitting exactly on its downbeat
    /// is a real and common state and must not read as an absent one.
    ControlObject* masterBarPhase() const {
        return m_pMasterBarPhase.get();
    }

    /// Pressed to take tempo master. A request, not a decision.
    ControlPushButton* takeMaster() const {
        return m_pTakeMaster.get();
    }
    /// Whether we hold it. Read-only: this is what the network settled.
    ControlObject* isMaster() const {
        return m_pIsMaster.get();
    }
    /// SYNC: follow the network master's tempo, and say so on the wire.
    ControlPushButton* syncEnabled() const {
        return m_pSyncEnabled.get();
    }

  private:
    std::unique_ptr<ControlPushButton> m_pPullDb;
    std::unique_ptr<ControlObject> m_pMasterDevice;
    std::unique_ptr<ControlObject> m_pMasterBpm;
    std::unique_ptr<ControlObject> m_pMasterBarPhase;
    std::unique_ptr<ControlPushButton> m_pTakeMaster;
    std::unique_ptr<ControlObject> m_pIsMaster;
    std::unique_ptr<ControlPushButton> m_pSyncEnabled;
};

} // namespace prolink
} // namespace mixxx
