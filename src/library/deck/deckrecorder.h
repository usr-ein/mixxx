#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
#include <memory>

#include "library/deck/inputcapture.h"
#include "library/deck/setrecording.h"

class ControlProxy;
class QProcess;

namespace mixxx {
namespace deck {

class DeckHistory;
class MediaRegistry;

/// "Record set" (Sam): the DAC's line input recorded onto the card, then
/// written, with the History's track list for it, to one of the deck's own
/// sticks.
///
/// **One recording at a time**, in a directory of its own on the card
/// (pi_config/trimixxx-recording.service: the state partition on a release
/// card), which every boot empties: a recording lasts until it is written or
/// discarded, or until the next power-up, and never lives in RAM.
///
///  - **Idle**: "Record set" starts a recording.
///  - **Recording**: InputCapture records, on a thread of its own, for three
///    hours at most. The card's room is watched, and so is the input, for the
///    silence prompt (`[TriMixxx],record_silence`, the skin's overlay).
///  - **Recorded**: stopped, on the card, until the DJ writes it to a stick
///    (writeTo(): dj-usb's `write`, through sudo, one stick at a time) or
///    discards it. Written once, it is done when the DJ leaves the list of
///    sticks (finish()), and the files stay until the next recording or
///    power-up.
///
/// A Mixxx that quits while recording ends the recording there; started again
/// within the same power-up, it finds it Recorded (`recording.json`), ready to
/// be written.
class DeckRecorder : public QObject {
    Q_OBJECT

  public:
    enum class State {
        Idle,
        Recording,
        Recorded,
    };

    /// One of the deck's own sticks, as the list to write to shows it.
    struct Stick {
        /// Where dj-usb mounted it: /media/DJ_USB_N.
        QString mountPoint;
        /// Which of the deck's ports, N.
        int slot = 0;
        QString name;
        qint64 freeBytes = -1;
        /// Why the set cannot go onto it, or empty: a filesystem the deck does
        /// not write, too little room.
        QString refusal;
        /// The set is on it already: written this power-up.
        bool written = false;
    };

    /// Where the deck records: pi_config/trimixxx-recording.service.
    static QString defaultDirectory();
    /// Room kept on the card when a recording starts, on top of three hours
    /// of it, and the room under which a recording stops (Sam: "2a").
    static constexpr qint64 kStartSpareBytes = 256LL * 1024 * 1024;
    static constexpr qint64 kStopBelowBytes = 128LL * 1024 * 1024;

    /// *pHistory* names what played; *pRegistry* names the sticks. Used and
    /// not owned.
    DeckRecorder(DeckHistory* pHistory,
            MediaRegistry* pRegistry,
            const QString& directory = defaultDirectory(),
            QObject* pParent = nullptr);
    ~DeckRecorder() override;

    State state() const {
        return m_state;
    }
    /// Why a recording cannot start now, in a few words for the menu; empty
    /// when it can (or one is under way).
    QString cannotStart() const;
    void start();
    /// End the recording. Recorded once the file is closed, which is within a
    /// block (50 ms).
    void stop();
    /// How long it has recorded: so far, or in all.
    qint64 lengthMs() const;
    /// The 8 digits of its files' names.
    QString digits() const {
        return m_digits;
    }

    /// The loudest of each channel since the last call, in dBFS: for the
    /// meter, which draws L and R apart (Sam: "to diagnose bad cabling").
    QPair<double, double> takePeaks();

    /// The deck's own sticks, now: what dj-usb has mounted in its ports.
    QList<Stick> sticks() const;
    bool isWriting() const {
        return m_pWriter != nullptr;
    }
    /// The stick being written to, by its mount point, and by its name; empty
    /// when none is.
    QString writingTo() const {
        return m_writeMountPoint;
    }
    QString writingName() const {
        return m_writeName;
    }
    /// How much of the write is done, 0..1.
    double writeProgress() const;
    /// Write the set and its track list onto the stick at *mountPoint*.
    void writeTo(const QString& mountPoint);
    /// Whether the set went onto a stick yet.
    bool isWritten() const {
        return !m_written.isEmpty();
    }
    /// Delete the recording, unwritten or not. Not while it writes.
    void discard();
    /// The DJ is done with a written set: Idle, and "Record set" again.
    void finish();

    /// The track list of the recording, as written next to it.
    QString trackListText() const;

  signals:
    /// The state, what has been written, a write starting or ending.
    void stateChanged();
    /// A second gone while recording; progress while writing.
    void tick();
    /// New levels for the meter: every block while recording.
    void levelsChanged();
    /// Said to the DJ in the corner: a recording that stopped by itself, a
    /// write done or failed.
    void notice(const QString& text);
    /// The silence prompt's END RECORDING: the recording has stopped, and the
    /// DJ wants the sticks to write it to.
    void endAsked();

  private:
    /// Whether this set's names are on the stick at *mountPoint* already.
    bool namesTaken(const QString& mountPoint) const;
    /// New digits, free on the stick at *mountPoint* and on the card: the
    /// recording renamed, its tag's title with it.
    bool renumber(const QString& mountPoint);
    void onLevels(float left, float right, qint64 frames);
    void onCaptureFinished(qint64 frames, InputCapture::End end, const QString& error);
    void onWriteFinished(int exitCode, bool crashed);
    void checkRoom();
    void updateSilencePrompt();
    QString audioPath() const;
    QString trackListPath() const;
    /// Which stick is at *mountPoint*, as it is told apart from the next one
    /// in the same port: its filesystem's UUID, which dj-usb wrote down.
    static QString stickId(const QString& mountPoint);
    QString stickName(const QString& mountPoint, int slot) const;
    /// What the set takes on a stick, with room to spare.
    qint64 bytesNeeded() const;
    /// recording.json: which recording, of which power-up, from when to when,
    /// written where, and whether the DJ is done with it.
    void save() const;
    void load();
    /// The recording's files, and nothing else of the directory's.
    void clearDirectory();

    DeckHistory* const m_pHistory;
    MediaRegistry* const m_pRegistry;
    const QString m_directory;

    QThread m_thread;
    /// Lives on m_thread; deleted once it has stopped.
    InputCapture* m_pCapture = nullptr;

    State m_state = State::Idle;
    QString m_digits;
    QByteArray m_bootId;
    /// On the History's clock, so the track list is cut where it recorded.
    qint64 m_startMs = 0;
    qint64 m_endMs = -1;
    qint64 m_frames = 0;
    /// Why it was asked to stop, when it was not the DJ: the card full.
    QString m_stopReason;
    /// The sticks it is on, by stickId().
    QSet<QString> m_written;

    float m_peakLeft = 0.0f;
    float m_peakRight = 0.0f;
    setrecording::SilenceWatch m_silence;

    QTimer m_tick;
    QTimer m_roomCheck;

    QProcess* m_pWriter = nullptr;
    QString m_writeMountPoint;
    QString m_writeName;
    /// The files being written, by name, and their bytes.
    QStringList m_writeFiles;
    qint64 m_writeTotalBytes = 0;
    /// The stick's block device, and what it had written when the write began.
    QString m_writeDevice;
    qint64 m_writeBaseline = -1;
    QTimer m_writeProgress;

    std::unique_ptr<ControlProxy> m_pSilencePrompt;
    std::unique_ptr<ControlProxy> m_pSilenceKeep;
    std::unique_ptr<ControlProxy> m_pSilenceEnd;
};

} // namespace deck
} // namespace mixxx
