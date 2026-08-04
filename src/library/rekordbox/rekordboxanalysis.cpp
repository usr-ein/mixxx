#include "library/rekordbox/rekordboxanalysis.h"

#include <mp3guessenc.h>

#include <QSet>
#include <QtDebug>

#include <algorithm>

#include "library/rekordbox/rekordboxconstants.h"
#include "library/rekordbox/rekordboxwaveform.h"
#include "network/prolink/prolinkanlz.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("RekordboxAnalysis");

enum class IDForColor : uint8_t {
    Pink = 1,
    Red,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple
};

constexpr mixxx::RgbColor kColorForIDPink(0xF870F8);
constexpr mixxx::RgbColor kColorForIDRed(0xF870900);
constexpr mixxx::RgbColor kColorForIDOrange(0xF8A030);
constexpr mixxx::RgbColor kColorForIDYellow(0xF8E331);
constexpr mixxx::RgbColor kColorForIDGreen(0x1EE000);
constexpr mixxx::RgbColor kColorForIDAqua(0x16C0F8);
constexpr mixxx::RgbColor kColorForIDBlue(0x0150F8);
constexpr mixxx::RgbColor kColorForIDPurple(0x9808F8);
constexpr mixxx::RgbColor kColorForIDNoColor(0x0);

mixxx::RgbColor colorFromIDLocal(int colorID) {
    switch (static_cast<IDForColor>(colorID)) {
    case IDForColor::Pink:
        return kColorForIDPink;
    case IDForColor::Red:
        return kColorForIDRed;
    case IDForColor::Orange:
        return kColorForIDOrange;
    case IDForColor::Yellow:
        return kColorForIDYellow;
    case IDForColor::Green:
        return kColorForIDGreen;
    case IDForColor::Aqua:
        return kColorForIDAqua;
    case IDForColor::Blue:
        return kColorForIDBlue;
    case IDForColor::Purple:
        return kColorForIDPurple;
    }
    return kColorForIDNoColor;
}

struct memory_cue_loop_t {
    mixxx::audio::FramePos startPosition;
    mixxx::audio::FramePos endPosition;
    QString comment;
    mixxx::RgbColor::optional_t color;
};


void setHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;
    const QList<CuePointer> cuePoints = track->getCuePoints();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getHotCue() == id) {
            pCue = trackCue;
            break;
        }
    }

    mixxx::CueType type = mixxx::CueType::HotCue;
    if (endPosition.isValid()) {
        type = mixxx::CueType::Loop;
    }

    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type,
                id,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

/// Milliseconds from the file, as a position in the track's frames.
///
/// The offset compensates for MP3 decoders disagreeing about where a file
/// starts; see timingOffsetForFile(). Clamped to 1 ms because a cue at frame
/// zero of a file whose decoder starts late lands before the audio does.
mixxx::audio::FramePos framePosOf(quint32 timeMs,
        int timingOffset,
        double sampleRateKhz) {
    const int time = std::max(1, static_cast<int>(timeMs) - timingOffset);
    return mixxx::audio::FramePos(sampleRateKhz * static_cast<double>(time));
}

