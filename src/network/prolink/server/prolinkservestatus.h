#pragma once

#include <QHostAddress>
#include <QList>
#include <QMetaType>
#include <QString>

#include "network/prolink/prolinkdefs.h"

namespace mixxx {
namespace prolink {
namespace server {

/// One of our slots, as the network sees it.
struct ServedSlot {
    MediaSlot slot = MediaSlot::Empty;
    /// The NFS export a player mounts for it: `/C/` or `/B/`.
    QString exportPath;
    /// The stick's own label, which is what a deck displays.
    QString volumeName;
    /// Where it is mounted on this machine, so the DJ can tell two sticks apart
    /// when both are unlabelled.
    QString localPath;
    int trackCount = 0;
    int playlistCount = 0;
    /// The stick is **gone**, and a player is still playing off it.
    ///
    /// It stays announced -- that is what keeps the consumer's mount valid and
    /// its player from erroring out mid-track -- and is served from copies made
    /// while the stick was in. It is no longer browsable, so nobody can start
    /// something we could not finish, and it leaves for real once the last
    /// consumer moves on (browser-prd.md 12.5).
    bool phantom = false;
};

/// A player that has loaded one of *our* tracks.
///
/// Read from the consumer's own status packets rather than from what it asked
/// us for, and the difference matters. A dbserver request log says what was
/// *browsed* — a deck scrolling a list requests metadata for rows the DJ never
/// loads, and a track loaded ten minutes ago generates no further requests at
/// all. The status packet carries `source_player`, `source_slot` and `track_id`
/// continuously, so it says what is *loaded right now* and from which of our
/// slots. That is the question worth answering on this page.
struct ServeConsumer {
    int deviceNumber = 0;
    QString deviceName;
    QHostAddress address;
    /// Which of our slots the track came from.
    MediaSlot slot = MediaSlot::Empty;
    quint32 trackId = 0;
    QString title;
    QString artist;
    /// Whether the deck is actually playing it, as opposed to holding it cued.
    bool playing = false;
};

/// Everything the ProLink page needs to say about what we are serving.
struct ServeStatus {
    /// False until we have claimed a player number: everything here is inert
    /// without one, since a deck validates it in every dbserver request.
    bool active = false;
    int deviceNumber = 0;
    /// What we announce ourselves as — a real model name, because a deck that
    /// does not recognise the string may not offer us as a source at all.
    QString deviceName;
    QHostAddress address;
    QString interfaceName;

    /// Zero when the listener could not be bound. The portmapper is the one
    /// that matters: without it a deck never discovers our mount ports and
    /// never reaches the dbserver at all (F46).
    quint16 portmapPort = 0;
    quint16 mountdPort = 0;
    quint16 nfsdPort = 0;
    quint16 dbserverPort = 0;

    QList<ServedSlot> media;
    QList<ServeConsumer> consumers;

    /// Evidence that a deck is really talking to us, in the order the protocol
    /// makes it happen. A zero further down the chain than a non-zero above it
    /// is where to look when something is wrong.
    int mediaQueriesAnswered = 0;
    int mountCalls = 0;
    int readCalls = 0;
    int dbserverClients = 0;

    bool servesAnything() const {
        return !media.isEmpty();
    }
};

} // namespace server
} // namespace prolink
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::prolink::server::ServeStatus)
