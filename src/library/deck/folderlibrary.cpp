#include "library/deck/folderlibrary.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

namespace {

/// What the deck will try to play. Mixxx decodes more than this -- tracker
/// modules, Wavpack -- but a DJ stick does not carry them, and listing a
/// stray `.mod` from somebody's backup folder as a track helps nobody.
const QStringList kMusicSuffixes = {
        QStringLiteral("mp3"),
        QStringLiteral("m4a"),
        QStringLiteral("mp4"),
        QStringLiteral("aac"),
        QStringLiteral("flac"),
        QStringLiteral("wav"),
        QStringLiteral("aif"),
        QStringLiteral("aiff"),
        QStringLiteral("ogg"),
        QStringLiteral("opus"),
};

/// Directories never walked into. Compared case-insensitively, because FAT
/// and exFAT are, and a stick formatted on Windows says `$Recycle.Bin`.
const QStringList kSkippedDirectories = {
        QStringLiteral("System Volume Information"),
        QStringLiteral("$RECYCLE.BIN"),
        QStringLiteral("RECYCLER"),
        QStringLiteral("LOST.DIR"),
        QStringLiteral("FOUND.000"),
        // A CDJ writes MYSETTING.DAT here even on a stick with no rekordbox
        // library, so its presence says nothing about whether there is one.
        QStringLiteral("PIONEER"),
        QStringLiteral("Engine Library"),
        QStringLiteral("_Serato_"),
};

/// Cover names, best first.
const QStringList kCoverNames = {
        QStringLiteral("cover"),
        QStringLiteral("folder"),
        QStringLiteral("front"),
        QStringLiteral("album"),
        QStringLiteral("artwork"),
};
const QStringList kCoverSuffixes = {
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("png"),
};

QString suffixOf(const QString& fileName) {
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == fileName.size() - 1) {
        return QString();
    }
    return fileName.mid(dot + 1).toLower();
}

bool isAsciiDigit(QChar c) {
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

/// A directory of the stick, as the tree is being built.
struct Directory {
    /// As the filesystem spells it.
    QString name;
    /// For ordering only: see sortFold(). A stick written by a Mac stores NFD,
    /// and the same name in the two forms must sort the same.
    QString sortKey;
    /// Leading slash, no trailing one; empty for the root.
    QString relativePath;
    /// Indices into the listing, in the order they are listed.
    std::vector<int> files;
    std::vector<std::unique_ptr<Directory>> children;
    /// Lookup while building, by name as spelled.
    QHash<QString, Directory*> childByName;
};

QString lastComponent(const QString& relativePath) {
    const int slash = relativePath.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? relativePath : relativePath.mid(slash + 1);
}

/// A name with its accents taken off, for ordering: `Été` sorts with the Es,
/// not after `Zoo` where its code points would put it. Decomposed, and the
/// combining marks dropped -- which also makes the NFC and NFD spellings of a
/// name identical here, so a stick written by a Mac sorts like any other.
/// Letters with no decomposition (`ø`, `ß`) are left as they are.
QString sortFold(const QString& name) {
    const QString decomposed = name.normalized(QString::NormalizationForm_D);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing) {
            folded.append(c);
        }
    }
    return folded;
}

/// How a file sorts: its name without the extension first, so `Track.mp3`
/// comes before `Track 2.mp3` -- compared whole, the dot sorts after the space
/// -- and with its accents taken off; then the whole name, in NFC, only to
/// break a tie.
struct FileSortKey {
    QString stem;
    QString name;
};

/// Sort a directory and everything under it into the order it will be shown.
///
/// *fileKeys* are worked out once rather than inside the comparator, where a
/// 20 000-file stick would normalise each name a dozen times over.
void sortTree(Directory* pDirectory, const std::vector<FileSortKey>& fileKeys) {
    std::sort(pDirectory->files.begin(),
            pDirectory->files.end(),
            [&fileKeys](int left, int right) {
                const FileSortKey& a = fileKeys.at(static_cast<size_t>(left));
                const FileSortKey& b = fileKeys.at(static_cast<size_t>(right));
                const int byStem = mixxx::deck::naturalCompare(a.stem, b.stem);
                if (byStem != 0) {
                    return byStem < 0;
                }
                return mixxx::deck::naturalCompare(a.name, b.name) < 0;
            });
    std::sort(pDirectory->children.begin(),
            pDirectory->children.end(),
            [](const std::unique_ptr<Directory>& left,
                    const std::unique_ptr<Directory>& right) {
                const int byFold = mixxx::deck::naturalCompare(left->sortKey, right->sortKey);
                if (byFold != 0) {
                    return byFold < 0;
                }
                return mixxx::deck::naturalCompare(left->name, right->name) < 0;
            });
    for (const auto& child : pDirectory->children) {
        sortTree(child.get(), fileKeys);
    }
}

} // namespace

