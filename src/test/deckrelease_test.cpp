#include "widget/deck/deckrelease.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "widget/deck/wdeckdiagnostics.h"

using mixxx::deck::DeckRelease;
using mixxx::deck::WDeckDiagnostics;
using Blocked = DeckRelease::Blocked;
using Kind = DeckRelease::Verdict::Kind;

namespace {

// ---------------------------------------------------------------------------
// The files as they are on a release card. The system.conf is
// pi_config/rauc/system.conf, the rest as the emulated deck had them, release
// 0.0.1 freshly flashed and then shipped into B.
// ---------------------------------------------------------------------------

const QString kSystemConf = QStringLiteral(
        "# RAUC on a TriMixxx release card\n"
        "[system]\n"
        "compatible=trimixxx-pi4\n"
        "bootloader=custom\n"
        "data-directory=/var/lib/rauc\n"
        "bundle-formats=plain\n"
        "\n"
        "[keyring]\n"
        "path=/etc/rauc/keyring.pem\n"
        "\n"
        "[handlers]\n"
        "bootloader-custom-backend=/usr/lib/rauc/rpi-tryboot\n"
        "\n"
        "[slot.rootfs.0]\n"
        "device=/dev/disk/by-partuuid/5d0bc1ec-05\n"
        "type=raw\n"
        "bootname=A\n"
        "\n"
        "[slot.boot.0]\n"
        "device=/dev/disk/by-partuuid/5d0bc1ec-02\n"
        "type=vfat\n"
        "parent=rootfs.0\n"
        "\n"
        "[slot.rootfs.1]\n"
        "device=/dev/disk/by-partuuid/5d0bc1ec-06\n"
        "type=raw\n"
        "bootname=B\n"
        "\n"
        "[slot.boot.1]\n"
        "device=/dev/disk/by-partuuid/5d0bc1ec-03\n"
        "type=vfat\n"
        "parent=rootfs.1\n");

// RAUC's central status file once `pi-qemu deck ship` had installed 0.0.1
// into B.
const QString kInstalledInB = QStringLiteral(
        "[slot.rootfs.1]\n"
        "bundle.compatible=trimixxx-pi4\n"
        "bundle.version=0.0.1\n"
        "bundle.build=pi/v0.0.1\n"
        "bundle.hash=3354949e79604000033b02b8e677d4acd5353f1e7ad135f15eb40a9e778dcdfe\n"
        "status=ok\n"
        "sha256=1c0c9a1388817fe91fe9ceb0459d96eabb54707e25a6b0e4c2d543e03e3ade1f\n"
        "size=1044127744\n"
        "installed.transaction=db90bf3a-5ea3-40f3-af01-c02dbb7c6e5b\n"
        "installed.timestamp=2026-10-09T09:43:09Z\n"
        "installed.count=1\n"
        "activated.timestamp=2026-10-09T09:43:10Z\n"
        "activated.count=1\n"
        "\n"
        "[slot.boot.1]\n"
        "bundle.compatible=trimixxx-pi4\n"
        "bundle.version=0.0.1\n"
        "bundle.build=pi/v0.0.1\n"
        "status=ok\n"
        "installed.timestamp=2026-10-09T09:43:10Z\n"
        "installed.count=1\n"
        "\n"
        "[system]\n"
        "boot-id=4cd2d0dc-dd69-4cde-b3b1-1445a0a6d6e8\n");

// rpi-tryboot's autoboot_for SLOT.
QString autobootFor(int committed, int trial) {
    return QStringLiteral("[all]\ntryboot_a_b=1\nboot_partition=%1\n[tryboot]\nboot_partition=%2\n")
            .arg(committed)
            .arg(trial);
}

// ---------------------------------------------------------------------------
// The release file.
// ---------------------------------------------------------------------------

TEST(DeckReleaseTest, ReleaseFileWithHashAndDate) {
    const DeckRelease::Release release = DeckRelease::parseRelease(QStringLiteral(
            "VERSION=0.0.9\n"
            "COMMIT=pi/v0.0.1-5-g52e3260-dirty (rehearsal)\n"
            "HASH=52e3260f00c0ffee52e3260f00c0ffee52e3260f\n"
            "BUILT=2026-10-09T10:00Z\n"));
    EXPECT_EQ(QStringLiteral("0.0.9"), release.version);
    EXPECT_EQ(QStringLiteral("pi/v0.0.1-5-g52e3260-dirty (rehearsal)"), release.commit);
    EXPECT_EQ(QStringLiteral("52e3260f00c0ffee52e3260f00c0ffee52e3260f"), release.hash);
    EXPECT_EQ(QStringLiteral("2026-10-09T10:00Z"), release.built);
}

TEST(DeckReleaseTest, ReleaseFileOfAnOlderRelease) {
    // 0.0.1 was sealed before HASH and BUILT were written.
    const DeckRelease::Release release =
            DeckRelease::parseRelease(QStringLiteral("VERSION=0.0.1\nCOMMIT=pi/v0.0.1\n"));
    EXPECT_EQ(QStringLiteral("0.0.1"), release.version);
    EXPECT_EQ(QStringLiteral("pi/v0.0.1"), release.commit);
    EXPECT_TRUE(release.hash.isEmpty());
    EXPECT_TRUE(release.built.isEmpty());
}

TEST(DeckReleaseTest, ReleaseFileIgnoresWhatItDoesNotKnow) {
    // A fault build rewrites VERSION in place; anything added later is skipped.
    const DeckRelease::Release release = DeckRelease::parseRelease(QStringLiteral(
            "VERSION=0.0.9-nonet\nCOMMIT=pi/v0.0.9\nFUTURE=1\n\nnot a line\n"));
    EXPECT_EQ(QStringLiteral("0.0.9-nonet"), release.version);
    EXPECT_EQ(QStringLiteral("pi/v0.0.9"), release.commit);
}

// ---------------------------------------------------------------------------
// The slot started, and whether as a trial.
// ---------------------------------------------------------------------------

TEST(DeckReleaseTest, SlotFromTheCommandLine) {
    // The emulated deck's, slot A of release 0.0.1.
    EXPECT_EQ(QChar('A'),
            DeckRelease::slotOnCommandLine(QStringLiteral(
                    "console=ttyS0,115200 coherent_pool=1M watchdog.open_timeout=60 "
                    "console=tty1 root=PARTUUID=5d0bc1ec-05 rootfstype=squashfs "
                    "rootwait panic=10 overlayroot=tmpfs:recurse=0 rauc.slot=A "
                    "systemd.mount-extra=PARTUUID=5d0bc1ec-02:/boot/firmware:vfat:ro\n")));
    EXPECT_EQ(QChar('B'), DeckRelease::slotOnCommandLine(QStringLiteral("quiet rauc.slot=B")));
    EXPECT_EQ(QChar('B'), DeckRelease::slotOnCommandLine(QStringLiteral("rauc.slot=B\n")));
}

TEST(DeckReleaseTest, NoSlotOnADevCard) {
    EXPECT_TRUE(DeckRelease::slotOnCommandLine(
            QStringLiteral("console=tty1 root=PARTUUID=0a1b2c3d-02 rootfstype=ext4 rootwait\n"))
                    .isNull());
    EXPECT_TRUE(DeckRelease::slotOnCommandLine(QStringLiteral("rauc.slot=C")).isNull());
    EXPECT_TRUE(DeckRelease::slotOnCommandLine(QStringLiteral("rauc.slot=AB")).isNull());
    EXPECT_TRUE(DeckRelease::slotOnCommandLine(QStringLiteral("myrauc.slot=A")).isNull());
    EXPECT_TRUE(DeckRelease::slotOnCommandLine(QString()).isNull());
}

TEST(DeckReleaseTest, TrialFlagIsABigEndianOne) {
    EXPECT_TRUE(DeckRelease::isTrial(QByteArray("\x00\x00\x00\x01", 4)));
    EXPECT_FALSE(DeckRelease::isTrial(QByteArray("\x00\x00\x00\x00", 4)));
    EXPECT_FALSE(DeckRelease::isTrial(QByteArray("\x01\x00\x00\x00", 4)));
    // Not a Pi, or no firmware that knows tryboot.
    EXPECT_FALSE(DeckRelease::isTrial(QByteArray()));
}

// ---------------------------------------------------------------------------
// The committed slot, from the copy of autoboot.txt.
// ---------------------------------------------------------------------------

TEST(DeckReleaseTest, CommittedSlotAsTheBackendWritesIt) {
    EXPECT_EQ(QChar('A'), DeckRelease::committedIn(autobootFor(2, 3)));
    EXPECT_EQ(QChar('B'), DeckRelease::committedIn(autobootFor(3, 2)));
}

TEST(DeckReleaseTest, CommittedSlotAsTheFirmwareReadsIt) {
    // [all]'s last boot_partition wins; [tryboot]'s is the trial's, not the
    // committed one; and a Mac editor's line ends and spaces change nothing.
    EXPECT_EQ(QChar('B'),
            DeckRelease::committedIn(QStringLiteral(
                    "[all]\r\ntryboot_a_b=1\r\nboot_partition = 2\r\nboot_partition = 3\r\n"
                    "[tryboot]\r\nboot_partition=2\r\n")));
    // Lines before any section are [all]'s.
    EXPECT_EQ(QChar('A'), DeckRelease::committedIn(QStringLiteral("boot_partition=2\n")));
}

TEST(DeckReleaseTest, NoCommittedSlotInADamagedFile) {
    EXPECT_TRUE(DeckRelease::committedIn(QString()).isNull());
    EXPECT_TRUE(DeckRelease::committedIn(QStringLiteral("\x01\x02garbage")).isNull());
    EXPECT_TRUE(DeckRelease::committedIn(QStringLiteral("[tryboot]\nboot_partition=3\n")).isNull());
    // p1 is no slot.
    EXPECT_TRUE(DeckRelease::committedIn(autobootFor(1, 2)).isNull());
}

// ---------------------------------------------------------------------------
// What RAUC installed.
// ---------------------------------------------------------------------------

TEST(DeckReleaseTest, NothingInstalledOnANewCard) {
    // No central status file at all until RAUC first writes a slot.
    EXPECT_TRUE(DeckRelease::parseInstalls(kSystemConf, QString()).isEmpty());
}

TEST(DeckReleaseTest, ReleaseInstalledInB) {
    const auto installs = DeckRelease::parseInstalls(kSystemConf, kInstalledInB);
    ASSERT_TRUE(installs.contains(QChar('B')));
    EXPECT_FALSE(installs.contains(QChar('A')));
    const DeckRelease::Install b = installs.value(QChar('B'));
    EXPECT_EQ(QStringLiteral("0.0.1"), b.version);
    EXPECT_EQ(QStringLiteral("pi/v0.0.1"), b.build);
    EXPECT_EQ(QStringLiteral("2026-10-09T09:43:09Z"), b.timestamp);
    EXPECT_TRUE(b.ok());
}

TEST(DeckReleaseTest, AnActivationIsNoInstall) {
    // `rauc status mark-active` on A, which came on the card, records only
    // when it was activated.
    const auto installs = DeckRelease::parseInstalls(kSystemConf,
            kInstalledInB +
                    QStringLiteral("\n[slot.rootfs.0]\n"
                                   "activated.timestamp=2026-10-09T10:15:00Z\n"
                                   "activated.count=1\n"));
    EXPECT_FALSE(installs.contains(QChar('A')));
    EXPECT_TRUE(installs.contains(QChar('B')));
}

TEST(DeckReleaseTest, AnInstallIsWholeOnlyIfEveryPartIs) {
    // RAUC marks a slot pending before it writes it, and ok or failed after.
    // The system's status is followed by its checksum, the boot partition's
    // by its time.
    QString pending = kInstalledInB;
    pending.replace(QStringLiteral("status=ok\nsha256="), QStringLiteral("status=pending\nsha256="));
    ASSERT_NE(pending, kInstalledInB);
    EXPECT_EQ(QStringLiteral("pending"),
            DeckRelease::parseInstalls(kSystemConf, pending).value(QChar('B')).status);

    // The system written, its boot partition not.
    QString bootFailed = kInstalledInB;
    bootFailed.replace(QStringLiteral("status=ok\ninstalled.timestamp=2026-10-09T09:43:10Z"),
            QStringLiteral("status=failed\ninstalled.timestamp=2026-10-09T09:43:10Z"));
    ASSERT_NE(bootFailed, kInstalledInB);
    const DeckRelease::Install b =
            DeckRelease::parseInstalls(kSystemConf, bootFailed).value(QChar('B'));
    EXPECT_EQ(QStringLiteral("failed"), b.status);
    EXPECT_FALSE(b.ok());
    EXPECT_EQ(QStringLiteral("0.0.1"), b.version);
}

// ---------------------------------------------------------------------------
// The health check's verdicts, in pi_config/trimixxx-health's own words.
// ---------------------------------------------------------------------------

TEST(DeckReleaseTest, ATrialKept) {
    // As the emulated deck logged it, the clock a day behind (no NTP yet).
    const QString log = QStringLiteral(
            "2026-10-08T09:57:52Z +4s trial of slot B (0.0.1)\n"
            "2026-10-08T09:57:54Z +6s slot B can take the next release: committed\n");
    const DeckRelease::Verdict b = DeckRelease::verdictOf(log, QChar('B'));
    EXPECT_EQ(Kind::Kept, b.kind);
    EXPECT_EQ(QStringLiteral("0.0.1"), b.version);
    EXPECT_EQ(Kind::None, DeckRelease::verdictOf(log, QChar('A')).kind);
}

TEST(DeckReleaseTest, ATrialThatCouldNotTakeTheNextRelease) {
    const DeckRelease::Verdict b = DeckRelease::verdictOf(
            QStringLiteral("2026-10-09T10:02:10Z +4s trial of slot B (0.0.9-nonet)\n"
                           "2026-10-09T10:04:40Z +154s slot B can't take the next release "
                           "after 150 s (no network: NetworkManager says disconnected): marked "
                           "bad, rebooting to the committed slot\n"),
            QChar('B'));
    EXPECT_EQ(Kind::Failed, b.kind);
    EXPECT_EQ(QStringLiteral("0.0.9-nonet"), b.version);
    EXPECT_EQ(QStringLiteral("no network: NetworkManager says disconnected"), b.why);
}

TEST(DeckReleaseTest, ATrialRaucCouldNotCommit) {
    const DeckRelease::Verdict a = DeckRelease::verdictOf(
            QStringLiteral("2026-10-09T10:02:10Z +4s trial of slot A (0.0.9)\n"
                           "2026-10-09T10:02:12Z +6s slot A: RAUC can't commit it, so it can't "
                           "take the next release either; rebooting to the committed slot\n"),
            QChar('A'));
    EXPECT_EQ(Kind::Failed, a.kind);
    EXPECT_FALSE(a.why.isEmpty());
}

TEST(DeckReleaseTest, AFallBackMarksTheCommittedSlotBad) {
    // B was committed, failed to start, and the bootloader's watchdog started
    // A, which the health check then committed.
    const QString log = QStringLiteral(
            "2026-10-09T09:00:00Z +4s trial of slot B (0.0.9)\n"
            "2026-10-09T09:00:02Z +6s slot B can take the next release: committed\n"
            "2026-10-09T11:00:00Z +5s slot A started, but B is committed: the bootloader's "
            "watchdog fell back (or a reboot by hand)\n"
            "2026-10-09T11:00:02Z +7s slot A can take the next release: committed, B marked "
            "bad\n");
    const DeckRelease::Verdict b = DeckRelease::verdictOf(log, QChar('B'));
    EXPECT_EQ(Kind::Failed, b.kind);
    EXPECT_EQ(QStringLiteral("0.0.9"), b.version);
    EXPECT_EQ(Kind::Kept, DeckRelease::verdictOf(log, QChar('A')).kind);
}

TEST(DeckReleaseTest, TheLastTrialDecides) {
    // A failed release, then a good one in the same slot.
    const QString log = QStringLiteral(
            "2026-10-09T10:02:10Z +4s trial of slot B (0.0.9-nonet)\n"
            "2026-10-09T10:04:40Z +154s slot B can't take the next release after 150 s (no "
            "network: NetworkManager says disconnected): marked bad, rebooting to the "
            "committed slot\n"
            "2026-10-09T10:10:00Z +4s trial of slot B (0.0.9)\n");
    const DeckRelease::Verdict onTrial = DeckRelease::verdictOf(log, QChar('B'));
    EXPECT_EQ(Kind::OnTrial, onTrial.kind);
    EXPECT_EQ(QStringLiteral("0.0.9"), onTrial.version);
    EXPECT_TRUE(onTrial.why.isEmpty());

    const DeckRelease::Verdict kept = DeckRelease::verdictOf(log +
                    QStringLiteral("2026-10-09T10:10:02Z +6s slot B can take the next release: "
                                   "committed\n"),
            QChar('B'));
    EXPECT_EQ(Kind::Kept, kept.kind);
    EXPECT_EQ(QStringLiteral("0.0.9"), kept.version);
}

TEST(DeckReleaseTest, LinesThatAreNoVerdict) {
    const QString log = QStringLiteral(
            "2026-10-09T10:00:00Z +3s cannot tell which slot started; leaving everything as "
            "it is\n"
            "2026-10-09T10:01:00Z +3s autoboot.txt restored; rebooting into the committed "
            "slot\n"
            "2026-10-09T10:02:00Z +5s slot A can't take the next release either (sshd isn't "
            "running): nothing changed\n"
            "trial of slot B (0.0.9)\n"
            "\n");
    EXPECT_EQ(Kind::None, DeckRelease::verdictOf(log, QChar('A')).kind);
    EXPECT_EQ(Kind::None, DeckRelease::verdictOf(log, QChar('B')).kind);
    EXPECT_EQ(Kind::None, DeckRelease::verdictOf(QString(), QChar('B')).kind);
}

// ---------------------------------------------------------------------------
// Whether RESTART INTO may run.
// ---------------------------------------------------------------------------

DeckRelease settled(QChar booted) {
    DeckRelease release;
    release.booted = booted;
    release.other = booted == QChar('A') ? QChar('B') : QChar('A');
    release.committed = booted;
    return release;
}

DeckRelease::Install installed(const QString& version, const QString& status = QStringLiteral("ok")) {
    return DeckRelease::Install{version, QStringLiteral("pi/v") + version, QString(), status};
}

TEST(DeckReleaseTest, NeverOnADevCard) {
    EXPECT_EQ(Blocked::DevCard, DeckRelease().blocked());
}

TEST(DeckReleaseTest, TheCardsOwnReleaseInAIsThere) {
    // Running B after an update; A still holds what the card came with.
    DeckRelease release = settled(QChar('B'));
    release.trial = true; // kept: the flag lasts the whole start
    EXPECT_EQ(Blocked::No, release.blocked());
    EXPECT_TRUE(release.otherVersion().isEmpty());
    // Once a trial of A has named it, so does the page.
    release.otherVerdict = DeckRelease::Verdict{Kind::Kept, QStringLiteral("0.0.1"), QString()};
    EXPECT_EQ(QStringLiteral("0.0.1"), release.otherVersion());
}

TEST(DeckReleaseTest, NotIntoAnEmptyB) {
    // A trial of an empty slot would start A again, flagged as a trial.
    EXPECT_EQ(Blocked::OtherEmpty, settled(QChar('A')).blocked());
}

TEST(DeckReleaseTest, IntoAnInstalledB) {
    DeckRelease release = settled(QChar('A'));
    release.otherInstall = installed(QStringLiteral("0.0.9"));
    EXPECT_EQ(Blocked::No, release.blocked());
    EXPECT_EQ(QStringLiteral("0.0.9"), release.otherVersion());
}

TEST(DeckReleaseTest, NotIntoAnUnfinishedInstall) {
    for (const QString& status : {QStringLiteral("pending"), QStringLiteral("failed")}) {
        DeckRelease release = settled(QChar('A'));
        release.otherInstall = installed(QStringLiteral("0.0.9"), status);
        EXPECT_EQ(Blocked::OtherUnfinished, release.blocked()) << status.toStdString();
    }
}

TEST(DeckReleaseTest, NotIntoASlotThatFailedItsTrial) {
    DeckRelease release = settled(QChar('A'));
    release.otherInstall = installed(QStringLiteral("0.0.9-nonet"));
    release.otherVerdict = DeckRelease::Verdict{
            Kind::Failed, QStringLiteral("0.0.9-nonet"), QStringLiteral("no network")};
    EXPECT_EQ(Blocked::OtherFailed, release.blocked());

    // A release installed there since, not yet started.
    release.otherInstall = installed(QStringLiteral("0.0.9"));
    EXPECT_EQ(Blocked::No, release.blocked());

    // Marked bad after a fall-back, which names no release: no telling.
    release.otherVerdict.version.clear();
    EXPECT_EQ(Blocked::OtherFailed, release.blocked());
}

TEST(DeckReleaseTest, AnUnfinishedTrialMayBeTriedAgain) {
    // Its start ended before a verdict: a power cut, a hang.
    DeckRelease release = settled(QChar('A'));
    release.otherInstall = installed(QStringLiteral("0.0.9"));
    release.otherVerdict = DeckRelease::Verdict{Kind::OnTrial, QStringLiteral("0.0.9"), QString()};
    EXPECT_EQ(Blocked::No, release.blocked());
}

TEST(DeckReleaseTest, NotWhileThisStartIsUnsettled) {
    DeckRelease release = settled(QChar('B'));
    release.committed = QChar('A');
    release.trial = true;
    EXPECT_EQ(Blocked::TrialPending, release.blocked());
    release.trial = false;
    EXPECT_EQ(Blocked::FellBack, release.blocked());
    release.committed = QChar();
    EXPECT_EQ(Blocked::CommittedUnknown, release.blocked());
}

// ---------------------------------------------------------------------------
// All of it from a card's files.
// ---------------------------------------------------------------------------

class DeckReleaseCardTest : public testing::Test {
  protected:
    void write(const QString& path, const QByteArray& contents) const {
        const QString file = m_root.path() + path;
        ASSERT_TRUE(QDir().mkpath(QFileInfo(file).path()));
        QFile out(file);
        ASSERT_TRUE(out.open(QIODevice::WriteOnly));
        out.write(contents);
    }

