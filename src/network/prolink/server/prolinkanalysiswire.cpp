#include "network/prolink/server/prolinkanalysiswire.h"

#include <kaitai/exceptions.h>
#include <kaitai/kaitaistream.h>

#include <QElapsedTimer>
#include <QHash>
#include <algorithm>
#include <exception>
#include <sstream>
#include <vector>

#include "rekordbox_anlz.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("ProLinkAnalysisWire");

/// The fixed part of a tagged section's header: fourcc, header length, tag
/// length. Kaitai hands us everything after these three words as the body, so a
/// tag's *payload* starts `len_header - 12` bytes into it.
constexpr uint32_t kTagFixedHeader = 12;

/// One parsed ANLZ container, with the payloads addressable by tag.
///
/// Held as plain byte arrays rather than as the Kaitai objects, because the
/// callers below all want raw bytes and the parse tree would otherwise have to
/// outlive the call.
struct AnlzTags {
    QHash<int, QByteArray> payloads;
    /// The first word after the fixed header, per tag. `waveformDetail` needs
    /// it: the entry width lives in the tag's extended header, not its payload.
    QHash<int, quint32> firstHeaderWord;

    QByteArray payload(rekordbox_anlz_t::section_tags_t tag) const {
        return payloads.value(static_cast<int>(tag));
    }
    bool has(rekordbox_anlz_t::section_tags_t tag) const {
        return payloads.contains(static_cast<int>(tag));
    }
};

quint32 readU32Be(const QByteArray& data, int offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return (static_cast<quint32>(static_cast<quint8>(data.at(offset))) << 24) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 8) |
            static_cast<quint32>(static_cast<quint8>(data.at(offset + 3)));
}

quint16 readU16Be(const QByteArray& data, int offset) {
    if (offset + 2 > data.size()) {
        return 0;
    }
    return static_cast<quint16>(
            (static_cast<quint8>(data.at(offset)) << 8) |
            static_cast<quint8>(data.at(offset + 1)));
}

void appendU32Le(QByteArray* pOut, quint32 value) {
    for (int shift = 0; shift < 32; shift += 8) {
        pOut->append(static_cast<char>((value >> shift) & 0xFF));
    }
}

void appendU16Le(QByteArray* pOut, quint16 value) {
    pOut->append(static_cast<char>(value & 0xFF));
    pOut->append(static_cast<char>((value >> 8) & 0xFF));
}

AnlzTags parseAnlz(const QByteArray& data) {
    AnlzTags tags;
    if (data.isEmpty()) {
        return tags;
    }
    try {
        const std::string buffer(data.constData(), static_cast<size_t>(data.size()));
        kaitai::kstream stream(buffer);
        rekordbox_anlz_t file(&stream);
        for (const auto& section : *file.sections()) {
            const std::string& body = section->_raw_body();
            const QByteArray raw(body.data(), static_cast<int>(body.size()));
            if (section->len_header() < kTagFixedHeader) {
                continue;
            }
            const int skip = static_cast<int>(section->len_header() - kTagFixedHeader);
            if (skip > raw.size()) {
                continue;
            }
            const int key = static_cast<int>(section->fourcc());
            // A container legitimately repeats a fourcc (two PCOB tags, memory
            // points and hot cues); keep the first, which is what the reference
            // capture's replies carry.
            if (!tags.payloads.contains(key)) {
                tags.payloads.insert(key, raw.mid(skip));
                tags.firstHeaderWord.insert(key, readU32Be(raw, 0));
            }
        }
    } catch (const std::exception& e) {
        kLogger.debug() << "could not parse ANLZ:" << e.what();
    } catch (...) {
        kLogger.debug() << "could not parse ANLZ";
    }
    return tags;
}

/// A run of big-endian 32-bit words as little-endian ones. Any trailing bytes
/// that do not fill a word are passed through, since there is no word to swap.
QByteArray swapWords(const QByteArray& payload) {
    QByteArray out;
    out.reserve(payload.size());
    const int whole = payload.size() - payload.size() % 4;
    for (int i = 0; i < whole; i += 4) {
        out.append(payload.at(i + 3));
        out.append(payload.at(i + 2));
        out.append(payload.at(i + 1));
        out.append(payload.at(i));
    }
    out.append(payload.mid(whole));
    return out;
}

/// Waveform frames per second. The detail waveform is drawn at 150 columns per
/// second, and a cue's position travels as a *frame index* rather than a time so
/// the player can place the marker without doing the arithmetic itself.
constexpr int kWaveformFps = 150;

/// A cue time in waveform frames, **truncated** as the hardware truncates: 271 ms
/// becomes 40, not 41. Confirmed against all three cues in the reference
/// capture, which is what rules out rounding.
quint32 frameOf(quint32 milliseconds) {
    return static_cast<quint32>(
            static_cast<quint64>(milliseconds) * kWaveformFps / 1000);
}

struct Cue {
    quint16 order = 0;
    quint32 hotCue = 0;
    quint32 time = 0;
    quint32 loopTime = 0;
};

/// Walk the `PCPT` sub-entries of a `PCOB` payload.
///
/// A malformed or truncated entry ends the walk rather than raising: a bad cue
/// should cost the cue, not the load.
std::vector<Cue> iterateCues(const QByteArray& payload) {
    std::vector<Cue> cues;
    int offset = 0;
    while (offset + 40 <= payload.size()) {
        if (payload.mid(offset, 4) != QByteArrayLiteral("PCPT")) {
            break;
        }
        const quint32 entryLength = readU32Be(payload, offset + 8);
        if (entryLength < 40 ||
                static_cast<qint64>(offset) + entryLength > payload.size()) {
            break;
        }
        Cue cue;
        cue.hotCue = readU32Be(payload, offset + 12);
        cue.order = readU16Be(payload, offset + 28);
        cue.time = readU32Be(payload, offset + 32);
        cue.loopTime = readU32Be(payload, offset + 36);
        cues.push_back(cue);
        offset += static_cast<int>(entryLength);
    }
    return cues;
}

} // namespace