/// Apply a `PQTZ` beat grid.
///
/// **Only ever from the `.DAT`.** Beat grids are correct there and not in the
/// `.EXT`, which is the one asymmetry between the two files that matters.
void applyBeatGrid(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        const mixxx::prolink::AnlzContents& contents) {
    if (contents.beats.isEmpty()) {
        return;
    }
    const double sampleRateKhz = sampleRate / 1000.0;

    QVector<mixxx::audio::FramePos> beats;
    beats.reserve(contents.beats.size());
    mixxx::audio::FramePos firstDownbeatPosition;
    mixxx::audio::FramePos firstBeatPosition;
    // Every beat carries the tempo rekordbox analysed it at. On a fixed-tempo
    // track they are all the same value, and that value is exact -- see below
    // for why it matters more than the positions.
    QSet<quint16> storedTempos;

    for (const mixxx::prolink::AnlzBeat& beat : contents.beats) {
        const auto position = framePosOf(beat.timeMs, timingOffset, sampleRateKhz);
        beats << position;
        if (beat.tempo > 0) {
            storedTempos.insert(beat.tempo);
        }
        if (!firstBeatPosition.isValid()) {
            firstBeatPosition = position;
        }

        // Rekordbox numbers every beat 1..4 within its bar. Mixxx has nowhere
        // to store that, but remembering the first downbeat is enough to
        // reconstruct the phase of the whole grid.
        if (beat.beatNumber == 1 && !firstDownbeatPosition.isValid()) {
            firstDownbeatPosition = position;
        }
    }

    // **A fixed-tempo track gets a fixed-tempo grid, from the stored tempo
    // rather than from the beat positions.**
    //
    // rekordbox writes beat times as whole milliseconds. At 144 BPM a beat is
    // 416.67 ms, so the stored gaps alternate 417, 417, 416 -- and a grid built
    // from those positions has a local tempo that swings by up to 0.45 BPM
    // depending on which pair of beats is being measured. That is what makes
    // the displayed BPM wander as a track plays: not the hardware, and not
    // rekordbox being clever, just millisecond rounding preserved into a
    // variable-tempo grid.
    //
    // The tempo field alongside each beat has no such problem: it is centi-BPM,
    // it is exact, and on all six real tracks checked it was identical for
    // every beat of the track. So when there is exactly one, use it and anchor
    // the grid at the first beat.
    mixxx::BeatsPointer pBeats;
    if (storedTempos.size() == 1 && firstBeatPosition.isValid()) {
        const double bpm = *storedTempos.constBegin() / 100.0;
        pBeats = mixxx::Beats::fromConstTempo(sampleRate,
                firstBeatPosition,
                mixxx::Bpm(bpm),
                mixxx::rekordboxconstants::beatsSubversion);
    } else {
        // Genuinely variable tempo -- a live recording, or a track gridded by
        // hand. The per-beat positions are all there is, and their millisecond
        // rounding is the least of the problem.
        pBeats = mixxx::Beats::fromBeatPositions(
                sampleRate,
                beats,
                mixxx::rekordboxconstants::beatsSubversion);
    }
    track->trySetBeats(pBeats);

    // The intro cue is what WaveformRenderBeat anchors its downbeat display to,
    // so seed it from the grid to get the same bar phase Rekordbox shows. Never
    // clobber an existing intro cue: the user (or AnalyzerSilence) may have
    // placed it deliberately.
    if (firstDownbeatPosition.isValid() &&
            !track->findCueByType(mixxx::CueType::Intro)) {
        track->createAndAddCue(mixxx::CueType::Intro,
                Cue::kNoHotCue,
                firstDownbeatPosition,
                mixxx::audio::kInvalidFramePos);
    }
}

