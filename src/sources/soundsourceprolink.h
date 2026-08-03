#pragma once

#include <memory>

#include "library/deck/streamingfile.h"
#include "sources/soundsourceffmpeg.h"

namespace mixxx {

/// Decode a track that is still arriving over Pro DJ Link.
///
/// The only difference from the ordinary FFmpeg source is where the bytes come
/// from: an AVIOContext whose read and seek are served by a StreamingFile,
/// which **blocks** on a range that has not arrived rather than handing back
/// the zeros a sparse file would. Everything after the open -- probing, stream
/// selection, decoding, resampling -- is the base class's, unchanged.
///
/// That is the whole reason to reuse SoundSourceFFmpeg rather than write a
/// decoder: MP3, AAC, FLAC, WAV and AIFF all work the moment the bytes do.
class SoundSourceProLink : public SoundSourceFFmpeg {
  public:
    explicit SoundSourceProLink(const QUrl& url);
    ~SoundSourceProLink() override;

    void close() override;

  protected:
    AVIOContext* createAvioContext() override;

  private:
    static int readPacket(void* pOpaque, uint8_t* pBuffer, int size);
    static int64_t seek(void* pOpaque, int64_t offset, int whence);

    std::shared_ptr<mixxx::deck::StreamingFile> m_pStream;
    AVIOContext* m_pAvioContext = nullptr;
    /// Where the decoder has got to. FFmpeg's callbacks are a stream, not a
    /// random-access file, so the position has to be kept here.
    int64_t m_position = 0;
};

/// Claims the same file types as the ordinary decoders, and declines anything
/// that is not currently being streamed.
///
/// Declining is the important half: `newSoundSource` returns null for an
/// ordinary file, and SoundSourceProxy then falls through to the normal
/// providers. So a stick on the local filesystem never touches any of this.
class SoundSourceProviderProLink : public SoundSourceProvider {
  public:
    static const QString kDisplayName;

    ~SoundSourceProviderProLink() override = default;

    QString getDisplayName() const override {
        return kDisplayName;
    }

    QStringList getSupportedFileTypes() const override;

    SoundSourceProviderPriority getPriorityHint(
            const QString& supportedFileType) const override;

    SoundSourcePointer newSoundSource(const QUrl& url) override;
};

} // namespace mixxx
