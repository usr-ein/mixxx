#include "library/rekordbox/rekordboxwaveform.h"

#include <rekordbox_anlz.h>

#include <QFile>
#include <QtDebug>

#include <algorithm>
#include <cmath>
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

/// Mixxx's own waveform resolution, matching AnalyzerWaveform: 441 visual
/// samples **per second** (not per 441 frames -- the constructor parameter is a
/// rate, and reading it the other way is what made the first cut of this look
/// blocky). So the output is nearly three times denser than rekordbox's 150,
/// and the conversion is an upsample.
constexpr int kMixxxVisualSamplesPerSecond = 441;
/// Two visual samples per pixel across a 1920-wide overview.
constexpr int kSummaryVisualSamples = 2 * 1920;

/// Above this many inputs per output, downsampling averages instead of taking
/// the peak. Four is the point where "a couple of samples merged" turns into
/// "a window wide enough that its maximum is always near the ceiling".
constexpr double kMeanAboveRatio = 4.0;

/// Exponent applied to each band's share of the dominant one. 1.0 leaves the
/// hue as rekordbox stored it and renders mostly white; higher values separate
/// the colours. 2.2 keeps bass unmistakably red and stops a bright hi-hat from
/// whiting out the whole bar.
constexpr double kBandContrast = 2.2;

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

/// One output sample's worth of each band, already scaled to the 0-255 Mixxx
/// stores.
struct Scaled {
    double low;
    double mid;
    double high;
    double all;
};

/// Turn one rekordbox point into Mixxx's four channels.
///
/// **The three colour fields are a hue, not three magnitudes.** They sit near
/// saturation whatever the track is doing -- mean levels 5.7, 3.4 and 2.1 out of
/// 7 -- and they correlate with the 5-bit magnitude at only 0.0-0.4, while that
/// magnitude correlates with the plain PWV3 waveform at 0.91-0.99. So rekordbox
/// stores spectral balance and amplitude separately, which is exactly what a
/// colour waveform is.
///
/// Mixxx's RGB renderer, though, derives its height from the three bands
/// (`low^2 + mid^2 + high^2`) and never reads `filtered.all`. Feeding it the hue
/// values directly therefore draws a waveform whose height is the spectral
/// balance rather than the volume -- and quantised to the eight levels three
/// bits allow, which is what made it blocky.
///
/// So the dominant band is scaled to the 5-bit amplitude and the other two keep
/// their ratio to it. Hue is preserved exactly, height becomes the real envelope
/// at 32 levels instead of 8, and `filtered.all` is filled in too for the
/// renderers that do use it.
Scaled scalePoint(const ColourPoint& point) {
    const double amplitude = point.all * 255.0 / 31.0;
    const quint8 dominant = std::max({point.low, point.mid, point.high});
    if (dominant == 0) {
        return {0.0, 0.0, 0.0, amplitude};
    }

    // **Contrast between the bands, or everything comes out white.**
    //
    // Mixxx's RGB renderer mixes the three band colours in proportion, so three
    // near-equal bands make near-white. rekordbox's hue fields sit close to
    // saturation whatever is playing -- mean levels 5.7, 3.4 and 2.1 out of 7 --
    // so a straight proportional scaling washed most of the track out, and any
    // passage with real top end went white and stayed there.
    //
    // Raising each band's ratio to a power pushes the quieter two down without
    // touching the dominant one, which separates the colours while leaving the
    // hue's ordering exactly as rekordbox recorded it. The height is unaffected:
    // that comes from the 5-bit amplitude.
    const double inverse = 1.0 / static_cast<double>(dominant);
    const double low = std::pow(point.low * inverse, kBandContrast) * amplitude;
    const double mid = std::pow(point.mid * inverse, kBandContrast) * amplitude;
    const double high = std::pow(point.high * inverse, kBandContrast) * amplitude;
    return {low, mid, high, amplitude};
}

/// Rounded and clamped. The scaling above can push a band past 255 when two are
/// nearly equal, and wrapping that to a small number would punch holes in the
/// waveform at exactly its loudest moments.
unsigned char toByte(double value) {
    return static_cast<unsigned char>(std::lround(std::clamp(value, 0.0, 255.0)));
}