/// Apply every cue and loop of both lists.
///
/// Hot cues go straight on; memory cues and loops are collected, sorted by
/// position, and applied afterwards, because the first of them chronologically
/// becomes Mixxx's main cue.
///
/// The plain and extended entries are applied in that order and not merged. A
/// `PCO2` entry for a hot cue the `PCOB` list also holds lands second and wins,
/// which is what carries its comment and its pad colour onto a cue that was
/// already placed.
void applyCues(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        const mixxx::prolink::AnlzContents& contents) {
    const double sampleRateKhz = sampleRate / 1000.0;
    QList<memory_cue_loop_t> memoryCuesAndLoops;
    int lastHotCueIndex = 0;

    for (const mixxx::prolink::AnlzCue& cue : contents.cues) {
        const auto position = framePosOf(cue.timeMs, timingOffset, sampleRateKhz);

        if (cue.hotList) {
            const int hotCueIndex = static_cast<int>(cue.hotCue) - 1;
            if (hotCueIndex > lastHotCueIndex) {
                lastHotCueIndex = hotCueIndex;
            }
            setHotCue(track,
                    position,
                    mixxx::audio::kInvalidFramePos,
                    hotCueIndex,
                    cue.comment,
                    cue.extended ? mixxx::RgbColor::optional(
                                           mixxx::RgbColor(qRgb(cue.colorRed,
                                                   cue.colorGreen,
                                                   cue.colorBlue)))
                                 : mixxx::RgbColor::nullopt());
            continue;
        }

        memory_cue_loop_t entry;
        entry.startPosition = position;
        entry.endPosition = cue.isLoop
                ? framePosOf(cue.loopTimeMs, timingOffset, sampleRateKhz)
                : mixxx::audio::kInvalidFramePos;
        entry.comment = cue.comment;
        entry.color = cue.extended
                ? mixxx::RgbColor::optional(
                          colorFromIDLocal(static_cast<int>(cue.colorId)))
                : mixxx::RgbColor::nullopt();
        memoryCuesAndLoops << entry;
    }

    if (memoryCuesAndLoops.isEmpty()) {
        return;
    }
    std::sort(memoryCuesAndLoops.begin(),
            memoryCuesAndLoops.end(),
            [](const memory_cue_loop_t& a, const memory_cue_loop_t& b) -> bool {
                return a.startPosition < b.startPosition;
            });

    bool mainCueFound = false;
    for (const memory_cue_loop_t& memoryCueOrLoop : memoryCuesAndLoops) {
        if (!mainCueFound && !memoryCueOrLoop.endPosition.isValid()) {
            // Set first chronological memory cue as Mixxx MainCue
            track->setMainCuePosition(memoryCueOrLoop.startPosition);
            CuePointer pMainCue = track->findCueByType(mixxx::CueType::MainCue);
            pMainCue->setLabel(memoryCueOrLoop.comment);
            // Guarded, unlike everything else about this optional. A plain
            // `PCOB` memory cue has no colour at all, and dereferencing the
            // empty optional it comes with is undefined behaviour rather than a
            // default colour -- reachable on any medium whose tracks have no
            // `.EXT`, which is what an older rekordbox writes.
            if (memoryCueOrLoop.color) {
                pMainCue->setColor(*memoryCueOrLoop.color);
            }
            mainCueFound = true;
        } else {
            // Mixxx v2.4 will feature multiple loops, so these saved here will
            // be usable. For 2.3, Mixxx treats them as hotcues and the first one
            // will be loaded as the single loop Mixxx supports.
            lastHotCueIndex++;
            setHotCue(
                    track,
                    memoryCueOrLoop.startPosition,
                    memoryCueOrLoop.endPosition,
                    lastHotCueIndex,
                    memoryCueOrLoop.comment,
                    memoryCueOrLoop.color);
        }
    }
}

} // namespace

