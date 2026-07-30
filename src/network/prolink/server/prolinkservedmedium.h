#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

#include "network/prolink/prolinkdefs.h"
#include "network/prolink/prolinkpdb.h"

namespace mixxx {
namespace prolink {
namespace server {

/// One medium we serve: a slot, a mounted rekordbox volume, and its library.
///
/// TriMiXxX has two USB ports and presents them to the network as a USB and an
/// SD, which is the pair a CDJ expects. Serving two media is **one** dbserver
/// holding a medium per slot, not a server per slot: a player browsing both
/// opens a single connection and tells them apart purely by the slot byte in
/// each request's descriptor (F37).
///
/// Only a mounted rekordbox medium is ever served. Mixxx's own library is not
/// exposed here and must not be: it has no `export.pdb`, no ANLZ files and no
/// stable track ids, and a deck told about tracks it cannot then analyse would
/// stall at NOW LOADING rather than fail cleanly.
class ProLinkServedMedium {
  public:
    /// Read a mounted rekordbox medium. `ok` is false if it holds no usable
    /// `export.pdb` — serving a medium we cannot enumerate would put an empty
    /// library on the network, and a deck told a medium has no tracks has no
    /// reason to offer it (F24).
    static ProLinkServedMedium fromVolume(MediaSlot slot, const QString& volumePath);

    bool isValid() const {
        return m_ok;
    }
    MediaSlot slot() const {
        return m_slot;
    }
    const QString& rootPath() const {
        return m_rootPath;
    }
    /// The volume label, as the deck will show it in its Link Info panel.
    const QString& volumeName() const {
        return m_volumeName;
    }
    /// The medium's saved utility settings, or empty if it has none — which is
    /// normal, and which the reply has a representation for.
    const QByteArray& settings() const {
        return m_settings;
    }

    const PdbContents& library() const {
        return m_library;
    }
    int trackCount() const {
        return m_library.tracks.size();
    }
    int playlistCount() const {
        return m_library.playlistCount();
    }

    /// One track by rekordbox id, or nullptr. Valid until this medium is
    /// destroyed or reassigned.
    const PdbTrack* track(quint32 id) const;
    /// Every track, ordered artist then title — the library's own order, which
    /// is what the DEFAULT sort means.
    QList<const PdbTrack*> tracksInDefaultOrder() const;

    /// The cover image for *artworkId*, or empty. Read straight off the medium:
    /// we are serving it, so there is no fetch involved.
    QByteArray artwork(quint32 artworkId) const;

    /// The raw `.DAT` and `.EXT` analysis files for a track.
    ///
    /// Read together and cached, because a load asks for four tags across the
    /// two within a few milliseconds and asks again whenever the DJ reloads.
    /// Either may come back empty, which costs that tag and not the load.
    void analysisFiles(quint32 trackId, QByteArray* pDat, QByteArray* pExt) const;

    /// Human-readable one-liner for the log.
    QString toString() const;

  private:
    void index();

    bool m_ok = false;
    MediaSlot m_slot = MediaSlot::Empty;
    QString m_rootPath;
    QString m_volumeName;
    QByteArray m_settings;
    PdbContents m_library;

    /// Indices into `m_library.tracks`, not pointers into it. A medium is
    /// returned and assigned by value, and QList's implicit sharing would leave
    /// pointers aimed at whichever copy detached first — an aliasing bug that
    /// only shows up once a second medium is mounted.
    QHash<quint32, int> m_byId;
    QList<int> m_defaultOrder;
    /// track id to (.DAT, .EXT). Mutable so the cache can fill from const
    /// accessors: it is a memo of the medium's own contents, not state.
    mutable QHash<quint32, QPair<QByteArray, QByteArray>> m_analysisCache;
};

/// Where the rekordbox database lives on every medium, relative to its root.
constexpr char kPdbPath[] = "PIONEER/rekordbox/export.pdb";
/// The utility settings a deck can adopt over LINK (F38).
constexpr char kMySettingPath[] = "PIONEER/MYSETTING.DAT";

/// The settings bytes to put in a type-0x36 reply, or empty.
///
/// The container is uniform across the four `*SETTING*.DAT` variants: a 96-byte
/// header, a declared payload length, then the payload — all little-endian,
/// unlike the big-endian ANLZ files beside it. In `MYSETTING.DAT` the payload
/// opens with the constant `0x12345678` and a second word before the settings
/// themselves; `MYSETTING2.DAT` does not, so anything without the magic yields
/// empty rather than a guess at where its settings start.
QByteArray parseMySetting(const QByteArray& fileContents);

} // namespace server
} // namespace prolink
} // namespace mixxx
