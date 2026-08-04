#include "network/prolink/prolinkanlz.h"

#include "prolink-cxx/src/lib.rs.h"

namespace {
QString toQString(const ::rust::String& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
} // namespace

namespace mixxx {
namespace prolink {

AnlzContents readAnlz(const QString& path) {
    AnlzContents out;
    if (path.isEmpty()) {
        out.error = QStringLiteral("no analysis path");
        return out;
    }

    const QByteArray utf8 = path.toUtf8();
    const ::prolink::AnlzContents parsed =
            ::prolink::read_anlz(::rust::Str(utf8.constData(), utf8.size()));
    out.ok = parsed.ok;
    out.error = toQString(parsed.error);

    out.beats.reserve(static_cast<qsizetype>(parsed.beats.size()));
    for (const ::prolink::AnlzBeat& beat : parsed.beats) {
        AnlzBeat converted;
        converted.beatNumber = beat.beat_number;
        converted.tempo = beat.tempo;
        converted.timeMs = beat.time_ms;
        out.beats.append(converted);
    }

    out.cues.reserve(static_cast<qsizetype>(parsed.cues.size()));
    for (const ::prolink::AnlzCue& cue : parsed.cues) {
        AnlzCue converted;
        converted.extended = cue.extended;
        converted.hotList = cue.hot_list;
        converted.hotCue = cue.hot_cue;
        converted.isLoop = cue.is_loop;
        converted.timeMs = cue.time_ms;
        converted.loopTimeMs = cue.loop_time_ms;
        converted.comment = toQString(cue.comment);
        converted.colorId = cue.color_id;
        converted.colorRed = cue.color_red;
        converted.colorGreen = cue.color_green;
        converted.colorBlue = cue.color_blue;
        out.cues.append(converted);
    }

    out.colourDetail.reserve(static_cast<qsizetype>(parsed.colour_detail.size()));
    for (const ::prolink::AnlzColourColumn& column : parsed.colour_detail) {
        AnlzColourColumn converted;
        converted.bass = column.bass;
        converted.mid = column.mid;
        converted.treble = column.treble;
        converted.height = column.height;
        out.colourDetail.append(converted);
    }

    out.preview = QByteArray(reinterpret_cast<const char*>(parsed.preview.data()),
            static_cast<qsizetype>(parsed.preview.size()));
    return out;
}

} // namespace prolink
} // namespace mixxx
