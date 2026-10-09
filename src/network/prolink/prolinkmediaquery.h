#pragma once

#include <QMetaType>
#include <QString>

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
    /// Occupancy is published in status packets and nowhere else, and those are
    /// unicast only to peers that have announced themselves (F20/F21).
    bool isOccupied() const {
        return trackCount > 0 || !name.isEmpty();
    }
};

} // namespace prolink
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::prolink::MediaInfo)
