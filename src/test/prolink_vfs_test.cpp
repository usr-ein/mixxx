#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "network/prolink/server/prolinkvfs.h"

namespace {

using mixxx::prolink::server::ProLinkVfs;

/// Write *contents* to *relative* under *root*, creating directories.
void writeFile(const QString& root, const QString& relative, const QByteArray& contents) {
    const QString path = root + QLatin1Char('/') + relative;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(contents.size(), file.write(contents));
}

class ProLinkVfsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        writeFile(m_dir.path(),
                QStringLiteral("PIONEER/rekordbox/export.pdb"),
                QByteArrayLiteral("a rekordbox database"));
        writeFile(m_dir.path(),
                QStringLiteral("Contents/GESAFFELSTEIN/Track.mp3"),
                QByteArrayLiteral("0123456789"));
        m_vfs.mount(QStringLiteral("C"), m_dir.path());
    }

    /// Walk a slash-separated path from the export root, as a deck does: one
    /// LOOKUP per component.
    bool walk(const QString& path, QByteArray* pHandle, ProLinkVfs::Node* pNode) {
        QByteArray handle = ProLinkVfs::handleFor(QStringLiteral("/C"));
        ProLinkVfs::Node node;
        for (const QString& component : path.split(QLatin1Char('/'))) {
            if (!m_vfs.lookup(handle, component, &handle, &node)) {
                return false;
            }
        }
        if (pHandle) {
            *pHandle = handle;
        }
        if (pNode) {
            *pNode = node;
        }
        return true;
    }

    QTemporaryDir m_dir;
    ProLinkVfs m_vfs;
};

TEST_F(ProLinkVfsTest, WalksToAFileAndReadsIt) {
    QByteArray handle;
    ProLinkVfs::Node node;
    ASSERT_TRUE(walk(QStringLiteral("PIONEER/rekordbox/export.pdb"), &handle, &node));
    EXPECT_FALSE(node.isDirectory);
    EXPECT_EQ(20, node.size);
    EXPECT_EQ(QByteArrayLiteral("a rekordbox database"), m_vfs.read(handle, 0, 8192));
    // Byte ranges, which is how a real transfer arrives: 1280 at a time.
    EXPECT_EQ(QByteArrayLiteral("rekordbox"), m_vfs.read(handle, 2, 9));
}

TEST_F(ProLinkVfsTest, OnlyTheFirstTwelveBytesOfAHandleAreConsulted) {
    // **A CDJ does not treat the filehandle as opaque.** Handed one, it returns
    // a handle whose first 12 bytes match ours and whose remaining 20 it has
    // overwritten with its own file reference (F28). Matching on all 32 answers
    // NFSERR_STALE to a handle we issued a millisecond earlier.
    QByteArray handle;
    ASSERT_TRUE(walk(QStringLiteral("PIONEER/rekordbox/export.pdb"), &handle, nullptr));
    ASSERT_EQ(32, handle.size());

    QByteArray rewritten = handle.left(12) + QByteArray(20, '\x5a');
    ProLinkVfs::Node node;
    EXPECT_TRUE(m_vfs.resolve(rewritten, &node));
    EXPECT_EQ(QByteArrayLiteral("a rekordbox database"), m_vfs.read(rewritten, 0, 8192));

    // A handle whose *leading* bytes differ is genuinely unknown, and must not
    // resolve -- otherwise a stale handle would silently read the wrong file.
    QByteArray foreign = handle;
    foreign[0] = static_cast<char>(handle.at(0) ^ 0xFF);
    EXPECT_FALSE(m_vfs.resolve(foreign, nullptr));
}

TEST_F(ProLinkVfsTest, LookupFallsBackToACaseFold) {
    // A rekordbox medium is FAT32 -- case-insensitive and case-preserving -- and
    // export.pdb does not necessarily record a name with the case the directory
    // entry uses. On a real stick the database says `Gesaffelstein` where the
    // directory is `GESAFFELSTEIN`. A server doing exact byte comparison answers
    // NFSERR_NOENT and the track will not load.
    QByteArray handle;
    ProLinkVfs::Node node;
    ASSERT_TRUE(walk(QStringLiteral("Contents/Gesaffelstein/track.MP3"), &handle, &node));
    EXPECT_EQ(QByteArrayLiteral("0123456789"), m_vfs.read(handle, 0, 8192));

    // And the handle it hands back is the one for the name **as stored**, so it
    // resolves again later. Hashing the name as *asked* would mint a handle
    // that is not in the table, and every later use of it would be STALE.
    EXPECT_EQ(ProLinkVfs::handleFor(QStringLiteral("/C/Contents/GESAFFELSTEIN/Track.mp3")),
            handle);
}

