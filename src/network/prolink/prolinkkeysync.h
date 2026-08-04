#pragma once

#include <QObject>
#include <memory>

#include "network/prolink/keysync.h"

class ControlProxy;

namespace mixxx {
namespace prolink {

class ProLinkControls;

/// KEY SYNC, wired to the deck and to the `[ProLink]` controls.
///
/// The rules live in KeySync, which knows nothing about Mixxx. This is the
/// shell around it: it publishes `key_sync_available`, watches
/// `key_sync_enabled` for the button, and pitches `[Channel1]` when the answer
/// changes.
///
/// # Why the pitch is re-applied on every load
///
/// The latch holds a *key*, not a number of semitones, and the semitones only
/// exist relative to whatever is on the platter. Load a new track under an
/// engaged sync and the old shift is meaningless — it was worked out against a
/// track that is no longer there. So the shift is recomputed from the new
/// track's own key, which is what keeps the deck in the key the DJ asked for
/// across a whole set rather than only until the next mix.
class ProLinkKeySync : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkKeySync(QObject* pParent = nullptr);
    ~ProLinkKeySync() override;

    /// What the network is offering, as often as it changes.
    ///
    /// Cheap and idempotent: it publishes `key_sync_available` and does nothing
    /// else. **It deliberately cannot disturb an engaged sync** — see KeySync
    /// rules 2 and 3.
    void setLink(bool otherIsMaster, mixxx::track::io::key::ChromaticKey masterKey);

  private:
    /// The button moved, or we moved it. Engages, refuses, or releases.
    void onEnabledChanged(double value);
    /// A track landed on the deck (or left it).
    void onFileKeyChanged(double value);
    /// Pitch the deck into the latched key, if there is a track to pitch.
    void applyToDeck();

    KeySync m_state;
    KeySync::Link m_link;

    /// The `[ProLink]` controls, used and not owned. Null in a build with no
    /// core services, and every use is guarded.
    ProLinkControls* m_pControls = nullptr;
    std::unique_ptr<ControlProxy> m_pDeckPitch;
    std::unique_ptr<ControlProxy> m_pDeckFileKey;
};

} // namespace prolink
} // namespace mixxx
