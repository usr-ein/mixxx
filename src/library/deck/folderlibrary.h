#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include "library/rekordbox/rekordboxpdb.h"

namespace mixxx {
namespace deck {

/// A stick with music on it and no rekordbox library, turned into one.
///
/// **The browser does not learn a second data model for this.** A plain stick
/// is described with exactly the struct a parsed `export.pdb` produces, so
/// `writeMedium()`, every query behind the browser and the load path take it
/// unchanged. What a pdb would have called playlists are the stick's
/// directories (docs/plain-usb-plan.md section 4).
///
/// Everything in here is pure: no filesystem, no database, no Qt GUI. The walk
/// that finds the files is `folderscan`, and keeping the two apart is what lets
/// the mapping be tested with nothing plugged in.

/// One audio file found on the stick.
struct FolderFile {
    /// Relative to the medium root, with a leading slash and `/` separators,
    /// **spelled exactly as the filesystem spells it** -- it is what gets
    /// opened, so it is never normalised.
    QString relativePath;
    qint64 size = 0;
    QDateTime modified;
};

/// What the walk found, as the builder wants it.
struct FolderListing {
    QList<FolderFile> files;
    /// Directory (relative, leading slash; empty for the root) -> the cover
    /// image chosen for the tracks directly inside it.
    QHash<QString, QString> covers;
    /// The walk stopped at a cap before seeing everything.
    bool truncated = false;
};

/// The stick as a library.
struct FolderLibrary {
    mixxx::rekordbox::PdbContents contents;
    /// Directories that became a node, folders and playlists alike. What the
    /// source row calls "folders".
    int directoryCount = 0;
};

/// Whether a file is one the deck would try to play, by its name alone.
///
/// Hidden names are refused here, and that matters more than it looks: a stick
/// written by a Mac carries an AppleDouble `._name.mp3` beside every
/// `name.mp3`, which has the right extension and is not audio.
bool isMusicFileName(const QString& fileName);

/// Whether a directory is never walked into: hidden ones, operating-system
/// clutter, and other DJ software's metadata. `PIONEER` is among them -- a CDJ
/// writes its settings there even on a stick with no rekordbox library.
bool isSkippedDirectoryName(const QString& name);

/// How good a cover a file name is: 0 is the best, -1 is not a cover at all.
int coverFileRank(const QString& fileName);

/// The container byte rekordbox would store for this file, or 0 when it has
/// none (OGG, Opus).
quint32 containerForFileName(const QString& fileName);

/// Natural, case-insensitive order: `Track 2` before `Track 10`.
///
/// Total and deterministic: two different strings never compare equal, so
/// the same stick always lists the same way.
int naturalCompare(const QString& left, const QString& right);

/// What a file name says about a track whose tags have not been read yet.
struct FileNameGuess {
    int trackNumber = 0;
    QString artist;
    QString title;
};

/// Read a track number, an artist and a title off a file name.
///
/// `01 - Artist - Title.mp3` gives 1, `Artist`, `Title`. The artist is split
/// off at the **first** ` - `, so `Underworld - Bruce Lee - Ricks Mix` keeps
/// the mix name in the title, which is how DJ files are named. A track number
/// needs two digits or a separator after it, so `3 Doors Down - Kryptonite`
/// stays a band.
FileNameGuess guessFromFileName(const QString& fileName);

/// Turn what the walk found into a library (docs/plain-usb-plan.md 4.2).
///
/// *rootPlaylistName* names the playlist that holds the files at the very top
/// of the stick: the volume label, so it reads as "the stick itself".
FolderLibrary buildFolderLibrary(const FolderListing& listing,
        const QString& rootPlaylistName);

} // namespace deck
} // namespace mixxx
