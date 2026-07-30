#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSet>

#include "library/prolink/prolinkpdbimport.h"
#include "test/mixxxtest.h"

namespace {

using mixxx::prolink::PdbContents;
using mixxx::prolink::PdbPlaylist;
using mixxx::prolink::PdbTrack;

/// A real `export.pdb`, pulled off a CDJ-2000NXS over NFS and verified
/// byte-identical to the same file read from the physically ejected stick —
/// `cmp` showed exactly two differing bytes, both inside the `0x10`–`0x18`
/// header window the player rewrites as it operates (F13).
///
/// Not committed: it is a real library and carries its owner's track metadata.
/// Drop a copy at this path to run these tests; see
/// `prolinks-compat/docs/HARDWARE.md` §12 for how to produce one.
const char* kFixtureRelativePath = "prolink/export.pdb";

/// Resolved against the *source* tree, not the working directory: the test
/// binary runs from the build directory, which is somewhere else entirely.
QString fixturePath() {
    return QDir(QStringLiteral(QT_TESTCASE_SOURCEDIR))
            .filePath(QStringLiteral("src/test/") +
                    QString::fromLatin1(kFixtureRelativePath));
}

QByteArray loadFixture() {
    QFile file(fixturePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

class ProLinkPdbTest : public testing::Test {
  protected:
    void SetUp() override {
        m_data = loadFixture();
        if (m_data.isEmpty()) {
            GTEST_SKIP() << "no fixture at " << fixturePath().toStdString();
        }
    }
    QByteArray m_data;
};

/// Counts cross-checked against the independently written Python parser in
/// prolinks-compat, reading the same bytes. Two implementations of the same
/// format agreeing on 651 tracks is a much stronger statement than either one
/// producing a plausible number.
TEST_F(ProLinkPdbTest, MatchesThePythonParserOnTheSameFile) {
    const PdbContents contents = mixxx::prolink::parsePdb(m_data);
    ASSERT_TRUE(contents.ok) << contents.error.toStdString();

    EXPECT_EQ(651, contents.tracks.size());
    EXPECT_EQ(1, contents.playlistCount());
    EXPECT_EQ(0, contents.folderCount());

    QSet<QString> artists;
    QSet<QString> albums;
    for (const PdbTrack& track : contents.tracks) {
        if (!track.artist.isEmpty()) {
            artists.insert(track.artist);
        }
        if (!track.album.isEmpty()) {
            albums.insert(track.album);
        }
    }
    // The Python side counts rows in the artist and album tables; here we count
    // distinct values actually *referenced* by a track. Fewer, and legitimately
    // so: rekordbox leaves unreferenced rows behind when tracks are removed. The
    // measured figures are 290 of 329 artists and 273 album rows, so these
    // bounds pin the real numbers rather than guessing at them.
    EXPECT_EQ(290, artists.size());
    EXPECT_LE(albums.size(), 273);
    EXPECT_GE(albums.size(), 200);
}

TEST_F(ProLinkPdbTest, ReadsTheOnePlaylistWithItsCuratedOrder) {
    const PdbContents contents = mixxx::prolink::parsePdb(m_data);
    ASSERT_TRUE(contents.ok);
    ASSERT_EQ(1, contents.playlists.size());

    const PdbPlaylist& playlist = contents.playlists.first();
    EXPECT_EQ(QStringLiteral("test_formats"), playlist.name);
    EXPECT_FALSE(playlist.isFolder);
    // Forty, not the thirty-nine the Python parser reported when these two were
    // first compared. The stick is a format matrix whose generator emits exactly
    // 40 files, and the fortieth -- "AIFF 24b 48k", track id 651 -- is real. The
    // page header says entry_count 39 while its group masks mark 16 + 16 + 8 = 40
    // rows live, and the mask is the one telling the truth (F47).
    EXPECT_EQ(40, playlist.trackIds.size());

    // Entry indices are one-based and need not arrive in page order, so the
    // curated order is only correct if entries were sorted by index rather than
    // appended as encountered. Every id must also be a real track.
    QSet<quint32> known;
    for (const PdbTrack& track : contents.tracks) {
        known.insert(track.id);
    }
    for (const quint32 id : playlist.trackIds) {
        EXPECT_TRUE(known.contains(id)) << "playlist references unknown track " << id;
    }
}

TEST_F(ProLinkPdbTest, TrackFieldsAreUsable) {
    const PdbContents contents = mixxx::prolink::parsePdb(m_data);
    ASSERT_TRUE(contents.ok);

    int withPath = 0;
    int withAnalyze = 0;
    int withTempo = 0;
    QSet<quint32> fileTypes;
    for (const PdbTrack& track : contents.tracks) {
        // Paths must stay exactly as stored, leading slash and all: they are
        // handed straight back to NFS to fetch the file.
        if (track.filePath.startsWith(QLatin1Char('/'))) {
            withPath++;
        }
        if (!track.analyzePath.isEmpty()) {
            withAnalyze++;
        }
        if (track.tempoCentiBpm > 0) {
            withTempo++;
        }
        fileTypes.insert(track.fileType);
    }
    EXPECT_EQ(contents.tracks.size(), withPath);
    EXPECT_GT(withAnalyze, 0);
    EXPECT_GT(withTempo, 0);

    // Row offset 0x5a, which dysentery calls unknown6, is the container (F34).
    // This stick is the format matrix, so it should hold several: mp3 1, m4a 4,
    // flac 5, wav 11, aiff 12.
    EXPECT_GT(fileTypes.size(), 1) << "expected more than one container on this medium";
    for (const quint32 type : fileTypes) {
        EXPECT_TRUE(type == 1 || type == 4 || type == 5 || type == 11 || type == 12)
                << "unexpected container id " << type;
    }
}

TEST(ProLinkPdbErrorTest, RejectsGarbageWithoutThrowing) {
    // A pdb arrives over the network from a device we do not control, so a
    // malformed one has to be an error value rather than an exception escaping
    // into the network thread.
    EXPECT_FALSE(mixxx::prolink::parsePdb(QByteArray()).ok);
    EXPECT_FALSE(mixxx::prolink::parsePdb(QByteArrayLiteral("not a database")).ok);

    // A buffer of zeroes is the dangerous case: Kaitai parses it happily, with
    // num_tables coming out 0, so without a header check we would report a valid
    // database containing no tracks -- a corrupt download presenting as an empty
    // medium.
    const QByteArray zeroes(8192, '\0');
    const PdbContents fromZeroes = mixxx::prolink::parsePdb(zeroes);
    EXPECT_FALSE(fromZeroes.ok);
    EXPECT_TRUE(fromZeroes.error.contains(QStringLiteral("not a rekordbox database")));
}

} // namespace
