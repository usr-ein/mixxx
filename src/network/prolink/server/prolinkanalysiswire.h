#pragma once

#include <QByteArray>

namespace mixxx {
namespace prolink {
namespace server {

/// Turning rekordbox's ANLZ tags into the shapes dbserver puts on the wire.
///
/// The obvious guess — that a server hands a player the analysis bytes
/// rekordbox wrote, unaltered — is wrong. A real CDJ serving another CDJ
/// **transforms** every one of them, and the transformations are not cosmetic:
/// the file is big-endian and the wire is little-endian, and three of the five
/// change the layout as well.
///
/// Every rule here was derived by diffing a real load — deck A loading a track
/// from deck B, in `prolinks-compat/captures/S06-load-and-play` — against that
/// track's own `ANLZ0000.DAT`/`.EXT` on the medium that was in deck B. Having
/// both halves is what makes these confirmed rather than guessed.
///
/// A missing or unparseable tag yields an empty blob rather than an error: a
/// track analysed by an older rekordbox legitimately lacks the newer tags, and a
/// missing waveform should cost the waveform, not the load.
///
/// **Parsing is the vendored Kaitai reader** (`lib/rekordbox-metadata/`), the
/// same one the Rekordbox feature uses to consume these files. Only the wire
/// encoding below is hand-written, because Kaitai emits no C++ serializers.
namespace analysiswire {

/// `0x2504` → `0x4502`: the MP3 variable-bitrate seek index.
///
/// The `PVBR` payload with every 32-bit word byte-swapped; nothing else changes
/// and the length is a fixed 1604 bytes.
///
/// Probably the request that gates playback: without a table mapping playing
/// time to byte offset a player cannot seek within a VBR MP3, so it has no way
/// to begin streaming. Erroring on this is where our track loads stopped.
QByteArray vbrIndex(const QByteArray& dat);

/// `0x2204` → `0x4602`: the beat grid.
///
/// A 20-byte little-endian prefix, then one 16-byte entry per beat. The file
/// stores 8-byte big-endian entries — beat number u2, tempo u2, time u4 — and
/// the wire keeps the same three fields little-endian, then pads each entry to
/// 16 with eight `0xff` bytes.
QByteArray beatGrid(const QByteArray& dat);

/// `0x2004` → `0x4402`: the preview waveform, plus the tiny one.
///
/// The file packs each of the 400 columns into one byte: the low five bits are
/// the bar height, the top three a "whiteness" used for shading. The wire
/// unpacks that into two bytes per column, height first — 800 bytes — and then
/// appends the 100-byte `PWV2` tiny waveform verbatim, for 900 in all. That
/// trailing 100 bytes is why a plausible "widen each byte" implementation still
/// comes out the wrong length.
QByteArray waveformPreview(const QByteArray& dat);

/// `0x2904` → `0x4a02`: the scrolling waveform.
///
/// A 20-byte little-endian prefix and then the `PWV3` payload **verbatim** — the
/// one analysis blob the wire does not reorder, because its entries are single
/// bytes and so have no byte order to get wrong.
QByteArray waveformDetail(const QByteArray& ext);

/// Bytes per cue record in the first `CUE_POINTS` blob.
constexpr int kCueEntrySize = 0x24;

/// `0x2104` → `0x4702`: memory points and hot cues.
///
/// The one reply carrying two blobs. *pRecords* gets `*pCount` records of
/// kCueEntrySize bytes; *pTimes* gets one little-endian `(time, loop time)` pair
/// per cue. Cues go out **sorted by time**, not in the order the file stores
/// them — rekordbox writes them newest-first.
void cuePoints(const QByteArray& dat,
        QByteArray* pRecords,
        int* pCount,
        QByteArray* pTimes);

/// The fifth prefix word of `BEAT_GRID` and `WAVEFORM_DETAIL`.
///
/// We cannot derive it. What is known: the two observed values, `0x06114a48` and
/// `0x0612e0b4`, are for the **same track in the same load**, so it is not a
/// property of the content — it is per reply. They are 2.58 s apart and differ
/// by 104,044, about 40,000 per second, which makes it a free-running counter or
/// an allocator address on the serving deck. Either way a client cannot validate
/// it, so this emits a counter of the same shape.
///
/// **Confirmed on hardware**: with zero here the main waveform does not draw,
/// and with a deck-shaped counter it draws cleanly, everything else unchanged.
/// That is worth stating plainly because the reasoning that predicted otherwise
/// was wrong — "a value the client cannot recompute is a value it cannot check,
/// therefore it is ignored". A receiver does not have to *validate* a field to
/// *reject* it: zero is a perfectly good sentinel for "absent", and evidently
/// that is how it reads. All we know is that it must be non-zero and must not go
/// backwards.
quint32 prefixOpaque();

} // namespace analysiswire
} // namespace server
} // namespace prolink
} // namespace mixxx