    QTemporaryDir m_root;
};

TEST_F(DeckReleaseCardTest, ADevCard) {
    write(QStringLiteral("/proc/cmdline"), "console=tty1 root=PARTUUID=0a1b2c3d-02 rootwait\n");
    const DeckRelease release = DeckRelease::read(m_root.path());
    EXPECT_FALSE(release.isReleaseCard());
    EXPECT_TRUE(release.release.version.isEmpty());
    EXPECT_EQ(Blocked::DevCard, release.blocked());
}

TEST_F(DeckReleaseCardTest, AReleaseCardOnItsUpdate) {
    // Release 0.0.1 shipped into B, its trial kept: B runs, A is the card's.
    write(QStringLiteral("/etc/trimixxx-release"), "VERSION=0.0.1\nCOMMIT=pi/v0.0.1\n");
    write(QStringLiteral("/proc/cmdline"), "console=tty1 rauc.slot=B overlayroot=tmpfs:recurse=0\n");
    write(QStringLiteral("/proc/device-tree/chosen/bootloader/tryboot"),
            QByteArray("\x00\x00\x00\x01", 4));
    write(QStringLiteral("/var/lib/rauc/autoboot.txt"), autobootFor(3, 2).toUtf8());
    write(QStringLiteral("/etc/rauc/system.conf"), kSystemConf.toUtf8());
    write(QStringLiteral("/var/lib/rauc/central.raucs"), kInstalledInB.toUtf8());
    write(QStringLiteral("/var/lib/rauc/trimixxx-health.log"),
            "2026-10-08T09:57:52Z +4s trial of slot B (0.0.1)\n"
            "2026-10-08T09:57:54Z +6s slot B can take the next release: committed\n");

    const DeckRelease release = DeckRelease::read(m_root.path());
    EXPECT_TRUE(release.isReleaseCard());
    EXPECT_EQ(QStringLiteral("0.0.1"), release.release.version);
    EXPECT_EQ(QChar('B'), release.booted);
    EXPECT_TRUE(release.trial);
    EXPECT_EQ(QChar('B'), release.committed);
    EXPECT_EQ(QChar('A'), release.other);
    EXPECT_FALSE(release.otherInstall.has_value());
    EXPECT_EQ(Kind::None, release.otherVerdict.kind);
    EXPECT_EQ(Blocked::No, release.blocked());
}

TEST_F(DeckReleaseCardTest, ANewCardBeforeItsFirstUpdate) {
    write(QStringLiteral("/etc/trimixxx-release"), "VERSION=0.0.1\nCOMMIT=pi/v0.0.1\n");
    write(QStringLiteral("/proc/cmdline"), "rauc.slot=A\n");
    write(QStringLiteral("/proc/device-tree/chosen/bootloader/tryboot"),
            QByteArray("\x00\x00\x00\x00", 4));
    write(QStringLiteral("/var/lib/rauc/autoboot.txt"), autobootFor(2, 3).toUtf8());
    write(QStringLiteral("/etc/rauc/system.conf"), kSystemConf.toUtf8());

    const DeckRelease release = DeckRelease::read(m_root.path());
    EXPECT_EQ(QChar('A'), release.booted);
    EXPECT_FALSE(release.trial);
    EXPECT_EQ(QChar('A'), release.committed);
    EXPECT_EQ(Blocked::OtherEmpty, release.blocked());
}

TEST_F(DeckReleaseCardTest, TheVerdictInThePreviousLog) {
    // The health check moves its log to .1 at 64 KiB; the verdict may be
    // there, and the newer file has the last word.
    write(QStringLiteral("/proc/cmdline"), "rauc.slot=A\n");
    write(QStringLiteral("/var/lib/rauc/autoboot.txt"), autobootFor(2, 3).toUtf8());
    write(QStringLiteral("/etc/rauc/system.conf"), kSystemConf.toUtf8());
    write(QStringLiteral("/var/lib/rauc/central.raucs"), kInstalledInB.toUtf8());
    write(QStringLiteral("/var/lib/rauc/trimixxx-health.log.1"),
            "2026-10-08T09:57:52Z +4s trial of slot B (0.0.1)\n"
            "2026-10-08T09:58:20Z +154s slot B can't take the next release after 150 s "
            "(sshd isn't running): marked bad, rebooting to the committed slot\n");
    EXPECT_EQ(Blocked::OtherFailed, DeckRelease::read(m_root.path()).blocked());

    write(QStringLiteral("/var/lib/rauc/trimixxx-health.log"),
            "2026-10-09T09:57:52Z +4s trial of slot B (0.0.1)\n"
            "2026-10-09T09:57:54Z +6s slot B can take the next release: committed\n");
    EXPECT_EQ(Kind::Kept, DeckRelease::read(m_root.path()).otherVerdict.kind);
}

// ---------------------------------------------------------------------------
// Diagnostics' uptime.
// ---------------------------------------------------------------------------

TEST(WDeckDiagnosticsTest, Uptime) {
    EXPECT_EQ(QStringLiteral("0 s"), WDeckDiagnostics::uptimeText(0));
    EXPECT_EQ(QStringLiteral("59 s"), WDeckDiagnostics::uptimeText(59));
    EXPECT_EQ(QStringLiteral("1 min"), WDeckDiagnostics::uptimeText(60));
    EXPECT_EQ(QStringLiteral("59 min"), WDeckDiagnostics::uptimeText(3599));
    EXPECT_EQ(QStringLiteral("1 h 00 min"), WDeckDiagnostics::uptimeText(3600));
    EXPECT_EQ(QStringLiteral("2 h 05 min"), WDeckDiagnostics::uptimeText(2 * 3600 + 5 * 60 + 59));
    EXPECT_EQ(QStringLiteral("1 d 0 h"), WDeckDiagnostics::uptimeText(86400));
    EXPECT_EQ(QStringLiteral("3 d 4 h"), WDeckDiagnostics::uptimeText(3 * 86400 + 4 * 3600 + 5));
}

} // namespace