TEST_F(ProLinkVfsTest, RefusesToWalkOutOfTheExport) {
    const QByteArray root = ProLinkVfs::handleFor(QStringLiteral("/C"));
    for (const QString& name : {QStringLiteral(".."),
                 QStringLiteral("."),
                 QStringLiteral("../../etc"),
                 QStringLiteral("/etc/passwd"),
                 QString()}) {
        EXPECT_FALSE(m_vfs.lookup(root, name, nullptr, nullptr))
                << "walked out via " << name.toStdString();
    }
}

TEST_F(ProLinkVfsTest, AMissingNameIsNotAStaleHandle) {
    // The two are different answers and a deck acts on them differently: STALE
    // means re-mount, NOENT means the file is gone. The caller tells them apart
    // by resolving the parent, so the parent must still resolve after a miss.
    const QByteArray root = ProLinkVfs::handleFor(QStringLiteral("/C"));
    EXPECT_FALSE(m_vfs.lookup(root, QStringLiteral("nothing-here"), nullptr, nullptr));
    EXPECT_TRUE(m_vfs.resolve(root, nullptr));
}

TEST_F(ProLinkVfsTest, TwoMediaGetDistinctHandlesForTheSamePath) {
    // Sharing a root would mint the same handle for the same relative path --
    // most obviously the root itself -- and since a deck keeps only the leading
    // 12 bytes there would be nothing left to tell the media apart afterwards.
    QTemporaryDir second;
    ASSERT_TRUE(second.isValid());
    writeFile(second.path(),
            QStringLiteral("PIONEER/rekordbox/export.pdb"),
            QByteArrayLiteral("a different database"));
    m_vfs.mount(QStringLiteral("B"), second.path());

    EXPECT_NE(ProLinkVfs::handleFor(QStringLiteral("/C")),
            ProLinkVfs::handleFor(QStringLiteral("/B")));

    QByteArray usb;
    ASSERT_TRUE(walk(QStringLiteral("PIONEER/rekordbox/export.pdb"), &usb, nullptr));
    QByteArray sd = ProLinkVfs::handleFor(QStringLiteral("/B"));
    ProLinkVfs::Node node;
    for (const QString& component : {QStringLiteral("PIONEER"),
                 QStringLiteral("rekordbox"),
                 QStringLiteral("export.pdb")}) {
        ASSERT_TRUE(m_vfs.lookup(sd, component, &sd, &node));
    }
    EXPECT_NE(usb.left(ProLinkVfs::kHandlePrefix), sd.left(ProLinkVfs::kHandlePrefix));
    EXPECT_EQ(QByteArrayLiteral("a different database"), m_vfs.read(sd, 0, 8192));
}

TEST_F(ProLinkVfsTest, UnmountingInvalidatesEveryHandleBeneathIt) {
    // Which is exactly right when a stick is yanked: a deck still holding a
    // handle into it should be told STALE rather than served whatever now
    // happens to hash to the same path.
    QByteArray handle;
    ASSERT_TRUE(walk(QStringLiteral("PIONEER/rekordbox/export.pdb"), &handle, nullptr));
    ASSERT_TRUE(m_vfs.resolve(handle, nullptr));

    m_vfs.unmount(QStringLiteral("C"));
    EXPECT_FALSE(m_vfs.isMounted(QStringLiteral("C")));
    EXPECT_FALSE(m_vfs.resolve(handle, nullptr));
    EXPECT_FALSE(m_vfs.resolve(ProLinkVfs::handleFor(QStringLiteral("/C")), nullptr));
    EXPECT_TRUE(m_vfs.read(handle, 0, 8192).isEmpty());
}

TEST_F(ProLinkVfsTest, ReadDirectoryListsDirectChildrenOnly) {
    const QByteArray root = ProLinkVfs::handleFor(QStringLiteral("/C"));
    const QList<ProLinkVfs::Node> children = m_vfs.readDirectory(root);
    QStringList names;
    for (const ProLinkVfs::Node& child : children) {
        names.append(child.path);
    }
    names.sort();
    EXPECT_EQ(QStringList({QStringLiteral("/C/Contents"), QStringLiteral("/C/PIONEER")}),
            names);
}

} // namespace