namespace mixxx {
namespace rekordbox {

RgbColor colorFromID(int colorID) {
    return colorFromIDLocal(colorID);
}

int timingOffsetForFile(const QString& location) {
    // The following accounts for timing offsets required to correctly align
    // timing information (cue points, loops, beatgrids) exported from
    // Rekordbox. This is caused by different MP3 decoders treating MP3s encoded
    // in a variety of different cases differently. The mp3guessenc library is
    // used to determine which case the MP3 is classified in. See
    // https://github.com/mixxxdj/mixxx/pull/2119 for the detail.
    int timingOffset = 0;

    if (location.endsWith(QStringLiteral(".mp3"), Qt::CaseInsensitive)) {
        const int timingShiftCase =
                mp3guessenc_timing_shift_case(location.toStdString().c_str());
        qDebug() << "Timing shift case:" << timingShiftCase << "for MP3 file:" << location;

        switch (timingShiftCase) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            timingOffset = 12;
            break;
        case EXIT_CODE_CASE_B:
            timingOffset = 13;
            break;
        case EXIT_CODE_CASE_C:
            timingOffset = 26;
            break;
        case EXIT_CODE_CASE_D:
            timingOffset = 50;
            break;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#endif
        }
    }

#ifdef __COREAUDIO__
    if (location.toLower().endsWith(QStringLiteral(".m4a"))) {
        timingOffset = 48;
    }
#endif
    return timingOffset;
}

void applyAnalysis(TrackPointer track,
        const QString& anlzPath,
        AnalysisDao* pAnalysisDao,
        audio::SampleRate sampleRateOverride) {
    if (!track || anlzPath.isEmpty()) {
        return;
    }
    const QString location = track->getLocation();
    const int timingOffset = timingOffsetForFile(location);
    const audio::SampleRate sampleRate =
            sampleRateOverride.isValid() ? sampleRateOverride : track->getSampleRate();
    // **Everything below converts rekordbox's milliseconds into frames**, so
    // without a sample rate there is nothing to convert them to: the grid, the
    // cues and the waveform all come out empty and Mixxx analyses the track
    // from scratch as though no analysis had been found.
    //
    // Which is exactly what happened, invisibly, the moment the tag scan was
    // removed from the streaming path -- TagLib had been supplying the sample
    // rate as a side effect nobody had noticed depending on it. A silent
    // return would let that happen again.
    if (!sampleRate.isValid()) {
        kLogger.warning() << "no sample rate yet, so rekordbox's analysis of"
                          << location << "cannot be applied; open the audio "
                                         "source before calling this";
        return;
    }
    const QString anlzPathExt = anlzPath.left(anlzPath.length() - 3) + QStringLiteral("EXT");

    // Each file read once, here, rather than once per thing wanted out of it.
    // The .EXT used to be parsed twice -- once for its cues and again for its
    // waveform -- because both were reached by a path.
    const prolink::AnlzContents dat = prolink::readAnlz(anlzPath);
    const prolink::AnlzContents ext = prolink::readAnlz(anlzPathExt);
    if (!dat.ok && !ext.ok) {
        // Nothing to apply. Not an error: plenty of tracks have no analysis at
        // all, and one that has none still plays.
        kLogger.debug() << "no rekordbox analysis for" << location << "--" << dat.error;
        return;
    }

    // Beatgrids appear to be only correct in the legacy ANLZ file, so the grid
    // comes from the .DAT and everything else from the .EXT.
    applyBeatGrid(track, sampleRate, timingOffset, dat);
    // **The .DAT is the fallback and not merely the other file.** An older
    // rekordbox writes no .EXT at all, and the grid used to be skipped entirely
    // for those tracks -- the one call that could have applied it asked for
    // cues instead. A track with a perfectly good PQTZ tag arrived ungridded.
    applyCues(track, sampleRate, timingOffset, ext.ok ? ext : dat);
    // The waveform too, which is what otherwise makes Mixxx decode the whole
    // file after everything else has already been imported.
    importWaveforms(track, ext, pAnalysisDao, sampleRate);

    // Mixxx's main cue is where the track loads and where CUE returns to. Above,
    // it is set from the first *memory* cue -- but plenty of rekordbox libraries
    // have none at all. Two tracks pulled off a real stick: first beat at 23 ms,
    // hot cue A at 23 ms, memory cues empty. Nothing then tells the main cue
    // where to go, so it keeps whatever default it had and the track loads a
    // fraction before the downbeat.
    //
    // So fall back to the first beat, which is what a CDJ does with a gridded
    // track and what a DJ expects from a load. Only when rekordbox supplied
    // nothing: a memory cue is a deliberate choice and outranks this.
    if (!track->findCueByType(CueType::MainCue)) {
        const auto pBeats = track->getBeats();
        if (pBeats) {
            const audio::FramePos firstBeat = pBeats->firstBeat();
            if (firstBeat.isValid()) {
                track->setMainCuePosition(firstBeat);
            }
        }
    }
}

} // namespace rekordbox
} // namespace mixxx
