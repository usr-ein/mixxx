#include "network/prolink/prolinkcontrols.h"


namespace {
const QString kGroup = QStringLiteral("[ProLink]");

ConfigKey key(const char* item) {
    return ConfigKey(kGroup, QString::fromLatin1(item));
}
} // namespace

namespace mixxx {
namespace prolink {

ProLinkControls::ProLinkControls() {
    m_pPullDb = std::make_unique<ControlPushButton>(key("pull_db"));

    m_pMasterDevice = std::make_unique<ControlObject>(key("master_device"));
    m_pMasterBarPhase = std::make_unique<ControlObject>(key("master_bar_phase"));
    // Read-only, because nothing in Mixxx may tell a CDJ what phase it is at
    // and a skin binding that could write these would look like it worked.
    m_pMasterDevice->setReadOnly();
    m_pMasterBarPhase->setReadOnly();
    m_pMasterBarPhase->forceSet(-1.0);
    m_pMeterIsMaster = std::make_unique<ControlObject>(key("meter_is_master"));
    m_pMeterIsMaster->setReadOnly();
    m_pMeterLive = std::make_unique<ControlObject>(key("meter_live"));
    m_pMeterLive->setReadOnly();
    m_pMasterDevice->forceSet(0.0);

    // TRIGGER, not TOGGLE: pressing MASTER is a request that may take a couple
    // of packets to be granted, so the button must not hold a state of its own
    // -- what it displays comes from `is_master`, which is what the network
    // actually settled on.
    m_pTakeMaster = std::make_unique<ControlPushButton>(key("take_master"));
    m_pTakeMaster->setButtonMode(ControlPushButton::TRIGGER);
    m_pIsMaster = std::make_unique<ControlObject>(key("is_master"));
    m_pIsMaster->setReadOnly();
    m_pIsMaster->forceSet(0.0);

    m_pSyncEnabled = std::make_unique<ControlPushButton>(key("sync_enabled"));
    m_pSyncEnabled->setButtonMode(ControlPushButton::TOGGLE);
    m_pFollowing = std::make_unique<ControlObject>(key("following"));
    m_pFollowing->setReadOnly();
    m_pFollowing->forceSet(0.0);

    // TRIGGER, like `take_master`: a press is an action, and what it did is
    // `key_shift`, which is what the pad and the key readout show.
    m_pKeySync = std::make_unique<ControlPushButton>(key("key_sync"));
    m_pKeySync->setButtonMode(ControlPushButton::TRIGGER);
    m_pKeyShift = std::make_unique<ControlObject>(key("key_shift"));
    m_pKeyShift->forceSet(0.0);
    m_pKeySyncAvailable = std::make_unique<ControlObject>(key("key_sync_available"));
    m_pKeySyncAvailable->setReadOnly();
    m_pKeySyncAvailable->forceSet(0.0);

    // Persistent: read from mixxx.cfg, which is where a rig's measured value
    // lives (the fork never writes the file back, so it is set there).
    m_pPhaseTrimMs = std::make_unique<ControlObject>(
            key("phase_trim_ms"), true, false, true, 0.0);
}

ProLinkControls::~ProLinkControls() = default;

} // namespace prolink
} // namespace mixxx