namespace mixxx {
namespace prolink {
namespace server {
namespace analysiswire {

quint32 prefixOpaque() {
    constexpr qint64 kBase = 0x06000000;
    constexpr qint64 kRatePerSecond = 40000;
    // Saturating rather than wrapping. The only two properties we know this
    // field must have are non-zero and non-decreasing, and a modulo would break
    // the second one every 419 seconds -- well inside a set. At this rate the
    // ceiling is a little over 29 hours of uptime, by which point a stuck value
    // is the least of it.
    constexpr qint64 kCeiling = 0xFFFFFFFFll;
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
    }
    return static_cast<quint32>(
            qMin(kCeiling, kBase + timer.elapsed() * kRatePerSecond / 1000));
}

QByteArray vbrIndex(const QByteArray& dat) {
    const AnlzTags tags = parseAnlz(dat);
    return swapWords(tags.payload(rekordbox_anlz_t::SECTION_TAGS_VBR));
}

QByteArray beatGrid(const QByteArray& dat) {
    const AnlzTags tags = parseAnlz(dat);
    if (!tags.has(rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID)) {
        return QByteArray();
    }
    const QByteArray payload = tags.payload(rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID);
    const int count = payload.size() / 8;

    QByteArray entries;
    entries.reserve(count * 16);
    for (int i = 0; i < count; ++i) {
        appendU16Le(&entries, readU16Be(payload, 8 * i));     // beat number
        appendU16Le(&entries, readU16Be(payload, 8 * i + 2)); // tempo, centi-BPM
        appendU32Le(&entries, readU32Be(payload, 8 * i + 4)); // time, ms
        entries.append(8, static_cast<char>(0xFF));
    }

    QByteArray out;
    // Word 0 is the tag's own constant, word 2 the entry-block length.
    appendU32Le(&out, 0x80000);
    appendU32Le(&out, static_cast<quint32>(count));
    appendU32Le(&out, static_cast<quint32>(entries.size()));
    appendU32Le(&out, 1);
    appendU32Le(&out, prefixOpaque());
    return out + entries;
}

QByteArray waveformPreview(const QByteArray& dat) {
    const AnlzTags tags = parseAnlz(dat);
    const QByteArray preview = tags.payload(rekordbox_anlz_t::SECTION_TAGS_WAVE_PREVIEW);
    if (preview.isEmpty()) {
        return QByteArray();
    }
    QByteArray out;
    out.reserve(preview.size() * 2 + 100);
    for (const char byte : preview) {
        const quint8 packed = static_cast<quint8>(byte);
        out.append(static_cast<char>(packed & 0x1F)); // height
        out.append(static_cast<char>(packed >> 5));   // whiteness
    }
    return out + tags.payload(rekordbox_anlz_t::SECTION_TAGS_WAVE_TINY);
}

QByteArray waveformDetail(const QByteArray& ext) {
    const AnlzTags tags = parseAnlz(ext);
    if (!tags.has(rekordbox_anlz_t::SECTION_TAGS_WAVE_SCROLL)) {
        return QByteArray();
    }
    const QByteArray payload = tags.payload(rekordbox_anlz_t::SECTION_TAGS_WAVE_SCROLL);
    // The tag's extended header carries (entry width, entry count, constant);
    // the wire wants the count first. 0x96 is the high half of that constant.
    const quint32 width = qMax(
            1u, tags.firstHeaderWord.value(
                        static_cast<int>(rekordbox_anlz_t::SECTION_TAGS_WAVE_SCROLL)));
    QByteArray out;
    appendU32Le(&out, static_cast<quint32>(payload.size()));
    appendU32Le(&out, width);
    appendU32Le(&out, static_cast<quint32>(payload.size()));
    appendU32Le(&out, 0x96);
    appendU32Le(&out, prefixOpaque());
    return out + payload;
}

void cuePoints(const QByteArray& dat,
        QByteArray* pRecords,
        int* pCount,
        QByteArray* pTimes) {
    if (pRecords) {
        pRecords->clear();
    }
    if (pTimes) {
        pTimes->clear();
    }
    if (pCount) {
        *pCount = 0;
    }
    const AnlzTags tags = parseAnlz(dat);
    if (!tags.has(rekordbox_anlz_t::SECTION_TAGS_CUES)) {
        return;
    }
    std::vector<Cue> cues = iterateCues(tags.payload(rekordbox_anlz_t::SECTION_TAGS_CUES));
    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) {
        return a.time < b.time;
    });

    for (const Cue& cue : cues) {
        if (pRecords) {
            appendU16Le(pRecords, cue.order);
            appendU16Le(pRecords, static_cast<quint16>(cue.hotCue));
            appendU32Le(pRecords, 0);
            appendU32Le(pRecords, 0);
            appendU32Le(pRecords, frameOf(cue.time));
            pRecords->append(kCueEntrySize - 16, '\0');
        }
        if (pTimes) {
            appendU32Le(pTimes, cue.time);
            appendU32Le(pTimes, cue.loopTime);
        }
    }
    if (pCount) {
        *pCount = static_cast<int>(cues.size());
    }
}

} // namespace analysiswire
} // namespace server
} // namespace prolink
} // namespace mixxx
