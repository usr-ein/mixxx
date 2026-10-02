#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

#include "library/deck/folderlibrary.h"
#include "library/deck/pdbingest.h"

namespace mixxx {
namespace deck {

/// The disk half of reading a stick with no rekordbox library
/// (docs/plain-usb-plan.md, L5). `folderlibrary` is the pure half.
///
/// Both functions block on the stick and belong on a worker thread. Neither
/// writes anything: the stick is mounted read-only, and the database is the
/// caller's business.

/// How far a walk goes before it gives up and shows what it has.
struct FolderScanLimits {
    int maxFiles = 20000;
    int maxDepth = 16;
};

/// Pass A: every music file on the stick, and a cover per directory.
///
/// Seconds even for a large stick: one directory listing per directory and the
/// sizes and dates that come with it, and no file is opened. That is what makes
/// the stick browsable this quickly -- the tags, which do need every file
/// opened, come after, in pass B.
///
/// Symbolic links are not followed, so a link back up the tree cannot loop.
FolderListing walkFolderMedium(const QString& root, const FolderScanLimits& limits = {});

/// One file whose tags pass B should read.
struct FolderTagTarget {
    quint32 rbId = 0;
    /// Absolute.
    QString path;
};

/// Pass B: the tags of a batch of files, through the same importer the rest of
/// Mixxx uses. A file whose tags cannot be read is left out, not reported: it
/// keeps what its name said.
///
/// No cover is decoded -- that would be the expensive part, and a row's cover
/// comes from the folder image or, once loaded, from Mixxx's own import.
QList<TrackTagUpdate> readFolderTags(const QList<FolderTagTarget>& targets);

} // namespace deck
} // namespace mixxx
