#pragma once

#include <QObject>
#include <QString>
#include <atomic>

namespace mixxx {
namespace deck {

/// The DAC's line input into an MP3 file: what "Record set" records.
///
/// The deck's sound card opened for capture through ALSA, only for as long as
/// a recording lasts, beside the output Mixxx's engine holds: the UCA222's
/// input is a USB stream of its own, so the engine and its sound config are
/// untouched (Sam: "1a", no new process). What comes in is measured for the
/// meter and the silence prompt, and encoded by LAME as it comes, at
/// setrecording::kBitrateKbps, straight into the file.
///
/// **On a thread of its own** (DeckRecorder's): run() takes the thread until
/// the recording ends, reading the card every kBlockFrames. A read waits for
/// the card, an encode is a few milliseconds of CPU, and neither belongs on
/// the GUI thread or in the engine's callback.
class InputCapture : public QObject {
    Q_OBJECT

  public:
    /// The deck's sound card, by its ALSA id: the UCA222 calls itself
    /// "USB Audio CODEC", which ALSA makes "CODEC" -- and so does the emulated
    /// card (pi-qemu, QEMU patch 20).
    static constexpr const char* kDevice = "hw:CARD=CODEC,DEV=0";
    static constexpr int kRate = 44100;
    static constexpr int kChannels = 2;
    /// What is read and measured at a time: 50 ms, the meter's pace.
    static constexpr int kBlockFrames = kRate / 20;

    /// Why run() returned.
    enum class End {
        Asked,   ///< requestStop()
        Limit,   ///< the most frames it was given: three hours
        Failed,  ///< what failed, finished() says
    };

    explicit InputCapture(QObject* pParent = nullptr);
    ~InputCapture() override;

    /// Ready for the next run(), before it is queued: a stop asked for
    /// between the two is then kept, not forgotten by run() starting.
    void arm() {
        m_stop.store(false);
        m_frames.store(0);
    }

    /// Ask run() to finish, from any thread: it does within one block.
    void requestStop() {
        m_stop.store(true);
    }

    /// The frames recorded so far, from any thread.
    qint64 frames() const {
        return m_frames.load();
    }

  public slots:
    /// Record into *path*, a file it makes, at most *maxFrames*, then return.
    /// *title* goes into the file's tag. Says levels() as it goes, and
    /// finished() once the file is closed.
    void run(const QString& path, const QString& title, qint64 maxFrames);

  signals:
    /// The loudest of each channel since the last, 0..1, and the frames
    /// recorded so far. Every block.
    void levels(float left, float right, qint64 frames);
    /// The file is closed: what it holds, and why it ended. *error* says what
    /// failed, for End::Failed.
    void finished(qint64 frames, mixxx::deck::InputCapture::End end, const QString& error);

  private:
    std::atomic<bool> m_stop{false};
    std::atomic<qint64> m_frames{0};
};

} // namespace deck
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::deck::InputCapture::End)
