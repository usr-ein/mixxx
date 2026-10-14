#include "library/deck/inputcapture.h"

#include <lame/lame.h>

#include <QFile>
#include <algorithm>
#include <cstdlib>
#include <vector>

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

#include "library/deck/setrecording.h"
#include "moc_inputcapture.cpp"
#include "util/logger.h"

namespace mixxx {
namespace deck {

namespace {

const mixxx::Logger kLogger("InputCapture");

/// LAME's own trade, 0 (best, slowest) to 9: 5 is its "good quality, fast".
/// At 320 kbit/s what it changes is not audible, and the encoder shares the
/// Pi's cores with Mixxx for up to three hours.
constexpr int kLameQuality = 5;

/// Room for what LAME gives back from one block, from a flush, or a tag: its
/// own rule is 1.25 x the frames + 7200 bytes.
constexpr int kOutBytes = 16384;
static_assert(kOutBytes > InputCapture::kBlockFrames * 5 / 4 + 7200);

#ifdef __linux__
/// What the card holds while a block is encoded and written: two seconds, so
/// a card or a CPU slow for a moment costs nothing.
constexpr snd_pcm_uframes_t kBufferFrames = InputCapture::kRate * 2;
/// How long a wait for the input lasts before a stop is looked for again,
/// and how long an input may give nothing at all before the recording ends:
/// a card that stalls must not hold the recording, or Mixxx's exit, for ever.
constexpr int kWaitMs = 500;
constexpr int kStalledMs = 5000;

QString alsaError(int err) {
    return QString::fromLocal8Bit(snd_strerror(err));
}

/// The card's capture side, set as the deck records it; null, and why not.
snd_pcm_t* openCapture(QString* pError) {
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, InputCapture::kDevice, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        *pError = QObject::tr("no input on the sound card (%1)").arg(alsaError(err));
        return nullptr;
    }
    snd_pcm_hw_params_t* pHw = nullptr;
    snd_pcm_hw_params_alloca(&pHw);
    snd_pcm_uframes_t period = InputCapture::kBlockFrames;
    snd_pcm_uframes_t buffer = kBufferFrames;
    if ((err = snd_pcm_hw_params_any(pcm, pHw)) < 0 ||
            (err = snd_pcm_hw_params_set_access(pcm, pHw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
            (err = snd_pcm_hw_params_set_format(pcm, pHw, SND_PCM_FORMAT_S16_LE)) < 0 ||
            (err = snd_pcm_hw_params_set_channels(pcm, pHw, InputCapture::kChannels)) < 0 ||
            (err = snd_pcm_hw_params_set_rate(pcm, pHw, InputCapture::kRate, 0)) < 0 ||
            (err = snd_pcm_hw_params_set_period_size_near(pcm, pHw, &period, nullptr)) < 0 ||
            (err = snd_pcm_hw_params_set_buffer_size_near(pcm, pHw, &buffer)) < 0 ||
            (err = snd_pcm_hw_params(pcm, pHw)) < 0) {
        *pError = QObject::tr("the sound card's input will not take 16-bit stereo at 44.1 kHz (%1)")
                          .arg(alsaError(err));
        snd_pcm_close(pcm);
        return nullptr;
    }
    kLogger.info() << "recording" << InputCapture::kDevice << ": blocks of" << period
                   << "frames, a buffer of" << buffer;
    return pcm;
}
#endif

/// What a failed write to the card means to the DJ.
QString writeError(const QFile& file) {
    if (file.error() == QFileDevice::ResourceError) {
        return QObject::tr("the card is full");
    }
    return QObject::tr("could not write to the card: %1").arg(file.errorString());
}

} // namespace

InputCapture::InputCapture(QObject* pParent)
        : QObject(pParent) {
    qRegisterMetaType<mixxx::deck::InputCapture::End>();
}

InputCapture::~InputCapture() = default;

void InputCapture::run(const QString& path, const QString& title, qint64 maxFrames) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        emit finished(0,
                End::Failed,
                tr("could not make the recording on the card: %1").arg(file.errorString()));
        return;
    }
#ifdef __linux__
    QString error;
    snd_pcm_t* pcm = openCapture(&error);
    if (!pcm) {
        file.remove();
        emit finished(0, End::Failed, error);
        return;
    }

    // MP3 at a constant rate, so what a recording takes is known before it
    // starts; and the Info frame LAME keeps the first frame for, which
    // players read the exact length from, written at the end.
    lame_global_flags* pLame = lame_init();
    lame_set_in_samplerate(pLame, kRate);
    lame_set_out_samplerate(pLame, kRate);
    lame_set_num_channels(pLame, kChannels);
    lame_set_mode(pLame, JOINT_STEREO);
    lame_set_VBR(pLame, vbr_off);
    lame_set_brate(pLame, setrecording::kBitrateKbps);
    lame_set_quality(pLame, kLameQuality);
    lame_set_bWriteVbrTag(pLame, 1);
    // The title, as an ID3v2 tag written here rather than by LAME, so the
    // Info frame's place after it is known.
    lame_set_write_id3tag_automatic(pLame, 0);
    id3tag_init(pLame);
    id3tag_v2_only(pLame);
    const QByteArray titleLatin1 = title.toLatin1();
    id3tag_set_title(pLame, titleLatin1.constData());

