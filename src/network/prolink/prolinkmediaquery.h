#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include "network/prolink/prolinkdefs.h"

class QUdpSocket;

namespace mixxx {
namespace prolink {

/// What a player says is in one of its slots.
struct MediaInfo {
    /// The volume label the DJ formatted the medium with — `Sam CDJ1000mk3`,
    /// and what the deck itself shows. UTF-16 **big**-endian on the wire, like
    /// the dbserver strings and unlike the NFS layer's UTF-16LE.
    ///
    /// **Often empty, and legitimately so**: an unlabelled stick reports no
    /// name at all while still carrying a full library. Absence of a name is
    /// not absence of media — that is what `isOccupied()` is for.
    QString name;
    quint32 trackCount = 0;
    quint32 playlistCount = 0;

    /// A slot with something in it. A deck answers for an empty slot too, with
    /// everything zeroed, so this is the distinction that matters.
    ///
    /// This is the first time we can know it at all: occupancy is published in
    /// status packets and nowhere else, and those are unicast only to peers that
    /// have announced themselves (F20/F21). Before the virtual CDJ, both slots
    /// had to be offered and allowed to fail.
    bool isOccupied() const {
        return trackCount > 0 || !name.isEmpty();
    }
};

/// Asks players what is in their slots, over UDP 50002.
///
/// A type-0x05 query in, a type-0x06 response out, carrying the volume name and
/// the track and playlist counts the deck shows in its own Link Info panel.
///
/// **Only works once we have announced.** A player unicasts on 50002 to peers
/// that have announced themselves and to nobody else — 1507 status packets in
/// one session all went deck-to-deck, and not one reached a host that had been
/// on the network the whole time without announcing (F21). Before the virtual
/// CDJ landed this class could not have existed.
///
/// Lives on the ProLink network thread.
class ProLinkMediaQuery : public QObject {
    Q_OBJECT

  public:
    explicit ProLinkMediaQuery(QObject* parent = nullptr);

    /// Bind UDP 50002. Shares the port, like the discovery socket, so rekordbox
    /// or another tool holding it does not shut us out.
    bool start();
    void stop();

    /// Ask *player* about *slot*. Fire and forget: the answer arrives as
    /// mediaInfoReceived(), including for an empty slot, which answers with
    /// everything zeroed rather than staying silent.
    void query(const QHostAddress& player,
            const QHostAddress& localAddress,
            int targetDeviceNumber,
            MediaSlot slot);

    void setRequesterNumber(int number) {
        m_requesterNumber = number;
    }

  signals:
    /// *player* is the responding device's address, which is how the caller maps
    /// it back to a MAC — the response carries a device number, and numbers can
    /// be reassigned, but the address is what we asked.
    void mediaInfoReceived(const QHostAddress& player,
            mixxx::prolink::MediaSlot slot,
            const mixxx::prolink::MediaInfo& info);

  private slots:
    void readPendingDatagrams();

  private:
    QUdpSocket* m_pSocket = nullptr;
    int m_requesterNumber = 0;
};

/// Build a type-0x05 media query. Exposed for the unit tests, which hold it to a
/// datagram captured from a real CDJ-2000NXS.
QByteArray buildMediaQuery(const QString& name,
        int requesterNumber,
        const QHostAddress& requesterIp,
        int targetDeviceNumber,
        MediaSlot slot);

/// Decode a type-0x06 media response. False if the datagram is not one.
bool parseMediaResponse(const QByteArray& datagram,
        int* pDeviceNumber,
        MediaSlot* pSlot,
        MediaInfo* pInfo);

} // namespace prolink
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::prolink::MediaInfo)
