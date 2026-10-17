#pragma once

#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <memory>

#include "library/deck/bluetoothstate.h"

class ControlObject;
class ControlProxy;
class QDBusMessage;
class QDBusServiceWatcher;
class QProcess;
class SoundManager;

namespace mixxx {
namespace deck {

class BluetoothAgent;

/// The deck as a Bluetooth audio receiver: while the browser's "Bluetooth
/// receiver" menu is open, a phone plays straight to the DAC and the screen
/// shows what it plays (WDeckBluetooth). Sam: "While it's receiving music in
/// this menu, the deck itself is not playing from Mixxx anymore", and the
/// radio is on only while the menu is open.
///
/// **Opening** (setActive(true)) pauses the deck and keeps it paused, has
/// Mixxx let go of the sound card (SoundManager::releaseDevices()), then
/// brings the radio up: `trimixxx-bluetooth up`, as root through sudo -n
/// (pi_config/bluetooth), which starts bluetoothd, BlueALSA and the player
/// that sends the phone's stream to the DAC. **Closing** takes all of that
/// down (`trimixxx-bluetooth down`, which frees the DAC first), then takes the
/// sound card back (SoundManager::setupDevices()). A card that will not come
/// back is said (notice()) and tried again until it does: the deck is never
/// left silent without a word. Either can be asked while the other is under
/// way; what was asked last wins.
///
/// **What plays** comes from BlueZ over D-Bus, not through Mixxx: AVRCP's
/// track, position and status (org.bluez.MediaPlayer1), whether the audio
/// streams (MediaTransport1), the phone's name (Device1), and the cover when
/// the phone offers one, fetched by obexd (AVRCP's imaging, BIP). Mixxx is
/// also the agent BlueZ asks for pairing (BluetoothAgent): visible only after
/// Pair (pair()), with the six-digit code confirmed on the deck
/// (answerPairing()). Phones it knows are asked to reconnect whenever the
/// menu opens; BlueZ remembers them (on p8, on a release card).
///
/// **A crash** leaves nothing up: the helper's guard takes the radio down
/// when this Mixxx's process goes, and the constructor takes down whatever a
/// crash left before Mixxx opens the sound card. Nothing starts at boot.
///
/// Made with DeckServices, before any skin, for its `[Bluetooth]` controls,
/// which the mapping watches from its init on: `active` (1 while the menu is
/// open), `screen_off` (the screen and the button lights off) and `wake` (set
/// by the mapping on any control, which keeps the screen on, or wakes it).
class BluetoothReceiver : public QObject {
    Q_OBJECT

  public:
    explicit BluetoothReceiver(const QString& deckGroup, QObject* pParent = nullptr);
    ~BluetoothReceiver() override;

    /// The sound manager whose devices are let go of while the menu is open.
    /// Used and not owned; until it is set, nothing is released.
    void setSoundManager(SoundManager* pSoundManager);

    /// Mixxx is quitting: the radio down now, if it is up, and the sound
    /// manager forgotten (it goes before this does). Again, or from the
    /// destructor, does nothing.
    void shutdown();

    /// The menu opened (true) or closed (false).
    void setActive(bool active);
    bool isActive() const {
        return m_wanted;
    }

    /// Pair: visible to phones for the Pair window (DiscoverableTimeout).
    void pair();
    /// The code on screen, confirmed (true) or refused on the deck.
    void answerPairing(bool accept);

    const bluetooth::Facts& facts() const {
        return m_facts;
    }
    /// What phones list the deck as: the adapter's alias, else the hostname.
    QString deckName() const;
    /// The cover of what plays, a file, or empty.
    QString coverPath() const {
        return m_coverPath;
    }
    /// The monotonic clock facts().positionStampMs is on.
    qint64 nowMs() const {
        return m_clock.elapsed();
    }

    ControlObject* screenOffControl() const {
        return m_pScreenOff.get();
    }
    ControlObject* wakeControl() const {
        return m_pWake.get();
    }

  signals:
    /// Any of the facts, the cover or the pairing code changed.
    void changed();
    /// Something for the DJ to know, for the toast.
    void notice(const QString& text);

  private slots:
    void onBluezRegistered();
    void onBluezUnregistered();
    void onInterfacesAdded(const QDBusMessage& message);
    void onInterfacesRemoved(const QDBusMessage& message);
    void onPropertiesChanged(const QDBusMessage& message);
    void onObexPropertiesChanged(const QDBusMessage& message);

  private:
    enum class Helper {
        Up,
        Down,
    };

    /// The next thing to do towards what was asked, unless something is
    /// under way.
    void advance();
    void runHelper(Helper action);
    void onHelperFinished(Helper action, int exitCode, const QString& said);
    void releaseSound();
    void reacquireSound();
    void pauseDeck();

    void connectBluez();
    /// The agent unregistered, and stopListening().
    void disconnectBluez();
    /// BlueZ's signals no longer heard, and what it said forgotten.
    void stopListening();
    void readManagedObjects();
    void registerAgent();
    /// Paired phones asked to connect: the menu has just opened.
    void connectKnownPhones();
    /// One object's interfaces, as BlueZ reported them.
    void store(const QString& path, const QString& interface, const QVariantMap& properties);
    /// The facts again, from what BlueZ has said.
    void recompute();
    void setAdapterProperty(const QString& name, const QVariant& value);
    void fetchCover();
    void clearCover();
    /// Whether a device may pair or connect without a code (the agent).
    bool allowed(const QString& device) const;

    const QString m_group;
    SoundManager* m_pSoundManager = nullptr;
    std::unique_ptr<ControlObject> m_pActive;
    std::unique_ptr<ControlObject> m_pScreenOff;
    std::unique_ptr<ControlObject> m_pWake;
    /// The deck's play, held at 0 while the menu is open.
    std::unique_ptr<ControlProxy> m_pPlay;

    /// What was asked: the menu open.
    bool m_wanted = false;
    /// Mixxx has let go of the sound card, and has not got it back yet.
    bool m_soundReleased = false;
    int m_reacquireTries = 0;
    QTimer m_reacquireRetry;
    QProcess* m_pHelper = nullptr;

    QDBusServiceWatcher* m_pWatcher = nullptr;
    BluetoothAgent* m_pAgent = nullptr;
    bool m_bluezConnected = false;
    /// What BlueZ has said, by object and interface.
    QHash<QString, QHash<QString, QVariantMap>> m_objects;
    QString m_adapterPath;
    QString m_phonePath;
    QString m_playerPath;
    /// The phone whose code is on screen.
    QString m_pairingPath;
    /// Phones asked to connect once this time the menu opened.
    bool m_knownPhonesAsked = false;

    bluetooth::Facts m_facts;
    QElapsedTimer m_clock;

    /// The cover: the handle it is for, the obex session and transfer it
    /// comes through, and the file it landed in.
    QString m_coverHandle;
    QString m_obexSession;
    QString m_obexTransfer;
    QString m_obexFile;
    QString m_coverPath;
};

} // namespace deck
} // namespace mixxx