    std::vector<unsigned char> out(kOutBytes);
    qint64 infoAt = 0;
    End end = End::Asked;
    qint64 frames = 0;
    bool fileOk = true;
    const bool encoding = lame_init_params(pLame) >= 0;
    if (!encoding) {
        error = tr("the MP3 encoder would not start");
        end = End::Failed;
    } else {
        const size_t tagBytes = lame_get_id3v2_tag(pLame, out.data(), out.size());
        if (tagBytes > 0 && tagBytes <= out.size()) {
            const auto written = file.write(reinterpret_cast<const char*>(out.data()),
                    static_cast<qint64>(tagBytes));
            if (written != static_cast<qint64>(tagBytes)) {
                error = writeError(file);
                end = End::Failed;
                fileOk = false;
            }
            infoAt = static_cast<qint64>(tagBytes);
        }
    }

    std::vector<short> block(static_cast<size_t>(kBlockFrames) * kChannels);
    // Started now rather than by the first read, so there is something for
    // the bounded wait below to wait for.
    snd_pcm_start(pcm);
    int stalledMs = 0;
    while (end != End::Failed && !m_stop.load()) {
        const qint64 want = std::min<qint64>(kBlockFrames, maxFrames - frames);
        if (want <= 0) {
            end = End::Limit;
            break;
        }
        const int ready = snd_pcm_wait(pcm, kWaitMs);
        if (ready == 0) {
            stalledMs += kWaitMs;
            if (stalledMs >= kStalledMs) {
                error = tr("the sound card's input gave nothing for %1 s").arg(kStalledMs / 1000);
                end = End::Failed;
            }
            continue;
        }
        stalledMs = 0;
        const snd_pcm_sframes_t got =
                ready < 0 ? ready : snd_pcm_readi(pcm, block.data(), want);
        if (got < 0) {
            // An overrun loses what the card could not hold; a card that went
            // (unplugged, ENODEV) ends the recording here.
            if (got == -EPIPE) {
                kLogger.warning() << "the input overran: part of the set is lost";
            }
            const int err = snd_pcm_recover(pcm, static_cast<int>(got), 1);
            if (err < 0) {
                error = tr("the sound card's input stopped (%1)").arg(alsaError(err));
                end = End::Failed;
            } else {
                snd_pcm_start(pcm); // recovered prepared, not running
            }
            continue;
        }
        int peakLeft = 0;
        int peakRight = 0;
        for (snd_pcm_sframes_t i = 0; i < got; ++i) {
            peakLeft = std::max(peakLeft, std::abs(static_cast<int>(block[2 * i])));
            peakRight = std::max(peakRight, std::abs(static_cast<int>(block[2 * i + 1])));
        }
        const int bytes = lame_encode_buffer_interleaved(pLame,
                block.data(),
                static_cast<int>(got),
                out.data(),
                static_cast<int>(out.size()));
        if (bytes < 0) {
            error = tr("the MP3 encoder failed (%1)").arg(bytes);
            end = End::Failed;
            break;
        }
        if (bytes > 0 && file.write(reinterpret_cast<const char*>(out.data()), bytes) != bytes) {
            error = writeError(file);
            end = End::Failed;
            fileOk = false;
            break;
        }
        frames += got;
        m_frames.store(frames);
        emit levels(static_cast<float>(peakLeft) / 32768.0f,
                static_cast<float>(peakRight) / 32768.0f,
                frames);
    }
    snd_pcm_close(pcm);

    // What LAME still holds, then the Info frame where it kept its place: the
    // file is whole however the recording ended, unless the card refused it.
    if (encoding && fileOk) {
        const int last = lame_encode_flush(pLame, out.data(), static_cast<int>(out.size()));
        if (last > 0) {
            file.write(reinterpret_cast<const char*>(out.data()), last);
        }
        const size_t info = lame_get_lametag_frame(pLame, out.data(), out.size());
        if (info > 0 && info <= out.size() && file.seek(infoAt)) {
            file.write(reinterpret_cast<const char*>(out.data()), static_cast<qint64>(info));
        }
    }
    lame_close(pLame);
    file.close();
    kLogger.info() << "recorded" << frames << "frames into" << path
                   << (error.isEmpty() ? QString() : QStringLiteral(": ") + error);
    emit finished(frames, end, error);
#else
    Q_UNUSED(title);
    Q_UNUSED(maxFrames);
    file.remove();
    emit finished(0, End::Failed, tr("no input to record on this machine"));
#endif
}

} // namespace deck
} // namespace mixxx
