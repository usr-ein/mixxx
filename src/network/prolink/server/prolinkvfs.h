#pragma once

#include <QByteArray>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QString>
#include <map>

namespace mixxx {
namespace prolink {
namespace server {

/// A read-only view of the media we serve, addressed by NFS filehandle.
///
/// Everything a real CDJ reads off us goes through here: `export.pdb`, the ANLZ
/// analysis files, and the audio itself. Write procedures do not exist — a
/// rekordbox export is read-only and so is this.
///
/// **The one place a CDJ breaks the spec.** NFSv2 says a filehandle is 32
/// *opaque* bytes the client must echo back verbatim. A CDJ-2000NXS does not.
/// Handed a handle, it returns one whose first 12 bytes match ours and whose
/// remaining 20 it has overwritten with its own data:
///
///     served:   8a5edab282632443219e051e 4ade2d1d5bbc671c781051bf1437897cbdfea0f1
///     returned: 8a5edab282632443219e051e 03012d0000001b58000000000303010000000162
///               |____ first 12 kept ____| |______ replaced by the player _______|
///
/// That matches the shape of a real player's own handles — a 4-byte value
/// repeated three times then zeros — so evidently the leading 12 bytes are the
/// volume identity and the rest is the player's own file reference, which it
/// feels free to rewrite. Consequently **only the first 12 bytes can be relied
/// upon**, and the handle table is keyed on those (F28).
///
/// Handles are a truncated SHA-256 of the path, which makes them deterministic:
/// the same tree yields the same handles across restarts, and 12 bytes is ample
/// to stay collision-free.
///
/// **Enumeration is lazy.** The Python proof of concept walked each medium up
/// front; on a Pi with a 60 GB stick that is thousands of `stat` calls before
/// the first byte is served. A directory is read the first time something looks
/// inside it, which is also exactly when a handle for its children first has to
/// exist. Anything not in the table is `NFSERR_STALE`, which is a legitimate
/// answer and what a deck expects after a medium is swapped.
class ProLinkVfs {
  public:
    ProLinkVfs();
    ~ProLinkVfs();

    /// Bytes of a filehandle a CDJ preserves. See the class comment.
    static constexpr int kHandlePrefix = 12;
    /// NFSv2 filehandles are exactly this long (RFC 1094).
    static constexpr int kHandleSize = 32;

    /// Graft a real directory in at */C* or */B*.
    ///
    /// *prefix* is what keeps two media apart. Sharing a root would mint the
    /// same handle for the same relative path — most obviously for the root
    /// itself — and since a deck keeps only 12 bytes there would be nothing
    /// left to tell them apart with afterwards.
    void mount(const QString& prefix, const QString& directory);
    /// Forget a medium and every handle beneath it. Handles a deck still holds
    /// then answer `NFSERR_STALE`, which is the truth.
    void unmount(const QString& prefix);
    bool isMounted(const QString& prefix) const;

    /// The handle for a VFS path. Pure: no registration, no lookup.
    static QByteArray handleFor(const QString& path);

    struct Node {
        QString path;
        QString diskPath;
        bool isDirectory = false;
        quint32 fileId = 0;
        qint64 size = 0;
        qint64 modifiedSecs = 0;
    };

    /// The node a handle names, or false if we have never issued that handle —
    /// which is `NFSERR_STALE`.
    bool resolve(const QByteArray& handle, Node* pNode) const;

    /// Resolve *name* inside the directory *handle* names, registering the
    /// child so its handle resolves later.
    ///
    /// Returns false for a miss (`NFSERR_NOENT`) as well as for a stale parent;
    /// the caller distinguishes them by resolving the parent itself.
    bool lookup(const QByteArray& handle,
            const QString& name,
            QByteArray* pChildHandle,
            Node* pChild);

    /// Directory contents, registered as a side effect so a later `lookup` or
    /// a handle from the listing resolves.
    QList<Node> readDirectory(const QByteArray& handle);

    /// Read a byte range. Empty for a directory, a stale handle, or a file that
    /// vanished under us — a medium yanked mid-read costs the read, not the
    /// server.
    QByteArray read(const QByteArray& handle, quint32 offset, quint32 count);

  private:
    struct Entry {
        Node node;
        /// Set once this directory's children have been registered, so a
        /// second lookup does not re-read it.
        bool enumerated = false;
    };

    Entry* entryFor(const QByteArray& handle);
    const Entry* entryFor(const QByteArray& handle) const;
    Entry* registerNode(const Node& node);
    /// Read *entry*'s directory into the table if it has not been read yet.
    void enumerate(Entry* pEntry);

    /// The key two spellings of one filename must agree on.
    ///
    /// A rekordbox medium is FAT32 — case-insensitive and case-preserving — and
    /// `export.pdb` does not necessarily record a name with the same case as the
    /// directory entry it refers to. On a real stick the database says
    /// `Gesaffelstein` where the directory is `GESAFFELSTEIN`. Worse, rekordbox
    /// wrote the database and the files through different APIs, so a name can be
    /// composed (NFC) in one and decomposed (NFD) in the other — `カガミ` is
    /// U+30AC in the pdb and U+30AB U+3099 on disk. A player asking for the name
    /// the database gave it would get `NFSERR_NOENT` for a file that is plainly
    /// there.
    static QString fold(const QString& name);

    /// A `std::map`, not a `QHash`, for one reason: **reference stability**.
    /// `enumerate()` holds a pointer to a directory's entry while inserting its
    /// children, and a rehashing container would leave that pointer dangling
    /// halfway through — a use-after-free that would only show up on a directory
    /// large enough to trigger a rehash.
    std::map<QByteArray, Entry> m_entries;
    /// Case- and normalisation-folded child index, keyed `<parentPrefix>/<fold>`,
    /// so the fallback match is a hash lookup rather than a scan of the
    /// directory. Only populated for directories that have been enumerated.
    QHash<QString, QByteArray> m_folded;
    QHash<QString, QByteArray> m_mountRoots;
    quint32 m_nextFileId = 2;
};

} // namespace server
} // namespace prolink
} // namespace mixxx
