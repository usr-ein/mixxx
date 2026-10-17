#pragma once

#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
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
/// receiver" menu is open, a device -- a phone, a Mac -- plays straight to the
/// DAC and the screen shows what it plays (WDeckBluetooth). Sam: "While it's
/// receiving music in this menu, the deck itself is not playing from Mixxx
/// anymore", and the radio is on only while the menu is open.
///
/// **Opening** (setActive(true); the home list's row refuses it while the
/// deck plays, as a wrong row mid-set would be silence) keeps the deck
/// paused, has Mixxx let go of the sound card
/// (SoundManager::releaseDevices()), then brings the radio up:
/// `trimixxx-bluetooth up`, as root through sudo -n
/// (pi_config/bluetooth), which starts bluetoothd, BlueALSA and the player
/// that sends the device's stream to the DAC. **Closing** takes all of that
/// down (`trimixxx-bluetooth down`, which frees the DAC first), then takes the
/// sound card back (SoundManager::setupDevices()). A card that will not come
/// back is said (notice()) and tried again until it does: the deck is never
/// left silent without a word. Either can be asked while the other is under
/// way; what was asked last wins.
///
/// **What plays** comes from BlueZ over D-Bus, not through Mixxx: AVRCP's
/// track, position and status (org.bluez.MediaPlayer1), whether the audio
/// streams (MediaTransport1), the device's name (Device1), and the cover when
/// the device offers one, fetched by obexd (AVRCP's imaging, BIP) through a
/// cover session opened as soon as the device says where (its player's
/// ObexPort): a device gives a track's cover only while one is open. The
/// position is the device's: each one BlueZ reports replaces the deck's,
/// backwards as much as forwards (a seek back to the start). **The deck's
/// controls drive the device** while the menu is open: `[Bluetooth]`'s
/// play_pause, previous and next (the mapping's PLAY, loop halve and loop
/// double; the page's buttons call playPause(), previous(), next()), and
/// `playing` says whether it plays, for PLAY's light.
///
/// Mixxx is also the agent BlueZ asks for pairing (BluetoothAgent): visible
/// only after pair(), with the six-digit code confirmed on the deck
/// (answerPairing()). BlueZ remembers paired devices (on p8, on a release
/// card), and the page lists them: **nothing connects until the DJ picks
/// one** (pick()), and only the one picked is let in; any other trying to
/// connect or take over is refused until picked (Sam: "I don't want it
/// jumping clients without being told"). The one exception is the DJ's
/// **starred** device (star(), one at most, kept on p8 with the pairings by
/// the helper): the deck picks it by itself when the menu opens, or whenever
/// nothing is picked while it is open, and asks it to connect every
/// kAutoConnectRetryMs until it is in (Sam: "Autoconnect should happen when
/// the bluetooth menu is opened"); a pick of the DJ's stands over it. A device
/// just paired is picked. forget() unpairs one, its keys gone with it.
///
/// **A crash** leaves nothing up: the helper's guard takes the radio down
/// when this Mixxx's process goes, and the constructor takes down whatever a
/// crash left before Mixxx opens the sound card. Nothing starts at boot.
///
/// Made with DeckServices, before any skin, for its `[Bluetooth]` controls,
/// which the mapping watches from its init on: `active` (1 while the menu is
/// open), `screen_off` (the screen and the button lights off), `wake` (set
/// by the mapping on any control, which keeps the screen on, or wakes it),
/// `play_pause`, `previous` and `next` (set by the mapping: asked of the
/// device), and `playing` (the device plays; read only).
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

    /// Pair: visible to devices for the Pair window (DiscoverableTimeout).
    void pair();
    /// The code on screen, confirmed (true) or refused on the deck.
    void answerPairing(bool accept);
    /// The paired device at *path* plays: whatever else is connected goes,
    /// and it is asked to connect.
    void pick(const QString& path);
    /// The paired device at *path* unpaired: disconnected, and its keys gone.
    void forget(const QString& path);
    /// The paired device at *path* starred, in place of the one starred
    /// before; or, starred already, no device starred.
    void star(const QString& path);

    /// The device's player: play or pause, from what it says it is doing.
    void playPause();
    void previous();
    void next();

    const bluetooth::Facts& facts() const {
        return m_facts;
    }
    /// What devices list the deck as: the adapter's alias, else the hostname.
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
    void onObexInterfacesRemoved(const QDBusMessage& message);

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
    /// Trusted, which lets a device in without the agent: *path* only, or
    /// none for an empty one.
    void trustOnly(const QString& path);
    /// A Device1 method, asked and not waited for.
    void callDevice(const QString& path, const QString& method);
    /// The starred device's object path, if it is paired.
    QString starredPath() const;
    /// One object's interfaces, as BlueZ reported them.
    void store(const QString& path, const QString& interface, const QVariantMap& properties);
    /// The facts again, from what BlueZ has said.
    void recompute();
    void setAdapterProperty(const QString& name, const QVariant& value);
    /// The device's player asked *method* (MediaPlayer1's Play, Pause...).
    void callPlayer(const QString& method);
    /// *path* the device picked: trusted alone, whatever else is connected
    /// let go, and asked to connect.
    void take(const QString& path);
    /// Device1.Connect for the device picked, which is then "Connecting…".
    void connectPicked();
    /// The helper's `star` (ADDRESS, or "none"): kept on p8.
    void runStar(const QString& address);
    void fetchCover();
    /// The connected device's cover session: one open already (another
    /// client's), else a new one; tried again a few times when it fails.
    void ensureCoverSession();
    void openCoverSession();
    void retryCoverSession();
    void requestThumbnail();
    quint16 coverPort() const;
    QString connectedAddress() const;
    void clearCover();
    /// Whether a device may pair or connect without a code (the agent).
    bool allowed(const QString& device) const;

    const QString m_group;
    SoundManager* m_pSoundManager = nullptr;
    std::unique_ptr<ControlObject> m_pActive;
    std::unique_ptr<ControlObject> m_pScreenOff;
    std::unique_ptr<ControlObject> m_pWake;
    std::unique_ptr<ControlObject> m_pPlayPause;
    std::unique_ptr<ControlObject> m_pPrevious;
    std::unique_ptr<ControlObject> m_pNext;
    std::unique_ptr<ControlObject> m_pPlaying;
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
    /// The device connected: the one picked, once it is in.
    QString m_connectedPath;
    QString m_playerPath;
    /// The device whose code is on screen.
    QString m_pairingPath;
    /// The device picked, the only one let in; none on opening.
    QString m_pickedPath;
    /// The pick is the deck's own (the starred device), not the DJ's: it is
    /// asked again until it is in.
    bool m_autoPicked = false;
    QTimer m_autoConnectRetry;
    /// The one picked, while it is asked to connect.
    QString m_connectingPath;
    /// Paired devices in without being picked, being disconnected.
    QSet<QString> m_refused;
    /// The starred device's address, as the helper keeps it on p8: what is
    /// read at each `up` and written by star().
    QString m_starredAddress;

    bluetooth::Facts m_facts;
    QElapsedTimer m_clock;

    /// The cover: the handle it is for, the obex session and transfer it
    /// comes through (the session ours, or one another client opened to the
    /// device), and the file it landed in.
    QString m_coverHandle;
    QString m_obexSession;
    /// The device a cover session is being found or opened for, and the
    /// tries that failed for the one connected.
    QString m_coverSessionFor;
    int m_coverSessionTries = 0;
    QString m_obexTransfer;
    QString m_obexFile;
    QString m_coverPath;
};

} // namespace deck
} // namespace mixxx
