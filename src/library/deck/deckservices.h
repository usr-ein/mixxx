#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <memory>

#include "library/deck/mediaregistry.h"
#include "preferences/usersettings.h"

class Library;
class SoundManager;

namespace mixxx {
namespace prolink {
class ProLinkControls;
class ProLinkKeySync;
class ProLinkNetworkService;
} // namespace prolink
namespace deck {

class BluetoothReceiver;
class DeckAutoplay;
class DeckHistory;
class DeckLoader;
class DeckRecorder;
class RemoteTrackStreamer;
class TrackCache;

/// The one owner of the deck's services, the network side included.
///
/// Made by CoreServices, with the `[ProLink]` controls, before anything parses
/// a skin: the controls have to exist by then (see ProLinkControls). Started by
/// the skin's first deck node (LegacySkinParser::parseDeckNode()), in the same
/// skin parse that builds the widgets it is handed to, so that each of them
/// connects before anything is read, polled or fetched. Stopped as the first
/// thing CoreServices::finalize() does, after the skin is gone. A skin reload
/// finds the services started and leaves them be. docs/fork-map.md,
/// "Ownership and lifetime", has the order of everything.
class DeckServices : public QObject {
    Q_OBJECT

  public:
    DeckServices();
    ~DeckServices() override;

    /// The one CoreServices made, or null. For parseDeckNode(), which starts
    /// the services and hands them to the widgets it builds: nothing else
    /// reaches for it.
    static DeckServices* instance();

    /// The deck the browser loads into, the network publishes and KEY SYNC
    /// pitches.
    static QString deckGroup();

    /// Make the deck's tables, forget last boot's analysis, and start the
    /// services, in the order the browser's constructor did. Once: a second
    /// call does nothing.
    void start(Library* pLibrary, UserSettingsPointer pConfig);

    /// Take the services down, in the order the browser's members went. The
    /// controls stay until this object goes.
    void stop();

    /// The sound manager the Bluetooth receiver lets go of while its menu is
    /// open: CoreServices makes it after this, and hands it over.
    void setSoundManager(SoundManager* pSoundManager);

    /// Null until start(), and again after stop().
    MediaRegistry* registry() const {
        return m_pRegistry.get();
    }
    TrackCache* cache() const {
        return m_pCache.get();
    }
    DeckLoader* loader() const {
        return m_pLoader.get();
    }
    DeckAutoplay* autoplay() const {
        return m_pAutoplay.get();
    }
    DeckHistory* history() const {
        return m_pHistory.get();
    }
    DeckRecorder* recorder() const {
        return m_pRecorder.get();
    }
    /// From construction to destruction, unlike the others: its [Bluetooth]
    /// controls are there before any mapping or skin, and it takes down a
    /// radio a crash left up before Mixxx opens the sound card.
    BluetoothReceiver* bluetooth() const {
        return m_pBluetooth.get();
    }

  signals:
    /// The registry's, passed on for the toast, and connected before anything
    /// else listens to the registry: what a medium did is said before what
    /// came of it -- a stick pulled during autoplay, then "Autoplay off".
    void mediumAppeared(mixxx::deck::MediumInfo medium);
    void mediumVanished(mixxx::deck::MediumInfo medium);
    void mediumFailed(mixxx::deck::MediumInfo medium);
    void mediumNotice(mixxx::deck::MediumInfo medium, const QString& text);

  private:
    QSqlDatabase database() const;

    std::unique_ptr<mixxx::prolink::ProLinkControls> m_pControls;
    std::unique_ptr<BluetoothReceiver> m_pBluetooth;
    Library* m_pLibrary = nullptr;
    std::unique_ptr<TrackCache> m_pCache;
    std::unique_ptr<mixxx::prolink::ProLinkNetworkService> m_pNetwork;
    std::unique_ptr<MediaRegistry> m_pRegistry;
    std::unique_ptr<RemoteTrackStreamer> m_pStreamer;
    std::unique_ptr<mixxx::prolink::ProLinkKeySync> m_pKeySync;
    std::unique_ptr<DeckLoader> m_pLoader;
    std::unique_ptr<DeckAutoplay> m_pAutoplay;
    std::unique_ptr<DeckHistory> m_pHistory;
    std::unique_ptr<DeckRecorder> m_pRecorder;
};

} // namespace deck
} // namespace mixxx
