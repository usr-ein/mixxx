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

    /// A stick of our own, identified by where it is mounted. The mount point
    /// is the only thing about a local medium that is guaranteed unique and
    /// stable for as long as it is plugged in — the label may be empty, and two
    /// sticks may share one.
    static MediumId local(const QString& mountPoint) {
        return MediumId(Source::Local, QStringLiteral("usb:") + mountPoint);
    }

    /// A slot on a player. *deviceKey* is the owning player's MAC as hex and
    /// *slot* its slot number, which together survive a device number being
    /// reassigned — numbers move between sessions, MACs do not.
    static MediumId proLink(const QString& deviceKey, int slot) {
        return MediumId(Source::ProLink,
                QStringLiteral("prolink:%1|%2").arg(deviceKey).arg(slot));
    }

    /// Rebuild from a key read back out of the database.
    static MediumId fromKey(const QString& key) {
        return MediumId(key.startsWith(QStringLiteral("usb:")) ? Source::Local
                                                               : Source::ProLink,
                key);
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
        return m_source == Source::Local ? m_key.mid(4) : QString();
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

    Source m_source = Source::Local;
    QString m_key;
};

inline size_t qHash(const MediumId& id, size_t seed = 0) {
    return qHash(id.key(), seed);
}

} // namespace deck
} // namespace mixxx
