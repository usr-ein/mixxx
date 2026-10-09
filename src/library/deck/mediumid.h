#pragma once

#include <QHash>
#include <QString>

#include <utility>

namespace mixxx {
namespace deck {

/// Which rekordbox-prepared volume a track came from.
///
/// The deck plays from exactly two kinds of place — a stick in one of its own
/// ports, or a slot on another Pro DJ Link player — and everything downstream
/// wants to treat those identically: one table, one set of queries, one browse
/// hierarchy. This is the identity that makes that possible.
///
/// **The key is what lands in SQL**, so it has to be stable across an unmount
/// and a remount, and distinct between two players holding clones of the same
/// stick. It is not a path and not a display name: an unlabelled stick has no
/// name at all, and two of them would collide.
///
/// Deliberately free of any Pro DJ Link include. The network layer formats its
/// own MAC and slot into a string and hands it over; this header stays a value
/// type that the library, the UI and the tests can all include for nothing.
class MediumId final {
  public:
    enum class Source {
        Local,   ///< A stick in this deck, mounted under /media.
        ProLink, ///< A slot on another player, reached over the network.
    };

    MediumId() = default;

    /// A stick of our own, identified by where it is mounted **and by which
    /// stick it is**.
    ///
    /// The mount point is unique and stable for as long as a stick is plugged
    /// in -- the label may be empty, and two sticks may share one -- but it is
    /// not unique *over time*: slots are handed out in plug order, so the next
    /// stick into the same port gets the same one. Everything keyed on this id
    /// then took the old stick for the new one, the track cache first: a copy
    /// is named after the medium and the file's path, so a second stick's
    /// `/01.mp3` was handed the first stick's copy of a different song, and the
    /// beats Mixxx had saved against it.
    ///
    /// *volumeId* is the filesystem's UUID, which is what tells the two apart.
    /// Empty when it cannot be read, which is the old behaviour and no worse.
    static MediumId local(const QString& mountPoint, const QString& volumeId = QString()) {
        QString key = kLocalPrefix + mountPoint;
        if (!volumeId.isEmpty()) {
            key += kVolumeMarker + volumeId;
        }
        return MediumId(Source::Local, key);
    }

    /// A slot on a player. *deviceKey* is the owning player's MAC as hex and
    /// *slot* its slot number, which together survive a device number being
    /// reassigned — numbers move between sessions, MACs do not.
    static MediumId proLink(const QString& deviceKey, int slot) {
        return MediumId(Source::ProLink,
                kProLinkPrefix + deviceKey + kSlotMarker + QString::number(slot));
    }

    /// Rebuild from a key read back out of the database.
    static MediumId fromKey(const QString& key) {
        return MediumId(key.startsWith(kLocalPrefix) ? Source::Local : Source::ProLink, key);
    }

    Source source() const {
        return m_source;
    }
    bool isLocal() const {
        return m_source == Source::Local;
    }
    /// The value stored in `deck_library.medium`. Empty for a default-constructed
    /// id, which is never a medium anything was read from.
    const QString& key() const {
        return m_key;
    }
    bool isValid() const {
        return !m_key.isEmpty();
    }

    /// Where a local medium is mounted. Empty for a remote one.
    QString mountPoint() const {
        if (m_source != Source::Local) {
            return QString();
        }
        const auto marker = m_key.lastIndexOf(kVolumeMarker);
        return marker < 0 ? m_key.mid(kLocalPrefix.size())
                          : m_key.mid(kLocalPrefix.size(), marker - kLocalPrefix.size());
    }

    /// The filesystem UUID a local medium was identified by, or empty.
    QString volumeId() const {
        if (m_source != Source::Local) {
            return QString();
        }
        const auto marker = m_key.lastIndexOf(kVolumeMarker);
        return marker < 0 ? QString() : m_key.mid(marker + kVolumeMarker.size());
    }

    /// The player a remote medium is on, as proLink() was given it: its MAC
    /// as hex. Empty for a local one.
    QString deviceKey() const {
        if (m_source != Source::ProLink) {
            return QString();
        }
        const auto marker = m_key.indexOf(kSlotMarker);
        return marker < 0 ? QString()
                          : m_key.mid(kProLinkPrefix.size(), marker - kProLinkPrefix.size());
    }

    /// The slot of that player a remote medium is in, as proLink() was given
    /// it. 0 for a local one.
    int slot() const {
        if (m_source != Source::ProLink) {
            return 0;
        }
        const auto marker = m_key.indexOf(kSlotMarker);
        return marker < 0 ? 0 : m_key.mid(marker + kSlotMarker.size()).toInt();
    }

    friend bool operator==(const MediumId& lhs, const MediumId& rhs) {
        return lhs.m_key == rhs.m_key;
    }
    friend bool operator!=(const MediumId& lhs, const MediumId& rhs) {
        return !(lhs == rhs);
    }

  private:
    MediumId(Source source, QString key)
            : m_source(source), m_key(std::move(key)) {
    }

    /// A local key: `usb:<mount point>`, then the UUID after kVolumeMarker.
    static inline const QString kLocalPrefix = QStringLiteral("usb:");
    /// Between the mount point and the UUID in a local key. Spelled out rather
    /// than a single character so that a mount point containing a `#` -- not
    /// one dj-usb makes, but one a development box might -- still parses.
    static inline const QString kVolumeMarker = QStringLiteral("#uuid:");
    /// A remote key: `prolink:<MAC as hex>|<slot>`.
    static inline const QString kProLinkPrefix = QStringLiteral("prolink:");
    static inline const QString kSlotMarker = QStringLiteral("|");

    Source m_source = Source::Local;
    QString m_key;
};

inline size_t qHash(const MediumId& id, size_t seed = 0) {
    return qHash(id.key(), seed);
}

} // namespace deck
} // namespace mixxx
