#include "library/rekordbox/rekordboxwaveform.h"

#include <rekordbox_anlz.h>

#include <QFile>
#include <QtDebug>

#include <fstream>
#include <vector>

#include "library/dao/analysisdao.h"
#include "track/track.h"
#include "waveform/waveform.h"
#include "waveform/waveformfactory.h"

namespace {

/// rekordbox's detail waveforms are 150 points per second, on both the plain and
/// the colour tag. Not read from the file -- nothing in the tag states it -- but
/// it is the rate the format is defined at, and the entry count divided by the
/// track duration comes out at 150 on every file checked.
constexpr double kRekordboxPointsPerSecond = 150.0;

/// Mixxx's own waveform resolution, matching AnalyzerWaveform: one visual sample
/// per 441 frames, i.e. 100 per second at 44.1 kHz.
constexpr int kMixxxFramesPerVisualSample = 441;
/// Two visual samples per pixel across a 1920-wide overview.
constexpr int kSummaryVisualSamples = 2 * 1920;

/// One entry of the `PWV5` colour waveform.
///
/// A 16-bit big-endian word. The layout below was derived from the files rather
/// than taken on trust, because the layout in common circulation
/// (`blue = w >> 2`, `green = w >> 5`, `red = w >> 8`) does not match: its
/// implied magnitude correlates at 0.13 with the plain `PWV3` waveform of the
/// same track, where the field at bits 2-6 correlates at **0.99**.
///
///     bits 15-13  low      mean level 5.70, relative roughness 0.06
///     bits 12-10  high     mean level 2.12, relative roughness 0.24
///     bits  9-7   mid      mean level 3.40, relative roughness 0.21
///     bits  6-2   overall  0.99 correlation with PWV3's 5-bit height
///     bits  1-0   always zero across every entry of every file checked
///
/// The band assignment is inferred, from two independent signals that agree:
/// bass is both the loudest band and the slowest-varying, treble the quietest
/// and the spikiest. Measured over six tracks off a real stick.
struct ColourPoint {
    quint8 low;
    quint8 mid;
    quint8 high;
    quint8 all;
};

ColourPoint decodePoint(quint16 word) {
    ColourPoint point;
    point.low = static_cast<quint8>((word >> 13) & 0x07);
    point.high = static_cast<quint8>((word >> 10) & 0x07);
    point.mid = static_cast<quint8>((word >> 7) & 0x07);
    point.all = static_cast<quint8>((word >> 2) & 0x1F);
    return point;
}

/// 3-bit and 5-bit fields scaled to the 0-255 Mixxx stores.
///
/// Multiplied rather than shifted so full scale maps to full scale: `7 << 5` is
/// 224, which would cap a loud track a seventh short of the top of the display.
constexpr quint8 scale3(quint8 value) {
    return static_cast<quint8>(value * 255 / 7);
}
constexpr quint8 scale5(quint8 value) {
    return static_cast<quint8>(value * 255 / 31);
}

/// Fill one Waveform by resampling *points* onto its visual samples.
///
/// The two rates do not divide (150 into 100), so each output sample covers a
/// fractional range of inputs and takes the **maximum** over it. Maximum rather
/// than mean because a waveform display is an envelope: averaging a transient
/// with its neighbours flattens exactly the peaks a DJ is looking at.
void fillWaveform(Waveform* pWaveform,
        const std::vector<ColourPoint>& points,
        double trackSeconds) {
    const int outputSize = pWaveform->getDataSize();
    if (outputSize <= 0 || points.empty() || trackSeconds <= 0) {
        return;
    }
    const double pointsPerOutput =
            static_cast<double>(points.size()) / static_cast<double>(outputSize);

    for (int i = 0; i < outputSize; ++i) {
        const auto begin = static_cast<size_t>(i * pointsPerOutput);
        auto end = static_cast<size_t>((i + 1) * pointsPerOutput);
        // Always consume at least one input, or an output longer than the input
        // would leave empty samples between the ones it does fill.
        end = std::max(end, begin + 1);
        end = std::min(end, points.size());

        ColourPoint peak{0, 0, 0, 0};
        for (size_t p = begin; p < end; ++p) {
            peak.low = std::max(peak.low, points[p].low);
            peak.mid = std::max(peak.mid, points[p].mid);
            peak.high = std::max(peak.high, points[p].high);
            peak.all = std::max(peak.all, points[p].all);
        }
        // Written through data() rather than the low()/mid()/high() setters,
        // which are private to AnalyzerWaveform.
        WaveformData& out = pWaveform->data()[i];
        out.filtered.low = scale3(peak.low);
        out.filtered.mid = scale3(peak.mid);
        out.filtered.high = scale3(peak.high);
        out.filtered.all = scale5(peak.all);
    }
}

/// The colour waveform's entries, or empty if the file has none.
std::vector<ColourPoint> readColourWaveform(const QString& anlzPathExt) {
    std::vector<ColourPoint> points;
    if (!QFile(anlzPathExt).exists()) {
        return points;
    }
    try {
        std::ifstream file(anlzPathExt.toStdString(), std::ifstream::binary);
        if (!file.good()) {
            return points;
        }
        kaitai::kstream stream(&file);
        rekordbox_anlz_t anlz(&stream);

        for (const auto& pSection : *anlz.sections()) {
            auto* section = pSection.get();
            if (section->fourcc() !=
                    rekordbox_anlz_t::SECTION_TAGS_WAVE_COLOR_SCROLL) {
                continue;
            }
            auto* body = static_cast<rekordbox_anlz_t::wave_color_scroll_tag_t*>(
                    section->body());
            if (body->len_entry_bytes() != 2) {
                // Two bytes per entry is what the format says and what every
                // file has. Anything else is a variant we cannot read, and
                // guessing at it would draw a plausible but wrong waveform --
                // worse than falling back to a real analysis.
                qWarning() << "rekordbox colour waveform has"
                           << body->len_entry_bytes()
                           << "bytes per entry, expected 2; ignoring it";
                return {};
            }
            const std::string& entries = body->entries();
            const size_t count = std::min(static_cast<size_t>(body->len_entries()),
                    entries.size() / 2);
            points.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                const auto high = static_cast<quint8>(entries[i * 2]);
                const auto low = static_cast<quint8>(entries[i * 2 + 1]);
                points.push_back(decodePoint(
                        static_cast<quint16>((high << 8) | low)));
            }
            return points;
        }
    } catch (const std::exception& e) {
        qWarning() << "could not read" << anlzPathExt << ":" << e.what();
        return {};
    } catch (...) {
        qWarning() << "could not read" << anlzPathExt;
        return {};
    }
    return points;
}

} // namespace

