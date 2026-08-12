#include "network/prolink/prolinkcontrols.h"

#include "util/assert.h"

namespace {
mixxx::prolink::ProLinkControls* s_pInstance = nullptr;

const QString kGroup = QStringLiteral("[ProLink]");

ConfigKey key(const char* item) {
    return ConfigKey(kGroup, QString::fromLatin1(item));
}

const QString kFxGroup = QStringLiteral("[EffectTempo]");

ConfigKey fxKey(const char* item) {
    return ConfigKey(kFxGroup, QString::fromLatin1(item));
}
} // namespace

namespace mixxx {
namespace prolink {

/*static*/ ProLinkControls* ProLinkControls::instance() {
    return s_pInstance;
}

ProLinkControls::ProLinkControls() {
    m_pPullDb = std::make_unique<ControlPushButton>(key("pull_db"));

    m_pFxBpm = std::make_unique<ControlObject>(fxKey("bpm"));
    m_pFxBpmSource = std::make_unique<ControlObject>(fxKey("source"));
    m_pFxBpm->setReadOnly();
    m_pFxBpmSource->setReadOnly();

    m_pMasterDevice = std::make_unique<ControlObject>(key("master_device"));
    m_pMasterBpm = std::make_unique<ControlObject>(key("master_bpm"));
    m_pMasterBarPhase = std::make_unique<ControlObject>(key("master_bar_phase"));
    // Read-only, because nothing in Mixxx may tell a CDJ what phase it is at
    // and a skin binding that could write these would look like it worked.
    m_pMasterDevice->setReadOnly();
    m_pMasterBpm->setReadOnly();
    m_pMasterBarPhase->setReadOnly();
    m_pMasterBarPhase->forceSet(-1.0);
    m_pMasterBpm->forceSet(0.0);
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

    // TOGGLE, unlike `take_master`: this one is not a request anybody has to
    // grant. It latches a key off the network and then holds it, so it has a
    // state of its own and the button shows exactly that state.
    m_pKeySyncEnabled = std::make_unique<ControlPushButton>(key("key_sync_enabled"));
    m_pKeySyncEnabled->setButtonMode(ControlPushButton::TOGGLE);
    m_pKeySyncAvailable = std::make_unique<ControlObject>(key("key_sync_available"));
    m_pKeySyncAvailable->setReadOnly();
    m_pKeySyncAvailable->forceSet(0.0);

    VERIFY_OR_DEBUG_ASSERT(s_pInstance == nullptr) {
        return;
    }
    s_pInstance = this;
}

ProLinkControls::~ProLinkControls() {
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

} // namespace prolink
} // namespace mixxx
