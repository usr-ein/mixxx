#include "sources/soundsourceprolink.h"

#include <utility>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("SoundSourceProLink");

/// What FFmpeg reads through in one call. Big enough that a track is not a
/// storm of tiny reads, small enough that one blocked read does not wait on a
/// megabyte that is not needed yet.
constexpr int kAvioBufferSize = 64 * 1024;
} // namespace

namespace mixxx {

/*static*/ const QString SoundSourceProviderProLink::kDisplayName =
        QStringLiteral("Pro DJ Link streaming");

SoundSourceProLink::SoundSourceProLink(const QUrl& url)
        : SoundSourceFFmpeg(url) {
    m_pStream = mixxx::deck::StreamingFileRegistry::lookup(getLocalFileName());
}

SoundSourceProLink::~SoundSourceProLink() {
    close();
}

void SoundSourceProLink::close() {
    SoundSourceFFmpeg::close();
    if (m_pAvioContext != nullptr) {
        // The buffer may have been reallocated by FFmpeg, so free the one the
        // context is actually holding rather than the one we handed it.
        av_freep(&m_pAvioContext->buffer);
        avio_context_free(&m_pAvioContext);
        m_pAvioContext = nullptr;
    }
    // **The stream is not part of the open state and must survive this.**
    //
    // AudioSource::open() begins with `close(); // reopening is not supported`.
    // So a close() that let go of the stream ran before every single open, and
    // createAvioContext() then found nothing to read from and returned null --
    // which means "open the path normally". FFmpeg duly opened the sparse file
    // and read the holes as zeros.
    //
    // Nothing failed. It looked like it worked, because by the time anything
    // opened the file enough of it had usually landed for the decoder to find
    // a stream; the whole streaming path was decorative and never once ran.
    // What exposed it was making the load *fast*: with the 2.2 s tag scan gone,
    // the decoder reached the file 17 ms after the transfer began, read a
    // megabyte of zeros, and reported no audio stream.
}

std::pair<SoundSourceProLink::ImportResult, QDateTime>
SoundSourceProLink::importTrackMetadataAndCoverImage(
        TrackMetadata* pTrackMetadata,
        QImage* pCoverImage,
        bool resetMissingTagMetadata) const {
    Q_UNUSED(pTrackMetadata)
    Q_UNUSED(pCoverImage)
    Q_UNUSED(resetMissingTagMetadata)
    return std::make_pair(ImportResult::Unavailable, QDateTime());
}

int SoundSourceProLink::readPacket(void* pOpaque, uint8_t* pBuffer, int size) {
    auto* pSelf = static_cast<SoundSourceProLink*>(pOpaque);
    if (!pSelf || !pSelf->m_pStream) {
        return AVERROR(EIO);
    }
    // Blocks if the bytes are not here yet. That is the entire point: the
    // alternative reads zeros out of a sparse hole and plays silence.
    const qint64 read = pSelf->m_pStream->read(
            pSelf->m_position, reinterpret_cast<char*>(pBuffer), size);
    if (read < 0) {
        kLogger.warning() << "read failed at" << pSelf->m_position
                          << "--" << pSelf->m_pStream->error();
        return AVERROR(EIO);
    }
    if (read == 0) {
        return AVERROR_EOF;
    }
    pSelf->m_position += read;
    return static_cast<int>(read);
}

int64_t SoundSourceProLink::seek(void* pOpaque, int64_t offset, int whence) {
    auto* pSelf = static_cast<SoundSourceProLink*>(pOpaque);
    if (!pSelf || !pSelf->m_pStream) {
        return AVERROR(EIO);
    }
    const int64_t size = pSelf->m_pStream->size();

    // AVSEEK_SIZE is a question, not a seek: FFmpeg asks how long the file is.
    // Answering it correctly is what gives the track its true duration while
    // the bytes are still arriving -- a source that returns an error here is
    // treated as unseekable, and the deck gets a track it cannot scrub.
    if (whence == AVSEEK_SIZE) {
        return size;
    }

    // AVSEEK_FORCE is a *flag*, not a whence, and FFmpeg ORs it in freely --
    // "seek even if you think it is expensive". Treating the combination as an
    // unknown whence and refusing it is what made every remote track fail at
    // `av_seek_frame() failed: Operation not permitted` after the stream had
    // been parsed perfectly.
    const int mode = whence & ~AVSEEK_FORCE;

    int64_t target = offset;
    switch (mode) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        target = pSelf->m_position + offset;
        break;
    case SEEK_END:
        target = size + offset;
        break;
    default:
        kLogger.warning() << "unknown seek whence" << whence;
        return AVERROR(EINVAL);
    }
    if (target < 0 || target > size) {
        kLogger.warning() << "seek out of range:" << target << "of" << size;
        return AVERROR(EINVAL);
    }
    pSelf->m_position = target;
    return target;
}

AVIOContext* SoundSourceProLink::createAvioContext() {
    if (!m_pStream) {
        // Not one of ours after all -- the transfer finished and the file was
        // unregistered between this object being made and being opened. The
        // path is now an ordinary complete file, so opening it normally is the
        // right answer rather than a failure.
        //
        // Said out loud, because this is also what a bug here looks like: it is
        // the difference between decoding the bytes as they arrive and decoding
        // whatever the sparse file happens to hold, and the second one does not
        // report an error.
        kLogger.info() << "not streaming" << getLocalFileName()
                       << "-- reading it as an ordinary file";
        return nullptr;
    }
    auto* pBuffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (pBuffer == nullptr) {
        kLogger.warning() << "could not allocate an AVIO buffer";
        return nullptr;
    }
    m_position = 0;
    m_pAvioContext = avio_alloc_context(pBuffer,
            kAvioBufferSize,
            0, // read only
            this,
            &SoundSourceProLink::readPacket,
            nullptr,
            &SoundSourceProLink::seek);
    if (m_pAvioContext == nullptr) {
        av_free(pBuffer);
        kLogger.warning() << "could not allocate an AVIO context";
        return nullptr;
    }
    kLogger.info() << "decoding" << getLocalFileName() << "as it arrives --"
                   << m_pStream->size() << "bytes";
    return m_pAvioContext;
}

QStringList SoundSourceProviderProLink::getSupportedFileTypes() const {
    // The containers a rekordbox medium actually carries. Claiming a type here
    // costs nothing for ordinary files, because newSoundSource() declines them.
    return QStringList{
            QStringLiteral("mp3"),
            QStringLiteral("m4a"),
            QStringLiteral("mp4"),
            QStringLiteral("aac"),
            QStringLiteral("flac"),
            QStringLiteral("wav"),
            QStringLiteral("aiff"),
            QStringLiteral("aif"),
    };
}

SoundSourceProviderPriority SoundSourceProviderProLink::getPriorityHint(
        const QString& supportedFileType) const {
    Q_UNUSED(supportedFileType);
    // Ahead of the ordinary decoders, so a file that IS being streamed is
    // caught here first. Everything else falls straight through.
    return SoundSourceProviderPriority::Higher;
}

SoundSourcePointer SoundSourceProviderProLink::newSoundSource(const QUrl& url) {
    const auto pStream = mixxx::deck::StreamingFileRegistry::lookup(url.toLocalFile());
    if (!pStream) {
        // An ordinary file. Declining is how SoundSourceProxy is told to try
        // the next provider, and it is the common case by far -- every track on
        // a stick comes through here and goes straight past.
        return SoundSourcePointer();
    }
    kLogger.info() << "claiming" << url.toLocalFile() << "-- still streaming";
    return newSoundSourceFromUrl<SoundSourceProLink>(url);
}

} // namespace mixxx
