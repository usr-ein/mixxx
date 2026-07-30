#include "library/rekordbox/rekordboxanalysis.h"

#include <mp3guessenc.h>
#include <rekordbox_anlz.h>

#include <QFile>
#include <QSet>
#include <QTextCodec>
#include <QtDebug>

#include <fstream>

#include "library/rekordbox/rekordboxconstants.h"
#include "library/rekordbox/rekordboxwaveform.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

/// Kaitai uses std::string as the single container for every string encoding,
/// so the decode has to happen here rather than in the schema. UTF-16 **big**
/// endian for ANLZ cue comments -- note that the pdb strings next door are
/// little-endian, and the two must not share a helper.
QString fromUtf16BeString(const std::string& toConvert) {
    const int length = static_cast<int>(toConvert.length()) - 2; // trailing NUL
    if (length <= 0) {
        return QString();
    }
    return QTextCodec::codecForName("UTF-16BE")->toUnicode(toConvert.data(), length);
}

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

void readAnalyzeFile(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath) {
    if (!QFile(anlzPath).exists()) {
        return;
    }

    qDebug() << "Rekordbox ANLZ path:" << anlzPath << " for: " << track->getTitle();

    std::ifstream ifs(anlzPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_anlz_t anlz = rekordbox_anlz_t(&ks);

    const double sampleRateKhz = sampleRate / 1000.0;

    QList<memory_cue_loop_t> memoryCuesAndLoops;
    int lastHotCueIndex = 0;

    for (const auto& section : *anlz.sections()) {
        switch (section->fourcc()) {
        case rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID: {
            if (!ignoreCues) {
                break;
            }

            auto* beatGridTag =
                    static_cast<rekordbox_anlz_t::beat_grid_tag_t*>(
                            section->body());

            QVector<mixxx::audio::FramePos> beats;
            mixxx::audio::FramePos firstDownbeatPosition;
            mixxx::audio::FramePos firstBeatPosition;
            // Every beat carries the tempo rekordbox analysed it at. On a
            // fixed-tempo track they are all the same value, and that value is
            // exact -- see below for why it matters more than the positions.
            QSet<uint16_t> storedTempos;

            for (const auto& beat : *beatGridTag->beats()) {
                int time = static_cast<int>(beat->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position =
                        mixxx::audio::FramePos(sampleRateKhz * static_cast<double>(time));
                beats << position;
                if (beat->tempo() > 0) {
                    storedTempos.insert(beat->tempo());
                }
                if (!firstBeatPosition.isValid()) {
                    firstBeatPosition = position;
                }

                // Rekordbox numbers every beat 1..4 within its bar. Mixxx has
                // nowhere to store that, but remembering the first downbeat is
                // enough to reconstruct the phase of the whole grid.
                if (beat->beat_number() == 1 && !firstDownbeatPosition.isValid()) {
                    firstDownbeatPosition = position;
                }
            }

            // **A fixed-tempo track gets a fixed-tempo grid, from the stored
            // tempo rather than from the beat positions.**
            //
            // rekordbox writes beat times as whole milliseconds. At 144 BPM a
            // beat is 416.67 ms, so the stored gaps alternate 417, 417, 416 --
            // and a grid built from those positions has a local tempo that
            // swings by up to 0.45 BPM depending on which pair of beats is
            // being measured. That is what makes the displayed BPM wander as a
            // track plays: not the hardware, and not rekordbox being clever,
            // just millisecond rounding preserved into a variable-tempo grid.
            //
            // The tempo field alongside each beat has no such problem: it is
            // centi-BPM, it is exact, and on all six real tracks checked it was
            // identical for every beat of the track. So when there is exactly
            // one, use it and anchor the grid at the first beat.
            mixxx::BeatsPointer pBeats;
            if (storedTempos.size() == 1 && firstBeatPosition.isValid()) {
                const double bpm = *storedTempos.constBegin() / 100.0;
                pBeats = mixxx::Beats::fromConstTempo(sampleRate,
                        firstBeatPosition,
                        mixxx::Bpm(bpm),
                        mixxx::rekordboxconstants::beatsSubversion);
            } else {
                // Genuinely variable tempo -- a live recording, or a track
                // gridded by hand. The per-beat positions are all there is, and
                // their millisecond rounding is the least of the problem.
                pBeats = mixxx::Beats::fromBeatPositions(
                        sampleRate,
                        beats,
                        mixxx::rekordboxconstants::beatsSubversion);
            }
            track->trySetBeats(pBeats);

            // The intro cue is what WaveformRenderBeat anchors its downbeat
            // display to, so seed it from the grid to get the same bar phase
            // Rekordbox shows. Never clobber an existing intro cue: the user
            // (or AnalyzerSilence) may have placed it deliberately.
            if (firstDownbeatPosition.isValid() &&
                    !track->findCueByType(mixxx::CueType::Intro)) {
                track->createAndAddCue(mixxx::CueType::Intro,
                        Cue::kNoHotCue,
                        firstDownbeatPosition,
                        mixxx::audio::kInvalidFramePos);
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES: {
            if (ignoreCues) {
                break;
            }

            auto* cuesTag =
                    static_cast<rekordbox_anlz_t::cue_tag_t*>(
                            section->body());

            for (const auto& cueEntry : *cuesTag->cues()) {
                int time = static_cast<int>(cueEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime = static_cast<int>(cueEntry->loop_time()) - timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueEntry->hot_cue() - 1);
                    if (hotCueIndex > lastHotCueIndex) {
                        lastHotCueIndex = hotCueIndex;
                    }
                    setHotCue(
                            track,
                            position,
                            mixxx::audio::kInvalidFramePos,
                            hotCueIndex,
                            QString(),
                            mixxx::RgbColor::nullopt());
                } break;
                }
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES_2: {
            if (ignoreCues) {
                break;
            }

            auto* cuesExtendedTag =
                    static_cast<rekordbox_anlz_t::cue_extended_tag_t*>(
                            section->body());

            for (const auto& cueExtendedEntry : *cuesExtendedTag->cues()) {
                int time = static_cast<int>(cueExtendedEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesExtendedTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueExtendedEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        memoryCue.color = colorFromIDLocal(static_cast<int>(
                                cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime =
                                static_cast<int>(
                                        cueExtendedEntry->loop_time()) -
                                timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        loop.color = colorFromIDLocal(static_cast<int>(cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueExtendedEntry->hot_cue() - 1);
                    if (hotCueIndex > lastHotCueIndex) {
                        lastHotCueIndex = hotCueIndex;
                    }
                    setHotCue(track,
                            position,
                            mixxx::audio::kInvalidFramePos,
                            hotCueIndex,
                            fromUtf16BeString(cueExtendedEntry->comment()),
                            mixxx::RgbColor(qRgb(
                                    static_cast<int>(
                                            cueExtendedEntry->color_red()),
                                    static_cast<int>(
                                            cueExtendedEntry->color_green()),
                                    static_cast<int>(cueExtendedEntry
                                                    ->color_blue()))));
                } break;
                }
            }
        } break;
        default:
            break;
        }
    }

    if (memoryCuesAndLoops.size() > 0) {
        std::sort(memoryCuesAndLoops.begin(),
                memoryCuesAndLoops.end(),
                [](const memory_cue_loop_t& a, const memory_cue_loop_t& b)
                        -> bool { return a.startPosition < b.startPosition; });

        bool mainCueFound = false;

        // Add memory cues and loops
        for (int memoryCueOrLoopIndex = 0;
                memoryCueOrLoopIndex < memoryCuesAndLoops.size();
                memoryCueOrLoopIndex++) {
            memory_cue_loop_t memoryCueOrLoop = memoryCuesAndLoops[memoryCueOrLoopIndex];

            if (!mainCueFound && !memoryCueOrLoop.endPosition.isValid()) {
                // Set first chronological memory cue as Mixxx MainCue
                track->setMainCuePosition(memoryCueOrLoop.startPosition);
                CuePointer pMainCue = track->findCueByType(mixxx::CueType::MainCue);
                pMainCue->setLabel(memoryCueOrLoop.comment);
                pMainCue->setColor(*memoryCueOrLoop.color);
                mainCueFound = true;
            } else {
                // Mixxx v2.4 will feature multiple loops, so these saved here will be usable
                // For 2.3, Mixxx treats them as hotcues and the first one will be loaded as the single loop Mixxx supports
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
}

} // namespace

namespace mixxx {
namespace rekordbox {

RgbColor colorFromID(int colorID) {
    return colorFromIDLocal(colorID);
}

void readAnalyze(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath) {
    readAnalyzeFile(track, sampleRate, timingOffset, ignoreCues, anlzPath);
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
        AnalysisDao* pAnalysisDao) {
    if (!track || anlzPath.isEmpty()) {
        return;
    }
    const QString location = track->getLocation();
    const int timingOffset = timingOffsetForFile(location);
    const audio::SampleRate sampleRate = track->getSampleRate();
    const QString anlzPathExt = anlzPath.left(anlzPath.length() - 3) + QStringLiteral("EXT");

    if (QFile(anlzPathExt).exists()) {
        // Beatgrids appear to be only correct in the legacy ANLZ file, so the
        // grid comes from the .DAT and everything else from the .EXT.
        readAnalyzeFile(track, sampleRate, timingOffset, true, anlzPath);
        readAnalyzeFile(track, sampleRate, timingOffset, false, anlzPathExt);
        // The waveform too, which is what otherwise makes Mixxx decode the whole
        // file after everything else has already been imported.
        importWaveforms(track, anlzPathExt, pAnalysisDao);
    } else {
        readAnalyzeFile(track, sampleRate, timingOffset, false, anlzPath);
    }

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