/// Fill one Waveform by resampling *points* onto its visual samples.
///
/// The two directions want opposite treatment, and getting that wrong is
/// visible: the main waveform is 441 samples per second against rekordbox's 150,
/// so it is an **upsample**, while the overview is a few samples per second and
/// is a heavy **downsample**.
///
///  * Downsampling takes the maximum over a *narrow* range and the **mean** over
///    a wide one. Max is right for the detail waveform, where a handful of
///    inputs collapse into one output and a transient would otherwise be lost.
///    It is quite wrong for the overview: a whole track into 1920 samples is
///    over twenty inputs each, and the loudest of twenty is near the ceiling
///    almost everywhere, so the minimap came out a solid block at full height
///    with no shape to it at all. Past a few inputs per output the mean is what
///    carries the shape of the track.
///  * Upsampling **interpolates** between the two nearest inputs. Repeating the
///    nearest one instead -- which is what a max over a sub-sample range degrades
///    to -- draws each rekordbox point as three identical output samples, and the
///    result is a staircase. It also matters vertically: the bands are only three
///    bits, eight distinct levels, so without interpolation every edge in the
///    picture is quantised twice over.
void fillWaveform(Waveform* pWaveform, const std::vector<ColourPoint>& points) {
    const int outputSize = pWaveform->getDataSize();
    if (outputSize <= 0 || points.empty()) {
        return;
    }

    // **The array is interleaved stereo: two entries per visual sample.**
    // Waveform's constructor sizes it `frameLength / ratio * kAnalysisChannels`,
    // so entry 2n is the left channel of sample n and 2n+1 the right. Treating
    // it as one long mono array maps those two neighbours to *different* points
    // in the track, which skews left against right by half a sample: the
    // renderer then draws two waveforms a hair apart and the interference
    // between them is a slow ripple across the whole thing. Very visible on the
    // overview, where each entry covers a second or more of audio.
    //
    // rekordbox's waveform is mono, so both channels get the same value.
    const int frames = outputSize / 2;
    if (frames <= 0) {
        return;
    }
    const double pointsPerOutput =
            static_cast<double>(points.size()) / static_cast<double>(frames);
    const size_t lastPoint = points.size() - 1;

    for (int i = 0; i < frames; ++i) {
        Scaled value{0, 0, 0, 0};

        if (pointsPerOutput <= 1.0) {
            // Upsampling: linear interpolation between neighbours.
            const double position = i * pointsPerOutput;
            const auto index = static_cast<size_t>(position);
            const double fraction = position - static_cast<double>(index);
            const Scaled a = scalePoint(points[std::min(index, lastPoint)]);
            const Scaled b = scalePoint(points[std::min(index + 1, lastPoint)]);
            value.low = a.low + (b.low - a.low) * fraction;
            value.mid = a.mid + (b.mid - a.mid) * fraction;
            value.high = a.high + (b.high - a.high) * fraction;
            value.all = a.all + (b.all - a.all) * fraction;
        } else {
            const auto begin = static_cast<size_t>(i * pointsPerOutput);
            auto end = static_cast<size_t>((i + 1) * pointsPerOutput);
            end = std::max(end, begin + 1);
            end = std::min(end, points.size());
            const bool useMean = pointsPerOutput > kMeanAboveRatio;
            double count = 0.0;
            for (size_t p = begin; p < end; ++p) {
                const Scaled scaled = scalePoint(points[p]);
                if (useMean) {
                    value.low += scaled.low;
                    value.mid += scaled.mid;
                    value.high += scaled.high;
                    value.all += scaled.all;
                    count += 1.0;
                } else {
                    value.low = std::max(value.low, scaled.low);
                    value.mid = std::max(value.mid, scaled.mid);
                    value.high = std::max(value.high, scaled.high);
                    value.all = std::max(value.all, scaled.all);
                }
            }
            if (useMean && count > 0.0) {
                value.low /= count;
                value.mid /= count;
                value.high /= count;
                value.all /= count;
            }
        }

        // Written through data() rather than the low()/mid()/high() setters,
        // which are private to AnalyzerWaveform. Both channels of the frame.
        WaveformData* pData = pWaveform->data();
        for (int channel = 0; channel < 2; ++channel) {
            WaveformData& out = pData[i * 2 + channel];
            out.filtered.low = toByte(value.low);
            out.filtered.mid = toByte(value.mid);
            out.filtered.high = toByte(value.high);
            out.filtered.all = toByte(value.all);
        }
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
            sampleRate, frameLength, kMixxxVisualSamplesPerSecond, -1));
    auto pSummary = WaveformPointer(new Waveform(sampleRate,
            frameLength,
            kMixxxVisualSamplesPerSecond,
            kSummaryVisualSamples));

    fillWaveform(pWaveform.data(), points);
    fillWaveform(pSummary.data(), points);

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