namespace mixxx {
namespace deck {

bool isMusicFileName(const QString& fileName) {
    // Hidden first: `._Track.mp3`, the AppleDouble shadow a Mac writes beside
    // every file on a FAT stick, has a perfectly good extension.
    if (fileName.isEmpty() || fileName.startsWith(QLatin1Char('.'))) {
        return false;
    }
    return kMusicSuffixes.contains(suffixOf(fileName));
}

bool isSkippedDirectoryName(const QString& name) {
    if (name.isEmpty() || name.startsWith(QLatin1Char('.'))) {
        return true;
    }
    for (const QString& skipped : kSkippedDirectories) {
        if (name.compare(skipped, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

int coverFileRank(const QString& fileName) {
    if (fileName.startsWith(QLatin1Char('.')) || !kCoverSuffixes.contains(suffixOf(fileName))) {
        return -1;
    }
    const QString base = fileName.left(fileName.lastIndexOf(QLatin1Char('.')));
    for (int rank = 0; rank < kCoverNames.size(); ++rank) {
        if (base.compare(kCoverNames.at(rank), Qt::CaseInsensitive) == 0) {
            return rank;
        }
    }
    return -1;
}

quint32 containerForFileName(const QString& fileName) {
    // rekordbox's own numbering (the pdb's track row, 0x5a), so a track built
    // here reads exactly like one parsed out of an export.
    const QString suffix = suffixOf(fileName);
    if (suffix == QStringLiteral("mp3")) {
        return 1;
    }
    if (suffix == QStringLiteral("m4a") || suffix == QStringLiteral("mp4") ||
            suffix == QStringLiteral("aac")) {
        return 4;
    }
    if (suffix == QStringLiteral("flac")) {
        return 5;
    }
    if (suffix == QStringLiteral("wav")) {
        return 11;
    }
    if (suffix == QStringLiteral("aif") || suffix == QStringLiteral("aiff")) {
        return 12;
    }
    return 0;
}

int naturalCompare(const QString& left, const QString& right) {
    int i = 0;
    int j = 0;
    const int leftSize = static_cast<int>(left.size());
    const int rightSize = static_cast<int>(right.size());
    while (i < leftSize && j < rightSize) {
        const QChar a = left.at(i);
        const QChar b = right.at(j);
        if (isAsciiDigit(a) && isAsciiDigit(b)) {
            // Whole runs of digits, compared as numbers. Leading zeros do not
            // make a number bigger, so they are skipped -- but one is kept, so
            // that "0" is still a number.
            int endA = i;
            while (endA < leftSize && isAsciiDigit(left.at(endA))) {
                ++endA;
            }
            int endB = j;
            while (endB < rightSize && isAsciiDigit(right.at(endB))) {
                ++endB;
            }
            int startA = i;
            while (startA < endA - 1 && left.at(startA) == QLatin1Char('0')) {
                ++startA;
            }
            int startB = j;
            while (startB < endB - 1 && right.at(startB) == QLatin1Char('0')) {
                ++startB;
            }
            const int lengthA = endA - startA;
            const int lengthB = endB - startB;
            if (lengthA != lengthB) {
                return lengthA < lengthB ? -1 : 1;
            }
            for (int k = 0; k < lengthA; ++k) {
                const QChar digitA = left.at(startA + k);
                const QChar digitB = right.at(startB + k);
                if (digitA != digitB) {
                    return digitA < digitB ? -1 : 1;
                }
            }
            i = endA;
            j = endB;
            continue;
        }
        const QChar foldedA = a.toCaseFolded();
        const QChar foldedB = b.toCaseFolded();
        if (foldedA != foldedB) {
            return foldedA < foldedB ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < leftSize) {
        return 1;
    }
    if (j < rightSize) {
        return -1;
    }
    // Equal but for case or leading zeros. Settled on the raw strings, so two
    // different names never compare equal and the order cannot wobble between
    // two reads of the same stick.
    const int raw = QString::compare(left, right, Qt::CaseSensitive);
    return raw < 0 ? -1 : (raw > 0 ? 1 : 0);
}

FileNameGuess guessFromFileName(const QString& fileName) {
    FileNameGuess guess;
    QString stem = fileName;
    const int dot = stem.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        stem.truncate(dot);
    }
    // `Artist_-_Title`: underscores stand for spaces, but only in a name that
    // has no spaces of its own -- otherwise they are part of the title.
    if (!stem.contains(QLatin1Char(' ')) && stem.contains(QLatin1Char('_'))) {
        stem.replace(QLatin1Char('_'), QLatin1Char(' '));
    }
    stem = stem.simplified();
    if (stem.isEmpty()) {
        guess.title = fileName;
        return guess;
    }

    // A track number: `01 - x`, `01-x`, `1. x`, `07) x`, or two or three
    // digits and a space. A single digit needs a space after its separator, so
    // a title called `2-Step` stays one, and a single digit followed by a
    // space alone is not taken at all, so a band called `3 Doors Down` stays
    // one too.
    static const QRegularExpression kPunctuated(
            QStringLiteral("^(\\d{2,3})\\s*[-.)_]\\s*(\\S.*)$"));
    static const QRegularExpression kSingleDigit(
            QStringLiteral("^(\\d)\\s*[-.)_]\\s+(\\S.*)$"));
    static const QRegularExpression kSpaced(QStringLiteral("^(\\d{2,3})\\s+(\\S.*)$"));
    QString rest = stem;
    for (const QRegularExpression* pPattern : {&kPunctuated, &kSingleDigit, &kSpaced}) {
        const QRegularExpressionMatch match = pPattern->match(stem);
        if (match.hasMatch()) {
            guess.trackNumber = match.captured(1).toInt();
            rest = match.captured(2).trimmed();
            break;
        }
    }

    // The artist is everything before the FIRST spaced dash. DJ files are
    // named `Artist - Title - Mix`, and splitting at the last dash would make
    // the title the mix name.
    static const QRegularExpression kSeparator(QStringLiteral("\\s+[-\u2013\u2014]\\s+"));
    const QRegularExpressionMatch separator = kSeparator.match(rest);
    if (separator.hasMatch()) {
        const QString artist = rest.left(separator.capturedStart()).trimmed();
        const QString title = rest.mid(separator.capturedEnd()).trimmed();
        if (!artist.isEmpty() && !title.isEmpty()) {
            guess.artist = artist;
            guess.title = title;
            return guess;
        }
    }
    guess.title = rest;
    return guess;
}

FolderLibrary buildFolderLibrary(const FolderListing& listing,
        const QString& rootPlaylistName) {
    FolderLibrary library;
    library.contents.ok = true;

    // 1. The tree, from the paths. Every directory in it holds music somewhere
    // below, because it was only ever created on the way to a file.
    Directory root;
    for (int index = 0; index < listing.files.size(); ++index) {
        QString path = listing.files.at(index).relativePath;
        if (!path.startsWith(QLatin1Char('/'))) {
            path.prepend(QLatin1Char('/'));
        }
        const QStringList components = path.mid(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (components.isEmpty()) {
            continue;
        }
        Directory* pDirectory = &root;
        for (int depth = 0; depth < components.size() - 1; ++depth) {
            const QString& name = components.at(depth);
            Directory* pChild = pDirectory->childByName.value(name);
            if (pChild == nullptr) {
                auto child = std::make_unique<Directory>();
                child->name = name;
                child->sortKey = sortFold(name);
                child->relativePath = pDirectory->relativePath + QLatin1Char('/') + name;
                pChild = child.get();
                pDirectory->childByName.insert(name, pChild);
                pDirectory->children.push_back(std::move(child));
            }
            pDirectory = pChild;
        }
        pDirectory->files.push_back(index);
    }
    std::vector<FileSortKey> fileKeys;
    fileKeys.reserve(static_cast<size_t>(listing.files.size()));
    for (const FolderFile& file : listing.files) {
        FileSortKey key;
        key.name = lastComponent(file.relativePath).normalized(QString::NormalizationForm_C);
        const auto dot = key.name.lastIndexOf(QLatin1Char('.'));
        key.stem = sortFold(dot > 0 ? key.name.left(dot) : key.name);
        fileKeys.push_back(key);
    }
    sortTree(&root, fileKeys);

    // 2. Tracks, numbered in the order the tree shows them: a directory's own
    // files, then its subdirectories.
    QHash<int, quint32> trackIdOf;
    QHash<QString, quint32> artworkIds;
    auto& tracks = library.contents.tracks;
    tracks.reserve(listing.files.size());
    const auto addTracks = [&](const Directory& directory, auto& self) -> void {
        for (const int index : directory.files) {
            const FolderFile& file = listing.files.at(index);
            mixxx::rekordbox::PdbTrack track;
            track.id = static_cast<quint32>(tracks.size() + 1);
            QString path = file.relativePath;
            if (!path.startsWith(QLatin1Char('/'))) {
                path.prepend(QLatin1Char('/'));
            }
            track.filePath = path;
            const FileNameGuess guess = guessFromFileName(lastComponent(path));
            track.title = guess.title;
            track.artist = guess.artist;
            track.trackNumber = static_cast<quint32>(std::max(0, guess.trackNumber));
            track.fileSize = static_cast<quint32>(std::clamp<qint64>(
                    file.size, 0, std::numeric_limits<quint32>::max()));
            track.fileType = containerForFileName(path);
            if (file.modified.isValid()) {
                // As rekordbox writes it, so "Date added" sorts the same way
                // for both kinds of stick: newest files first.
                track.dateAdded = file.modified.date().toString(Qt::ISODate);
            }
            const QString cover = listing.covers.value(directory.relativePath);
            if (!cover.isEmpty()) {
                track.artworkPath = cover;
                auto id = artworkIds.constFind(cover);
                if (id == artworkIds.constEnd()) {
                    id = artworkIds.insert(cover, static_cast<quint32>(artworkIds.size() + 1));
                }
                track.artworkId = *id;
            }
            trackIdOf.insert(index, track.id);
            tracks.append(track);
        }
        for (const auto& child : directory.children) {
            self(*child, self);
        }
    };
    addTracks(root, addTracks);
    for (auto it = artworkIds.constBegin(); it != artworkIds.constEnd(); ++it) {
        library.contents.artwork.insert(it.value(), it.key());
    }

    // 3. The nodes. A directory holding only files is a playlist; one with
    // subdirectories is a folder, and the files directly inside it get a
    // playlist of their own, of the same name, listed first.
    quint32 nextNodeId = 1;
    const auto idsOf = [&trackIdOf](const Directory& directory) {
        QList<quint32> ids;
        ids.reserve(static_cast<int>(directory.files.size()));
        for (const int index : directory.files) {
            ids.append(trackIdOf.value(index));
        }
        return ids;
    };
    // Siblings must not share a name: the ingest keys each node by its full
    // path, and a second "House" under the same parent would be refused.
    const auto uniqueName = [](QSet<QString>* pTaken, const QString& name) {
        QString candidate = name;
        for (int n = 2; pTaken->contains(candidate); ++n) {
            candidate = QStringLiteral("%1 (%2)").arg(name).arg(n);
        }
        pTaken->insert(candidate);
        return candidate;
    };
    const auto addNode = [&](quint32 parentId,
                                 const QString& name,
                                 bool isFolder,
                                 quint32 sortOrder,
                                 const QList<quint32>& trackIds) {
        mixxx::rekordbox::PdbPlaylist node;
        node.id = nextNodeId++;
        node.parentId = parentId;
        node.sortOrder = sortOrder;
        node.name = name;
        node.isFolder = isFolder;
        node.trackIds = trackIds;
        library.contents.playlists.insert(node.id, node);
        return node.id;
    };
    const auto addDirectory = [&](const Directory& directory,
                                      quint32 parentId,
                                      quint32 sortOrder,
                                      QSet<QString>* pSiblings,
                                      auto& self) -> void {
        ++library.directoryCount;
        const QString name = uniqueName(pSiblings, directory.name);
        if (directory.children.empty()) {
            addNode(parentId, name, false, sortOrder, idsOf(directory));
            return;
        }
        const quint32 folderId = addNode(parentId, name, true, sortOrder, {});
        QSet<QString> children;
        quint32 order = 0;
        if (!directory.files.empty()) {
            addNode(folderId, uniqueName(&children, directory.name), false, order++, idsOf(directory));
        }
        for (const auto& child : directory.children) {
            self(*child, folderId, order++, &children, self);
        }
    };

    QSet<QString> topLevel;
    quint32 order = 0;
    if (!root.files.empty()) {
        const QString name = rootPlaylistName.trimmed().isEmpty()
                ? QStringLiteral("USB")
                : rootPlaylistName.trimmed();
        addNode(0, uniqueName(&topLevel, name), false, order++, idsOf(root));
    }
    for (const auto& child : root.children) {
        addDirectory(*child, 0, order++, &topLevel, addDirectory);
    }
    return library;
}

} // namespace deck
} // namespace mixxx
