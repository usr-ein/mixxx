#include "network/prolink/server/prolinkvfs.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <algorithm>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkVfs");
} // namespace

namespace mixxx {
namespace prolink {
namespace server {

ProLinkVfs::ProLinkVfs() {
    Node root;
    root.path = QStringLiteral("/");
    root.isDirectory = true;
    root.fileId = 1;
    registerNode(root);
}

ProLinkVfs::~ProLinkVfs() = default;

QByteArray ProLinkVfs::handleFor(const QString& path) {
    const QByteArray digest = QCryptographicHash::hash(
            path.toUtf8(), QCryptographicHash::Sha256);
    return digest.left(kHandleSize);
}

QString ProLinkVfs::fold(const QString& name) {
    return name.normalized(QString::NormalizationForm_C).toCaseFolded();
}

ProLinkVfs::Entry* ProLinkVfs::registerNode(const Node& node) {
    const QByteArray key = handleFor(node.path).left(kHandlePrefix);
    auto existing = m_entries.find(key);
    if (existing != m_entries.end()) {
        // Refresh the metadata but keep the file id: a player may be holding it
        // from an earlier listing, and NFSv2 clients treat a changed fileid as a
        // different file.
        const quint32 fileId = existing->second.node.fileId;
        existing->second.node = node;
        existing->second.node.fileId = fileId;
        return &existing->second;
    }
    Entry entry;
    entry.node = node;
    if (entry.node.fileId == 0) {
        entry.node.fileId = m_nextFileId++;
    }
    return &m_entries.emplace(key, std::move(entry)).first->second;
}

ProLinkVfs::Entry* ProLinkVfs::entryFor(const QByteArray& handle) {
    const auto found = m_entries.find(handle.left(kHandlePrefix));
    return found == m_entries.end() ? nullptr : &found->second;
}

const ProLinkVfs::Entry* ProLinkVfs::entryFor(const QByteArray& handle) const {
    const auto found = m_entries.find(handle.left(kHandlePrefix));
    return found == m_entries.end() ? nullptr : &found->second;
}

void ProLinkVfs::mount(const QString& prefix, const QString& directory) {
    const QString path = QStringLiteral("/") + prefix;
    Node node;
    node.path = path;
    node.diskPath = directory;
    node.isDirectory = true;
    registerNode(node);
    m_mountRoots.insert(prefix, handleFor(path));
    kLogger.info() << "serving" << directory << "as" << path;
}

void ProLinkVfs::unmount(const QString& prefix) {
    const auto root = m_mountRoots.constFind(prefix);
    if (root == m_mountRoots.constEnd()) {
        return;
    }
    // Drop the subtree wholesale. Walking it to delete the children one by one
    // would need a parent index we do not otherwise keep; matching on the path
    // is O(handles) once per eject, against a table of a few thousand entries.
    const QString path = QStringLiteral("/") + prefix;
    const QString within = path + QLatin1Char('/');
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        const QString& nodePath = it->second.node.path;
        if (nodePath == path || nodePath.startsWith(within)) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // Folded keys are `<parent path>\n<folded name>`, so a key belongs to this
    // medium when its parent is the mount root itself or anything below it.
    const QString rootChildren = path + QLatin1Char('\n');
    for (auto it = m_folded.begin(); it != m_folded.end();) {
        if (it.key().startsWith(rootChildren) || it.key().startsWith(within)) {
            it = m_folded.erase(it);
        } else {
            ++it;
        }
    }
    m_mountRoots.remove(prefix);
    kLogger.info() << "stopped serving" << path;
}

bool ProLinkVfs::isMounted(const QString& prefix) const {
    return m_mountRoots.contains(prefix);
}

bool ProLinkVfs::resolve(const QByteArray& handle, Node* pNode) const {
    const Entry* pEntry = entryFor(handle);
    if (!pEntry) {
        return false;
    }
    if (pNode) {
        *pNode = pEntry->node;
    }
    return true;
}

void ProLinkVfs::enumerate(Entry* pEntry) {
    if (!pEntry || pEntry->enumerated || !pEntry->node.isDirectory) {
        return;
    }
    // Marked before the read, not after: a directory we cannot open is a
    // directory we should not retry on every lookup, and an empty listing is
    // the correct answer for one.
    pEntry->enumerated = true;
    if (pEntry->node.diskPath.isEmpty()) {
        return;
    }

    const QDir dir(pEntry->node.diskPath);
    const QFileInfoList children = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const QString base = pEntry->node.path == QStringLiteral("/")
            ? QString()
            : pEntry->node.path;
    for (const QFileInfo& info : children) {
        // Symlinks are not followed. A rekordbox medium has none, and following
        // one would let a crafted stick hand a deck anything on the Pi's
        // filesystem.
        if (info.isSymLink()) {
            continue;
        }
        Node child;
        child.path = base + QLatin1Char('/') + info.fileName();
        child.diskPath = info.absoluteFilePath();
        child.isDirectory = info.isDir();
        child.size = info.isDir() ? 0 : info.size();
        child.modifiedSecs = info.lastModified().toSecsSinceEpoch();
        registerNode(child);
        m_folded.insert(pEntry->node.path + QLatin1Char('\n') + fold(info.fileName()),
                handleFor(child.path));
    }
}

bool ProLinkVfs::lookup(const QByteArray& handle,
        const QString& name,
        QByteArray* pChildHandle,
        Node* pChild) {
    Entry* pParent = entryFor(handle);
    if (!pParent || !pParent->node.isDirectory) {
        return false;
    }
    // `..` and absolute or embedded paths are refused rather than resolved. A
    // LOOKUP names one component; anything else is either a broken client or an
    // attempt to walk out of the export.
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
            name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        return false;
    }
    enumerate(pParent);

