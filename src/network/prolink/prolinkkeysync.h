#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

#include "network/prolink/keysync.h"

class ControlProxy;

namespace mixxx {
namespace prolink {

class ProLinkControls;

/// KEY SYNC, wired to the deck and to the `[ProLink]` controls.
///
/// The rules live in KeySync, which knows nothing about Mixxx. This is the
/// shell around it, and the one thing that moves the deck's key: it answers
/// `key_sync` (the pad) and `key_shift` (the key list), pitches `[Channel1]`
/// by `pitch_adjust`, and publishes `key_sync_available`.
///
/// # A shift belongs to the track it was chosen for
///
/// A new track plays in its own key, and the pad goes dark (Sam, 2026-10-14).
/// The shift was worked out against the key of the track that was on the
/// deck; on the next one it would mean nothing, and one press matches the
/// master again.
class ProLinkKeySync : public QObject {
    Q_OBJECT

  public:
    /// *deckGroup* is the deck pitched. *pControls* are the `[ProLink]`
    /// controls, used and not owned; null only in a build that has none.
    ProLinkKeySync(const QString& deckGroup,
            ProLinkControls* pControls,
            QObject* pParent = nullptr);
    ~ProLinkKeySync() override;

    /// What the network is offering, as often as it changes.
    ///
    /// Cheap and idempotent: it publishes `key_sync_available` and does nothing
    /// else. **It deliberately leaves the shift alone** -- see KeySync rule 4.
    void setLink(bool otherIsMaster, mixxx::track::io::key::ChromaticKey masterKey);

  private:
    /// The pad.
    void onPressed(double value);
    /// `key_shift` written by someone else: the key list.
    void onShiftRequested(double value);
    /// Put the deck at *shift* from its own key, and say so everywhere;
    /// *cause* is for the log.
    void applyShift(int shift, const char* cause);
    /// `key_sync_available`, from the link, the track's key and the shift.
    void publishAvailable();

    int shift() const;
    mixxx::track::io::key::ChromaticKey fileKey() const;

    /// The shift applied, in semitones. Atomic, because the keylock watcher
    /// reads it on whichever thread toggled keylock.
    std::atomic<int> m_heldSteps{0};

    KeySync::Link m_link;

    const QString m_deckGroup;
    /// The `[ProLink]` controls, used and not owned. Null in a build with no
    /// core services, and every use is guarded.
    ProLinkControls* const m_pControls;
    std::unique_ptr<ControlProxy> m_pDeckPitchAdjust;
    std::unique_ptr<ControlProxy> m_pDeckFileKey;
    std::unique_ptr<ControlProxy> m_pDeckKeylock;
};

} // namespace prolink
} // namespace mixxx
