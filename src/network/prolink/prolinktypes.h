#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QMetaType>
#include <QString>

/// The values that cross from ProLinkNetworkService to the rest of Mixxx: a
/// slot, a device, what a device has in a slot.
///
/// The protocol itself is `lib/prolink`'s, in Rust. The prolinks-compat repo's
/// `docs/PROTOCOL.md` is its specification and `docs/FINDINGS.md` records how
/// each fact was established; finding numbers (F*n*) refer to that second
/// document.
///
/// No includes from `src/library/`: everything under `src/network/prolink/`
/// must stay usable, and unit-testable, without a Library.
namespace mixxx {
namespace prolink {

/// The slot byte of a dbserver request descriptor, and the discriminator when
/// one connection carries two media (F37).
enum class MediaSlot : quint8 {
    Empty = 0,
    Cd = 1,
    Sd = 2,
    Usb = 3,
    Rekordbox = 4,
};

/// The source player ProLinkSync::setLoadedTrack() takes for a track on a
/// medium we serve ourselves.
constexpr int kThisPlayer = -1;

/// One player, mixer or other device seen on the Pro DJ Link network.
///
/// A value type, so it can cross the thread boundary between the network thread
/// and the GUI as a plain copy in a queued signal.
///
/// **Identity is the MAC, not the device number and not the IP.** All three
/// change: a link-local address is self-assigned and can differ across boots, a
/// player in automatic mode picks a number that need not be the one it had
/// yesterday, and a DJ can renumber a deck mid-session from its UTILITY screen.
/// The MAC is the only field that stays put, so it is what the peer table keys
/// on — otherwise a renumbered deck appears twice and the original never times
/// out.
class ProLinkDevice {
  public:
    ProLinkDevice() = default;

    QByteArray mac;
    QHostAddress address;
    QString name;
    int deviceNumber = 0;

    /// Whether the discovery table considered this device present when the
    /// signal carrying it was emitted.
    ///
    /// Because deviceChanged is deliberately not emitted for plain keep-alives
    /// (four a second per deck would be noise), a mirror held by the GUI thread
    /// can be minutes old while the device is perfectly alive: this is the
    /// field to ask, not the age of the copy.
    bool online = true;
};

/// What a player says is in one of its slots.
struct MediaInfo {
    /// Whether the player's own status says the slot holds a mounted medium.
    ///
    /// Occupancy is published in status packets and nowhere else, and those are
    /// unicast only to peers that have announced themselves (F20/F21).
    bool hasMedia = false;
    /// The volume label the DJ formatted the medium with — `Sam CDJ1000mk3`,
    /// and what the deck itself shows. UTF-16 **big**-endian on the wire, like
    /// the dbserver strings and unlike the NFS layer's UTF-16LE.
    ///
    /// **Often empty, and legitimately so**: an unlabelled stick reports no
    /// name at all while still carrying a full library.
    QString name;
    /// Zero for a stick with no rekordbox library, which a player offers all
    /// the same, by its folders (PLAIN-STICKS.md §3).
    quint32 trackCount = 0;
    quint32 playlistCount = 0;
    /// A stick with no rekordbox library: no `export.pdb` to fetch, and read
    /// by its FOLDER menus instead. Its player says so when it describes it.
    bool plain = false;

    /// A slot with something in it, as its player's status says.
    ///
    /// Never read off the description: a stick with no label and no rekordbox
    /// library is described with no name and no tracks, exactly as an empty
    /// slot is, and is a medium all the same.
    bool isOccupied() const {
        return hasMedia;
    }
};

/// One folder or file of a player's plain stick, as its FOLDER menus list it.
struct PlainEntry {
    /// Its id on the player's medium.
    quint32 id = 0;
    /// The folder holding it, or 0 at the top of the stick.
    quint32 parent = 0;
    /// Its name; a file's with its extension.
    QString name;
    bool isFolder = false;
};

/// What a player says of one of its plain files, a file off a stick with no
/// rekordbox library: what `GET_GENERIC_METADATA` answers from the file's tags
/// (PLAIN-STICKS.md §5). The title is the file's name when it has no tag.
struct PlainFileTags {
    /// The file's id on its player's medium.
    quint32 id = 0;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    int durationSeconds = 0;
    /// The BPM tag, or 0.
    double bpm = 0.0;
    /// In kbps, or 0.
    int bitrate = 0;
};

/// What one player on the link is doing, as its status packets say: what it
/// has loaded and whether it is playing it.
///
/// Read off the session's table as it stands at the time of asking, on the GUI
/// thread; see ProLinkNetworkService::players().
struct ProLinkPlayer {
    /// What a platter is doing, as the status packet has it.
    enum class PlayState {
        NoTrack,
        Loading,
        Playing,
        Looping,
        Paused,
        Cued,
        /// Playing from the cue point while CUE is held.
        CuePlay,
        Searching,
        SpunDown,
        /// The medium went away mid-play and the deck plays its buffer.
        Emergency,
        Other,
    };

    int number = 0;
    /// Whether anything below means anything: status is only sent to a player
    /// that announced itself.
    bool hasStatus = false;
    /// The status's playing flag. Clear for a plain file even while it plays,
    /// on an emulated NXS at least (E12): ask playState as well.
    bool isPlaying = false;
    PlayState playState = PlayState::NoTrack;
    /// The loaded track's id, or 0.
    quint32 trackId = 0;
    /// Whose medium it came from, and which slot of it.
    int trackSourcePlayer = 0;
    MediaSlot trackSourceSlot = MediaSlot::Empty;
    /// Whether the loaded track is a rekordbox-analysed one: only then is
    /// trackId a row of that medium's export.pdb, and otherwise the player's
    /// own numbering.
    bool trackIsRekordbox = false;
    /// How old the last status is, in milliseconds; negative when there is
    /// none. A deck that has gone keeps its last status until it is forgotten,
    /// some 30 s on.
    double statusAgeMs = -1.0;
};

} // namespace prolink
} // namespace mixxx

// Both cross the network/GUI thread boundary in queued signals, which requires
// a registered metatype.
Q_DECLARE_METATYPE(mixxx::prolink::ProLinkDevice)
Q_DECLARE_METATYPE(mixxx::prolink::MediaInfo)
Q_DECLARE_METATYPE(mixxx::prolink::PlainFileTags)
