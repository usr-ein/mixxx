#include "library/deck/deckservices.h"

#include "library/deck/deckautoplay.h"
#include "library/deck/deckloader.h"
#include "library/deck/pdbingest.h"
#include "library/deck/ramstore.h"
#include "library/deck/remotetrackstreamer.h"
#include "library/deck/sessionpurge.h"
#include "library/deck/trackcache.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_deckservices.cpp"
#include "network/prolink/prolinkcontrols.h"
#include "network/prolink/prolinkkeysync.h"
#include "network/prolink/prolinknetworkservice.h"
#include "track/keyutils.h"
#include "util/assert.h"

namespace mixxx {
namespace deck {

namespace {
DeckServices* s_pInstance = nullptr;
} // namespace

DeckServices::DeckServices()
        : m_pControls(std::make_unique<mixxx::prolink::ProLinkControls>()) {
    DEBUG_ASSERT(s_pInstance == nullptr);
    s_pInstance = this;
}

DeckServices::~DeckServices() {
    stop();
    if (s_pInstance == this) {
        s_pInstance = nullptr;
    }
}

DeckServices* DeckServices::instance() {
    return s_pInstance;
}

QString DeckServices::deckGroup() {
    return QStringLiteral("[Channel1]");
}

void DeckServices::start(Library* pLibrary, UserSettingsPointer pConfig) {
    if (m_pRegistry) {
        return;
    }
    m_pLibrary = pLibrary;

    // The tables have to exist before anything reads them, and they are
    // temporary -- dropped and recreated each boot, never migrated.
    {
        QSqlDatabase db = database();
        if (db.isOpen()) {
            dropTables(db);
            createTables(db);
        }
    }

    // What Mixxx's own library remembers about the session's tracks goes at
    // the first start of a boot: beats, cues and the waveform files of every
    // copy played off a stick (docs/plain-usb-plan.md D1). Before anything is
    // loaded, so nothing on the deck is purged from under it.
    purgeSessionTracksIfNewBoot(m_pLibrary->trackCollectionManager(),
            pConfig->getSettingsPath(),
            {RamStore::root(), TrackCache::diskTierRoot(), QStringLiteral("/media")});

    // The deck never plays off removable media; everything goes through here.
    m_pCache = std::make_unique<TrackCache>();

    // Nothing is copied for a row the selection rests on, only for a track
    // that is loaded. A copy started for every row a DJ paused on while
    // looking for a track queued whole files on the stick, one after another,
    // and on a slow one those took the bandwidth that the track on the deck,
    // and any player reading the stick over the network, needed.

    m_pNetwork = std::make_unique<mixxx::prolink::ProLinkNetworkService>(
            deckGroup(), m_pControls.get());
    m_pRegistry = std::make_unique<MediaRegistry>(
            m_pLibrary->dbConnectionPool(), m_pNetwork.get(), m_pCache.get());
    connect(m_pRegistry.get(),
            &MediaRegistry::mediumAppeared,
            this,
            &DeckServices::mediumAppeared);
    connect(m_pRegistry.get(),
            &MediaRegistry::mediumVanished,
            this,
            &DeckServices::mediumVanished);
    connect(m_pRegistry.get(), &MediaRegistry::mediumFailed, this, &DeckServices::mediumFailed);
    connect(m_pRegistry.get(), &MediaRegistry::mediumNotice, this, &DeckServices::mediumNotice);
    m_pStreamer = std::make_unique<RemoteTrackStreamer>(
            m_pRegistry.get(), m_pNetwork.get(), m_pCache.get());
    // KEY SYNC borrows the master's key, which only the registry can tell:
    // it is in the copy of the master's medium database the registry read.
    // Listening before the session starts, which is when the first answer
    // comes.
    m_pKeySync = std::make_unique<mixxx::prolink::ProLinkKeySync>(
            deckGroup(), m_pControls.get());
    connect(m_pRegistry.get(),
            &MediaRegistry::masterKeyChanged,
            m_pKeySync.get(),
            [pKeySync = m_pKeySync.get()](bool otherIsMaster, int masterKeyId) {
                pKeySync->setLink(otherIsMaster, KeyUtils::keyFromNumericValue(masterKeyId));
            });
    m_pNetwork->start();
    m_pLoader = std::make_unique<DeckLoader>(deckGroup(),
            m_pLibrary,
            m_pRegistry.get(),
            m_pStreamer.get(),
            m_pCache.get());
    connect(m_pLoader.get(),
            &DeckLoader::loadTrackToPlayer,
            m_pLibrary,
            &Library::slotLoadTrackToPlayer);
    // Autoplay loads through the same loader as the DJ, so a track it picks
    // is copied off its medium, announced and marked in the list exactly like
    // one the DJ loads.
    m_pAutoplay = std::make_unique<DeckAutoplay>(deckGroup(),
            m_pRegistry.get(),
            [this]() { return database(); },
            m_pLoader.get());
    // A medium that has gone cannot be re-read, so what is cached from it is
    // now the only copy and must not be dropped to reclaim space.
    connect(m_pRegistry.get(),
            &MediaRegistry::mediumVanished,
            m_pCache.get(),
            [pCache = m_pCache.get()](const MediumInfo& medium) {
                pCache->markUnreachable(medium.id);
            });
    // And when it comes back -- the same stick, by its mount and UUID -- what
    // is cached from it can be read again, so it is dropped to make room
    // rather than written to the card.
    connect(m_pRegistry.get(),
            &MediaRegistry::mediumAppeared,
            m_pCache.get(),
            [pCache = m_pCache.get()](const MediumInfo& medium) {
                if (medium.id.isLocal()) {
                    pCache->markReachable(medium.id);
                }
            });
}

void DeckServices::stop() {
    // The browser's members went last first, and the cache was its last.
    m_pCache.reset();
    m_pAutoplay.reset();
    m_pLoader.reset();
    m_pKeySync.reset();
    m_pStreamer.reset();
    m_pRegistry.reset();
    m_pNetwork.reset();
}

QSqlDatabase DeckServices::database() const {
    return m_pLibrary->trackCollectionManager()->internalCollection()->database();
}

} // namespace deck
} // namespace mixxx
