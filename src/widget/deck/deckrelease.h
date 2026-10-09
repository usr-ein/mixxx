#pragma once

#include <QByteArray>
#include <QChar>
#include <QHash>
#include <QString>
#include <optional>

namespace mixxx {
namespace deck {

/// What the deck runs, and from where, for Diagnostics.
///
/// A release card (pi-qemu/PLAN.md Part 1) starts one of two slots, A or B,
/// each holding a release sealed by `pi-qemu release build`. A new card holds
/// its release in A and nothing in B; RAUC installs an update into whichever
/// slot is not running, has the firmware start it once as a trial, and the
/// health check (pi_config/trimixxx-health) commits it or falls back. A dev
/// card has no slots and no release, and reads as such here.
///
/// **Read from files, never by asking RAUC.** Every `rauc status` has the
/// backend mount the card's p1 read-write to find the committed slot
/// (pi_config/rauc/rpi-tryboot get-primary), and Diagnostics refreshes once a
/// second. Every one of these is readable by the deck's user:
///
///  - /etc/trimixxx-release: the release running (pi-qemu/release/container.sh).
///  - /proc/cmdline: the slot started, `rauc.slot=A|B`, as the splash reads it.
///  - /proc/device-tree/chosen/bootloader/tryboot: 1 when the firmware started
///    this slot as a trial. It stays 1 for the whole of that start, after the
///    health check has committed the slot too.
///  - /var/lib/rauc/autoboot.txt: the backend's copy of p1's file, naming the
///    committed slot under [all]. The health check's first start writes it.
///  - /etc/rauc/system.conf, /var/lib/rauc/central.raucs: which RAUC slot is A
///    and which B, and what RAUC installed in each. A slot that came on the card
///    has no record.
///  - /var/lib/rauc/trimixxx-health.log, and its previous file .1: the health
///    check's verdicts, in that script's words. RAUC keeps no record of a slot
///    marked bad, so this log is the only one.
///
/// The decisions are pure functions of those files' text, so they are tested
/// without a card (deckrelease_test.cpp).
struct DeckRelease {
    /// /etc/trimixxx-release. A release sealed before HASH and BUILT were
    /// written (0.0.1) has only the first two.
    struct Release {
        QString version;
        /// `git describe`: the release's tag, or how far past it, with -dirty
        /// and "(rehearsal)" when they apply.
        QString commit;
        QString hash;
        /// When it was sealed, in UTC: 2026-10-09T10:00Z.
        QString built;
    };

    /// What RAUC recorded installing into a slot.
    struct Install {
        QString version;
        /// The bundle's build: the release's `git describe`, as COMMIT.
        QString build;
        /// The deck's clock as RAUC installed it, in UTC.
        QString timestamp;
        /// "ok", or the first other status of the slot and its boot partition:
        /// "failed", or "pending" for an install that never finished.
        QString status;

        bool ok() const {
            return status == QLatin1String("ok");
        }
    };

    /// The health check's last word on a slot.
    struct Verdict {
        enum class Kind {
            /// Not in the log: never on trial, or the log is gone.
            None,
            /// A trial started and no verdict followed: it is being decided
            /// now, or the start ended before it was (a power cut, a hang).
            OnTrial,
            /// It could take the next release, and was committed.
            Kept,
            /// It couldn't: marked bad, or RAUC couldn't commit it.
            Failed,
        };
        Kind kind = Kind::None;
        /// The release its last trial started, from "trial of slot X (V)".
        QString version;
        /// Why it failed, in the health check's words.
        QString why;
    };

    /// What stops a restart into the other slot, if anything.
    enum class Blocked {
        No,
        /// A dev card: no slots.
        DevCard,
        /// The copy of autoboot.txt can't be read: RAUC's partition is missing.
        CommittedUnknown,
        /// This start is a trial the health check hasn't kept yet. RESTART
        /// would end it, and RAUC would put the committed slot back.
        TrialPending,
        /// This slot started though the other is committed: the bootloader
        /// fell back, or someone ran `reboot N`. The health check settles it.
        FellBack,
        /// Nothing was ever installed in the other slot: B on a new card. A
        /// trial of it would start A again, flagged as a trial.
        OtherEmpty,
        /// The other slot's install failed, or never finished.
        OtherUnfinished,
        /// The other slot failed its last trial.
        OtherFailed,
    };

    Release release;
    /// 'A' or 'B'; null on a dev card.
    QChar booted;
    /// The firmware started this slot as a trial.
    bool trial = false;
    /// 'A' or 'B'; null if it can't be told.
    QChar committed;
    /// The slot not running; null on a dev card.
    QChar other;
    /// RAUC's record of the other slot, if RAUC ever installed there.
    std::optional<Install> otherInstall;
    Verdict otherVerdict;

    bool isReleaseCard() const {
        return !booted.isNull();
    }

    Blocked blocked() const;

    /// The other slot failed its last trial, and no release went in since.
    bool otherFailed() const;

    /// The release in the other slot, as far as it is known: RAUC's record;
    /// for A without one, the release it came on the card with, if a trial of
    /// it ever named it; else empty.
    QString otherVersion() const;

    /// The deck's own files, or the same paths under *root* in a test.
    static DeckRelease read(const QString& root = QString());

    // ---- Each file's reader, pure. ----------------------------------------

    static Release parseRelease(const QString& text);
    /// `rauc.slot=` on a kernel command line.
    static QChar slotOnCommandLine(const QString& cmdline);
    /// The device tree's tryboot: a big-endian u32, 1 on a trial.
    static bool isTrial(const QByteArray& tryboot);
    /// The slot an autoboot.txt commits, as the firmware reads it: the last
    /// boot_partition of [all] (2 is A, 3 is B), whatever its line ends.
    static QChar committedIn(const QString& autobootTxt);
    /// What RAUC installed in each slot, by bootname, from its system.conf and
    /// central status file. A slot RAUC never wrote is absent.
    static QHash<QChar, Install> parseInstalls(
            const QString& systemConf, const QString& centralStatus);
    /// The last verdict on *slot* in the health check's log, oldest line first.
    static Verdict verdictOf(const QString& healthLog, QChar slot);
};

} // namespace deck
} // namespace mixxx
