#include "library/deck/folderlibrary.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QStringList>

namespace {

using mixxx::deck::buildFolderLibrary;
using mixxx::deck::FolderFile;
using mixxx::deck::FolderLibrary;
using mixxx::deck::FolderListing;
using mixxx::prolink::PdbPlaylist;
using mixxx::prolink::PdbTrack;

FolderListing listingOf(const QStringList& paths) {
    FolderListing listing;
    for (const QString& path : paths) {
        FolderFile file;
        file.relativePath = path;
        file.size = 1000;
        file.modified = QDateTime(QDate(2026, 9, 30), QTime(12, 0));
        listing.files.append(file);
    }
    return listing;
}

/// The nodes directly under *parentId*, in the order the browser lists them:
/// folders first, then by sort order -- the ORDER BY in deckqueries.cpp.
QList<PdbPlaylist> childrenOf(const FolderLibrary& library, quint32 parentId) {
    QList<PdbPlaylist> children;
    for (const PdbPlaylist& node : library.contents.playlists) {
        if (node.parentId == parentId) {
            children.append(node);
        }
    }
    std::sort(children.begin(), children.end(), [](const PdbPlaylist& a, const PdbPlaylist& b) {
        if (a.isFolder != b.isFolder) {
            return a.isFolder;
        }
        return a.sortOrder < b.sortOrder;
    });
    return children;
}

QStringList namesOf(const QList<PdbPlaylist>& nodes) {
    QStringList names;
    for (const PdbPlaylist& node : nodes) {
        names.append(node.name);
    }
    return names;
}

QStringList pathsOf(const FolderLibrary& library, const QList<quint32>& trackIds) {
    QStringList paths;
    for (const quint32 id : trackIds) {
        for (const PdbTrack& track : library.contents.tracks) {
            if (track.id == id) {
                paths.append(track.filePath);
            }
        }
    }
    return paths;
}

TEST(FolderLibraryTest, TheDocumentedExampleMapsAsDocumented) {
    // docs/plain-usb-plan.md 4.2, verbatim.
    const FolderLibrary library = buildFolderLibrary(listingOf({
                                                             "/a.mp3",
                                                             "/b.mp3",
                                                             "/House/c.mp3",
                                                             "/House/Deep/d.mp3",
                                                             "/Techno/e.mp3",
                                                     }),
            "KINGSTON");

    ASSERT_TRUE(library.contents.ok);
    EXPECT_EQ(5, library.contents.tracks.size());
    // House, House/Deep and Techno. The root is the stick, not a folder on it.
    EXPECT_EQ(3, library.directoryCount);

    const QList<PdbPlaylist> top = childrenOf(library, 0);
    ASSERT_EQ(3, top.size());
    EXPECT_EQ(QStringList({"House", "KINGSTON", "Techno"}), namesOf(top));
    EXPECT_TRUE(top.at(0).isFolder);
    EXPECT_FALSE(top.at(1).isFolder);
    EXPECT_FALSE(top.at(2).isFolder);
    EXPECT_EQ(QStringList({"/a.mp3", "/b.mp3"}), pathsOf(library, top.at(1).trackIds));
    EXPECT_EQ(QStringList({"/Techno/e.mp3"}), pathsOf(library, top.at(2).trackIds));

    const QList<PdbPlaylist> house = childrenOf(library, top.at(0).id);
    ASSERT_EQ(2, house.size());
    // The folder's own files first, under the folder's own name.
    EXPECT_EQ(QStringList({"House", "Deep"}), namesOf(house));
    EXPECT_EQ(QStringList({"/House/c.mp3"}), pathsOf(library, house.at(0).trackIds));
    EXPECT_EQ(QStringList({"/House/Deep/d.mp3"}), pathsOf(library, house.at(1).trackIds));
}

TEST(FolderLibraryTest, TrackIdsFollowTheTreeAndAreStable) {
    const QStringList paths = {"/Z/z.mp3", "/b.mp3", "/A/x.mp3", "/a.mp3", "/A/B/y.mp3"};
    const FolderLibrary first = buildFolderLibrary(listingOf(paths), "USB");
    // The same files in another order -- a directory listing is not ordered.
    const FolderLibrary second = buildFolderLibrary(
            listingOf({"/A/B/y.mp3", "/a.mp3", "/Z/z.mp3", "/A/x.mp3", "/b.mp3"}), "USB");

    QStringList order;
    for (const PdbTrack& track : first.contents.tracks) {
        order.append(track.filePath);
    }
    // Root files, then A's own, then A/B, then Z: the order the tree shows.
    EXPECT_EQ(QStringList({"/a.mp3", "/b.mp3", "/A/x.mp3", "/A/B/y.mp3", "/Z/z.mp3"}), order);
    for (int i = 0; i < first.contents.tracks.size(); ++i) {
        EXPECT_EQ(static_cast<quint32>(i + 1), first.contents.tracks.at(i).id);
        EXPECT_EQ(first.contents.tracks.at(i).filePath, second.contents.tracks.at(i).filePath);
    }
}

TEST(FolderLibraryTest, NaturalOrderPutsTwoBeforeTen) {
    const FolderLibrary library = buildFolderLibrary(
            listingOf({"/Track 10.mp3", "/track 2.mp3", "/Track 1.mp3"}), "USB");
    QStringList order;
    for (const PdbTrack& track : library.contents.tracks) {
        order.append(track.filePath);
    }
    EXPECT_EQ(QStringList({"/Track 1.mp3", "/track 2.mp3", "/Track 10.mp3"}), order);
}

TEST(FolderLibraryTest, SiblingsNeverShareAName) {
    // A folder whose own files and whose subdirectory are both called House.
    const FolderLibrary library = buildFolderLibrary(
            listingOf({"/House/one.mp3", "/House/House/two.mp3"}), "USB");
    const QList<PdbPlaylist> top = childrenOf(library, 0);
    ASSERT_EQ(1, top.size());
    const QList<PdbPlaylist> inside = childrenOf(library, top.at(0).id);
    ASSERT_EQ(2, inside.size());
    EXPECT_NE(inside.at(0).name, inside.at(1).name);
}

TEST(FolderLibraryTest, TheRootPlaylistIsNamedForTheStick) {
    const FolderLibrary unlabelled = buildFolderLibrary(listingOf({"/a.mp3"}), "  ");
    ASSERT_EQ(1, unlabelled.contents.playlists.size());
    EXPECT_EQ(QStringLiteral("USB"), unlabelled.contents.playlists.first().name);
}

TEST(FolderLibraryTest, AnEmptyListingIsAnEmptyLibrary) {
    const FolderLibrary library = buildFolderLibrary(FolderListing(), "USB");
    EXPECT_TRUE(library.contents.ok);
    EXPECT_TRUE(library.contents.tracks.isEmpty());
    EXPECT_TRUE(library.contents.playlists.isEmpty());
}

TEST(FolderLibraryTest, CoversBelongToTheirOwnDirectoryOnly) {
    FolderListing listing = listingOf({"/Album/a.mp3", "/Album/Bonus/b.mp3"});
    listing.covers.insert(QStringLiteral("/Album"), QStringLiteral("/Album/cover.jpg"));
    const FolderLibrary library = buildFolderLibrary(listing, "USB");
    ASSERT_EQ(2, library.contents.tracks.size());
    EXPECT_EQ(QStringLiteral("/Album/cover.jpg"), library.contents.tracks.at(0).artworkPath);
    EXPECT_NE(0u, library.contents.tracks.at(0).artworkId);
    EXPECT_TRUE(library.contents.tracks.at(1).artworkPath.isEmpty());
}

TEST(FolderLibraryTest, TrackFieldsComeFromTheFile) {
    FolderListing listing = listingOf({"/Set/01 - Underworld - Bruce Lee - Ricks Mix.MP3"});
    listing.files[0].size = 12345;
    const FolderLibrary library = buildFolderLibrary(listing, "USB");
    ASSERT_EQ(1, library.contents.tracks.size());
    const PdbTrack& track = library.contents.tracks.first();
    EXPECT_EQ(QStringLiteral("Underworld"), track.artist);
    EXPECT_EQ(QStringLiteral("Bruce Lee - Ricks Mix"), track.title);
    EXPECT_EQ(1u, track.trackNumber);
    EXPECT_EQ(12345u, track.fileSize);
    EXPECT_EQ(1u, track.fileType); // MP3, whatever the case of the suffix
    EXPECT_EQ(QStringLiteral("2026-09-30"), track.dateAdded);
}

TEST(FolderLibraryTest, NamesFromNfdAndNfcSortTogether) {
    // "é" composed and decomposed. A stick written by a Mac stores the second.
    const QString composed = QString::fromUtf8("/\xc3\xa9t\xc3\xa9.mp3");
    const QString decomposed = QString::fromUtf8("/e\xcc\x81te\xcc\x81 2.mp3");
    const FolderLibrary library = buildFolderLibrary(
            listingOf({decomposed, "/f.mp3", composed}), "USB");
    QStringList order;
    for (const PdbTrack& track : library.contents.tracks) {
        order.append(track.filePath);
    }
    // Both spellings land together, before "f", and keep the bytes they had.
    EXPECT_EQ(QStringList({composed, decomposed, QStringLiteral("/f.mp3")}), order);
}

TEST(FolderFileNamesTest, MusicIsRecognisedAndAppleDoubleIsNot) {
    EXPECT_TRUE(mixxx::deck::isMusicFileName(QStringLiteral("a.mp3")));
    EXPECT_TRUE(mixxx::deck::isMusicFileName(QStringLiteral("a.AIFF")));
    EXPECT_TRUE(mixxx::deck::isMusicFileName(QStringLiteral("a.m4a")));
    EXPECT_FALSE(mixxx::deck::isMusicFileName(QStringLiteral("._a.mp3")));
    EXPECT_FALSE(mixxx::deck::isMusicFileName(QStringLiteral("readme.txt")));
    EXPECT_FALSE(mixxx::deck::isMusicFileName(QStringLiteral("mp3")));
}

TEST(FolderFileNamesTest, ClutterDirectoriesAreSkipped) {
    EXPECT_TRUE(mixxx::deck::isSkippedDirectoryName(QStringLiteral(".Spotlight-V100")));
    EXPECT_TRUE(mixxx::deck::isSkippedDirectoryName(QStringLiteral("$Recycle.Bin")));
    EXPECT_TRUE(mixxx::deck::isSkippedDirectoryName(QStringLiteral("PIONEER")));
    EXPECT_TRUE(mixxx::deck::isSkippedDirectoryName(QStringLiteral("System Volume Information")));
    EXPECT_FALSE(mixxx::deck::isSkippedDirectoryName(QStringLiteral("Contents")));
    EXPECT_FALSE(mixxx::deck::isSkippedDirectoryName(QStringLiteral("House")));
}

TEST(FolderFileNamesTest, CoversAreRankedByName) {
    EXPECT_EQ(0, mixxx::deck::coverFileRank(QStringLiteral("Cover.JPG")));
    EXPECT_EQ(1, mixxx::deck::coverFileRank(QStringLiteral("folder.png")));
    EXPECT_EQ(-1, mixxx::deck::coverFileRank(QStringLiteral("cover.gif")));
    EXPECT_EQ(-1, mixxx::deck::coverFileRank(QStringLiteral("._cover.jpg")));
    EXPECT_EQ(-1, mixxx::deck::coverFileRank(QStringLiteral("scan.jpg")));
}

TEST(FolderFileNamesTest, GuessesFromFileNames) {
    using mixxx::deck::guessFromFileName;

    auto guess = guessFromFileName(QStringLiteral("Underworld - Born Slippy (Nuxx).mp3"));
    EXPECT_EQ(QStringLiteral("Underworld"), guess.artist);
    EXPECT_EQ(QStringLiteral("Born Slippy (Nuxx)"), guess.title);
    EXPECT_EQ(0, guess.trackNumber);

    guess = guessFromFileName(QStringLiteral("07. Title Only.mp3"));
    EXPECT_EQ(7, guess.trackNumber);
    EXPECT_TRUE(guess.artist.isEmpty());
    EXPECT_EQ(QStringLiteral("Title Only"), guess.title);

    guess = guessFromFileName(QStringLiteral("01 Root Track.mp3"));
    EXPECT_EQ(1, guess.trackNumber);
    EXPECT_EQ(QStringLiteral("Root Track"), guess.title);

    // Numbers that are part of a name stay part of it.
    guess = guessFromFileName(QStringLiteral("3 Doors Down - Kryptonite.mp3"));
    EXPECT_EQ(0, guess.trackNumber);
    EXPECT_EQ(QStringLiteral("3 Doors Down"), guess.artist);
    guess = guessFromFileName(QStringLiteral("2-Step Garage.mp3"));
    EXPECT_EQ(0, guess.trackNumber);
    EXPECT_EQ(QStringLiteral("2-Step Garage"), guess.title);
    guess = guessFromFileName(QStringLiteral("999999999 - Rave 4 love.mp3"));
    EXPECT_EQ(0, guess.trackNumber);
    EXPECT_EQ(QStringLiteral("999999999"), guess.artist);
    EXPECT_EQ(QStringLiteral("Rave 4 love"), guess.title);

    // Underscores for spaces, but only when there are no spaces.
    guess = guessFromFileName(QStringLiteral("Artist_-_Some_Title.mp3"));
    EXPECT_EQ(QStringLiteral("Artist"), guess.artist);
    EXPECT_EQ(QStringLiteral("Some Title"), guess.title);

    // A hyphen inside a word is not a separator.
    guess = guessFromFileName(QStringLiteral("Jay-Z.mp3"));
    EXPECT_TRUE(guess.artist.isEmpty());
    EXPECT_EQ(QStringLiteral("Jay-Z"), guess.title);
}

TEST(FolderFileNamesTest, NaturalCompareIsTotal) {
    using mixxx::deck::naturalCompare;
    EXPECT_LT(naturalCompare(QStringLiteral("a2"), QStringLiteral("a10")), 0);
    EXPECT_GT(naturalCompare(QStringLiteral("a10"), QStringLiteral("a2")), 0);
    EXPECT_LT(naturalCompare(QStringLiteral("abc"), QStringLiteral("ABD")), 0);
    // Equal but for case still orders, so no two names tie.
    EXPECT_NE(0, naturalCompare(QStringLiteral("abc"), QStringLiteral("ABC")));
    EXPECT_NE(0, naturalCompare(QStringLiteral("a01"), QStringLiteral("a1")));
    EXPECT_EQ(0, naturalCompare(QStringLiteral("same"), QStringLiteral("same")));
}

} // namespace