    const QString base = pParent->node.path == QStringLiteral("/")
            ? QString()
            : pParent->node.path;
    // The exact spelling first, which is what almost every lookup takes, so a
    // genuinely case-sensitive tree still tells two names apart. Only on a miss
    // do we fall back to the fold — which is what makes a rekordbox medium work
    // at all.
    QByteArray childHandle = handleFor(base + QLatin1Char('/') + name);
    Entry* pEntry = entryFor(childHandle);
    if (!pEntry) {
        const auto folded = m_folded.constFind(
                pParent->node.path + QLatin1Char('\n') + fold(name));
        if (folded == m_folded.constEnd()) {
            return false;
        }
        childHandle = folded.value();
        pEntry = entryFor(childHandle);
        if (!pEntry) {
            return false;
        }
    }
    // Answer with the handle for the name **as stored**, never for the name as
    // asked: when the two differ only by case or normalisation, hashing the
    // request would mint a handle that is not in the table, and every later use
    // of it would come back NFSERR_STALE.
    if (pChildHandle) {
        *pChildHandle = handleFor(pEntry->node.path);
    }
    if (pChild) {
        *pChild = pEntry->node;
    }
    return true;
}

QList<ProLinkVfs::Node> ProLinkVfs::readDirectory(const QByteArray& handle) {
    QList<Node> out;
    Entry* pEntry = entryFor(handle);
    if (!pEntry || !pEntry->node.isDirectory) {
        return out;
    }
    enumerate(pEntry);

    const QString within = pEntry->node.path == QStringLiteral("/")
            ? QStringLiteral("/")
            : pEntry->node.path + QLatin1Char('/');
    for (const auto& pair : m_entries) {
        const QString& path = pair.second.node.path;
        if (!path.startsWith(within) || path == within) {
            continue;
        }
        // Direct children only: no further separator after the prefix.
        if (path.indexOf(QLatin1Char('/'), within.size()) >= 0) {
            continue;
        }
        out.append(pair.second.node);
    }
    std::sort(out.begin(), out.end(), [](const Node& a, const Node& b) {
        return a.path < b.path;
    });
    return out;
}

QByteArray ProLinkVfs::read(const QByteArray& handle, quint32 offset, quint32 count) {
    const Entry* pEntry = entryFor(handle);
    if (!pEntry || pEntry->node.isDirectory || pEntry->node.diskPath.isEmpty()) {
        return QByteArray();
    }
    QFile file(pEntry->node.diskPath);
    if (!file.open(QIODevice::ReadOnly)) {
        kLogger.debug() << "cannot open" << pEntry->node.diskPath;
        return QByteArray();
    }
    if (!file.seek(offset)) {
        return QByteArray();
    }
    return file.read(count);
}

} // namespace server
} // namespace prolink
} // namespace mixxx