namespace mixxx {
namespace rekordbox {

bool importWaveforms(TrackPointer track,
        const QString& anlzPathExt,
        AnalysisDao* pAnalysisDao) {
    if (!track) {
        return false;
    }
    // Already has one, from a previous load or from Mixxx's own analyzer.
    if (track->getWaveform() && track->getWaveformSummary()) {
        return true;
    }

    const std::vector<ColourPoint> points = readColourWaveform(anlzPathExt);
    if (points.empty()) {
        return false;
    }

    const audio::SampleRate sampleRate = track->getSampleRate();
    const auto duration = track->getDuration();
    if (!sampleRate.isValid() || duration <= 0) {
        return false;
    }
    // Length in frames from rekordbox's own point count rather than from the
    // track's duration: the two agree to within a point, and using the point
    // count keeps the mapping exact at the end of the track, where a duration
    // rounded to the second would otherwise stretch or clip the last seconds.
    const double pointSeconds =
            static_cast<double>(points.size()) / kRekordboxPointsPerSecond;
    const auto frameLength =
            static_cast<SINT>(pointSeconds * static_cast<double>(sampleRate));

    auto pWaveform = WaveformPointer(new Waveform(
            sampleRate, frameLength, kMixxxFramesPerVisualSample, -1));
    auto pSummary = WaveformPointer(new Waveform(sampleRate,
            frameLength,
            kMixxxFramesPerVisualSample,
            kSummaryVisualSamples));

    fillWaveform(pWaveform.data(), points, pointSeconds);
    fillWaveform(pSummary.data(), points, pointSeconds);

    // Marked complete and current, exactly as AnalyzerWaveform::storeResults
    // does -- a waveform that is not both is treated as missing and analysed
    // again, which would make this whole exercise a no-op.
    pWaveform->setSaveState(Waveform::SaveState::SavePending);
    pWaveform->setCompletion(pWaveform->getDataSize());
    pWaveform->setVersion(WaveformFactory::currentWaveformVersion());
    pWaveform->setDescription(WaveformFactory::currentWaveformDescription());
    track->setWaveform(pWaveform);

    pSummary->setSaveState(Waveform::SaveState::SavePending);
    pSummary->setCompletion(pSummary->getDataSize());
    pSummary->setVersion(WaveformFactory::currentWaveformSummaryVersion());
    pSummary->setDescription(WaveformFactory::currentWaveformSummaryDescription());
    track->setWaveformSummary(pSummary);

    if (pAnalysisDao && track->getId().isValid()) {
        pAnalysisDao->saveTrackAnalyses(track->getId(), pWaveform, pSummary);
    }
    qDebug() << "imported rekordbox waveform:" << points.size() << "points ->"
             << pWaveform->getDataSize() << "visual samples for" << track->getLocation();
    return true;
}

} // namespace rekordbox
} // namespace mixxx
