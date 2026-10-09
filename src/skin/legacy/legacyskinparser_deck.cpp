// The deck's skin nodes, apart from the upstream parser so that its
// parseNode() carries one line for all of them: see parseDeckNode().

#include "skin/legacy/legacyskinparser.h"

#include "controllers/controllerlearningeventfilter.h"
#include "controllers/controllermanager.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/deck/deckservices.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "skin/legacy/skincontext.h"
#include "util/assert.h"
#include "widget/deck/wdeckautoplaybadge.h"
#include "widget/deck/wdeckbrowser.h"
#include "widget/deck/wdecktoast.h"
#include "widget/wprolinkphasemeter.h"
#include "widget/wtempopanel.h"

namespace {
const QStringList kDeckNodes = {
        QStringLiteral("DeckBrowser"),
        QStringLiteral("DeckToast"),
        QStringLiteral("DeckAutoplay"),
        QStringLiteral("ProLinkPhaseMeter"),
        QStringLiteral("TempoPanel"),
};
} // namespace

QWidget* LegacySkinParser::parseDeckNode(const QDomElement& node) {
    const QString nodeName = node.tagName();
    if (!kDeckNodes.contains(nodeName)) {
        return nullptr;
    }
    // The deck's services start with the skin's first deck node, in the skin
    // parse that builds every widget they are handed to: each connects before
    // anything is read, polled or fetched. Later calls find them started.
    mixxx::deck::DeckServices* pServices = mixxx::deck::DeckServices::instance();
    VERIFY_OR_DEBUG_ASSERT(pServices) {
        return nullptr;
    }
    pServices->start(m_pLibrary, m_pConfig);

    if (nodeName == "DeckBrowser") {
        auto* pBrowser = new mixxx::deck::WDeckBrowser(
                m_pParent, m_pLibrary, m_pConfig, pServices);
        commonWidgetSetup(node, pBrowser);
        pBrowser->setup(node, *m_pContext);
        pBrowser->installEventFilter(m_pKeyboard);
        // The browser loads tracks itself rather than going through WLibrary: it
        // has no LibraryView, and Library::slotLoadTrackToPlayer is the same entry
        // point the track table uses.
        pBrowser->Init();
        // Only the player knows when its deck goes empty -- and it ignores the
        // failure of a load that a newer one has already replaced, which a
        // track_loaded watcher could not tell apart.
        if (m_pPlayerManager) {
            if (BaseTrackPlayer* pDeck = m_pPlayerManager->getPlayer(
                        mixxx::deck::DeckServices::deckGroup())) {
                connect(pDeck,
                        &BaseTrackPlayer::playerEmpty,
                        pBrowser,
                        &mixxx::deck::WDeckBrowser::onDeckEmpty);
            }
        }
        return pBrowser;
    }
    if (nodeName == "DeckToast") {
        auto* pToast = new mixxx::deck::WDeckToast(m_pParent, pServices);
        commonWidgetSetup(node, pToast);
        pToast->setup(node, *m_pContext);
        // A track that will not load is said here rather than in Mixxx's modal
        // dialog, which the players no longer raise -- see slotLoadFailed().
        if (m_pPlayerManager) {
            for (int i = 0; i < m_pPlayerManager->numberOfDecks(); ++i) {
                if (BaseTrackPlayer* pDeck = m_pPlayerManager->getDeckBase(i)) {
                    connect(pDeck,
                            &BaseTrackPlayer::loadFailed,
                            pToast,
                            &mixxx::deck::WDeckToast::onLoadFailed);
                }
            }
        }
        // Deliberately NOT given the keyboard event filter: it takes no input at
        // all, and installing one on a widget that is transparent to the mouse
        // invites the question of why.
        pToast->Init();
        return pToast;
    }
    if (nodeName == "DeckAutoplay") {
        auto* pBadge = new mixxx::deck::WDeckAutoplayBadge(m_pParent, pServices->autoplay());
        commonWidgetSetup(node, pBadge);
        pBadge->setup(node, *m_pContext);
        // Like the toast, it takes no input, so no keyboard filter either.
        pBadge->Init();
        return pBadge;
    }
    if (nodeName == "ProLinkPhaseMeter") {
        const QString group = lookupNodeGroup(node);
        auto* pMeter = new WProLinkPhaseMeter(m_pParent, group);
        commonWidgetSetup(node, pMeter);
        pMeter->setup(node, *m_pContext);
        pMeter->installEventFilter(m_pKeyboard);
        pMeter->installEventFilter(
                m_pControllerManager->getControllerLearningEventFilter());
        pMeter->Init();
        return pMeter;
    }
    if (nodeName == "TempoPanel") {
        auto* pPanel = new WTempoPanel(m_pParent, lookupNodeGroup(node));
        commonWidgetSetup(node, pPanel);
        pPanel->setup(node, *m_pContext);
        pPanel->installEventFilter(m_pKeyboard);
        pPanel->Init();
        return pPanel;
    }
    return nullptr;
}
