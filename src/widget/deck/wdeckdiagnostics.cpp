#include "widget/deck/wdeckdiagnostics.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QStorageInfo>
#include <QStringList>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/ramstore.h"
#include "library/deck/streamingfile.h"
#include "library/deck/trackcache.h"
#include "util/versionstore.h"

namespace {
/// A minute of history at one sample a second.
constexpr int kHistoryLength = 60;

QString bytes(qint64 n) {
    if (n >= 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(n / (1024.0 * 1024 * 1024), 0, 'f', 2);
    }
    if (n >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(n / (1024.0 * 1024), 0, 'f', 1);
    }
    return QStringLiteral("%1 kB").arg(n / 1024.0, 0, 'f', 0);
}

QString row(const QString& label, const QString& value) {
    return QStringLiteral("<tr><td class='k'>%1</td><td>%2</td></tr>")
            .arg(label.toHtmlEscaped(), value);
}
} // namespace

namespace mixxx {
namespace deck {

WDeckDiagnostics::WDeckDiagnostics(QWidget* pParent)
        : QTextBrowser(pParent) {
    setObjectName(QStringLiteral("DeckDiagnostics"));
    setOpenExternalLinks(false);
    setOpenLinks(false);

    // Dragged with a finger, like every other list on this deck. A QTextBrowser
    // scrolls with a scrollbar and a wheel out of the box and with neither of
    // those on a touch panel -- so without this the page is simply stuck at the
    // top, which is what it was.
    QScroller::grabGesture(viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties properties = QScroller::scroller(viewport())->scrollerProperties();
    properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
            QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
            QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    QScroller::scroller(viewport())->setScrollerProperties(properties);
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &WDeckDiagnostics::sample);
}

void WDeckDiagnostics::scrollBy(int steps) {
    // One detent moves about a third of a screen, which is what it takes to get
    // from the top of this page to the bottom without spinning the encoder all
    // night.
    QScrollBar* pBar = verticalScrollBar();
    pBar->setValue(pBar->value() + steps * pBar->pageStep() / 3);
}

void WDeckDiagnostics::setActive(bool active) {
    if (active) {
        sample();
        m_timer.start();
    } else {
        m_timer.stop();
    }
}

QString WDeckDiagnostics::readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

QString WDeckDiagnostics::runCommand(const QString& program, const QStringList& args) {
    QProcess process;
    process.start(program, args);
    // Short and bounded: this runs once a second and must never be what makes
    // the page stop updating.
    if (!process.waitForFinished(300)) {
        process.kill();
        return QString();
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

double WDeckDiagnostics::sampleCpu() {
    const QString stat = readFile(QStringLiteral("/proc/stat"));
    const QStringList fields = stat.section(QChar('\n'), 0, 0).simplified().split(QChar(' '));
    if (fields.size() < 5) {
        return 0.0;
    }
    quint64 total = 0;
    for (int i = 1; i < fields.size(); ++i) {
        total += fields.at(i).toULongLong();
    }
    const quint64 idle = fields.at(4).toULongLong();
    const quint64 deltaTotal = total - m_lastCpuTotal;
    const quint64 deltaIdle = idle - m_lastCpuIdle;
    m_lastCpuTotal = total;
    m_lastCpuIdle = idle;
    if (deltaTotal == 0) {
        return 0.0;
    }
    return 100.0 * (1.0 - static_cast<double>(deltaIdle) / static_cast<double>(deltaTotal));
}

QString WDeckDiagnostics::sparkline(const QList<double>& history, double max) {
    // Eight levels, which is all the block characters offer and more than the
    // eye takes off a 60px-wide strip anyway.
    static const QString kBlocks = QStringLiteral("▁▂▃▄▅▆▇█");
    QString out;
    out.reserve(history.size());
    for (double value : history) {
        int level = max > 0 ? static_cast<int>(value / max * (kBlocks.size() - 1)) : 0;
        out.append(kBlocks.at(qBound(0, level, kBlocks.size() - 1)));
    }
    return out;
}

void WDeckDiagnostics::sample() {
    m_cpuHistory.append(sampleCpu());

    const QString meminfo = readFile(QStringLiteral("/proc/meminfo"));
    const auto meminfoValue = [&meminfo](const QString& key) -> double {
        for (const QString& line : meminfo.split(QChar('\n'))) {
            if (line.startsWith(key)) {
                return line.section(QChar(':'), 1).simplified().section(QChar(' '), 0, 0).toDouble();
            }
        }
        return 0.0;
    };
    const double memTotal = meminfoValue(QStringLiteral("MemTotal"));
    const double memAvailable = meminfoValue(QStringLiteral("MemAvailable"));
    m_memHistory.append(memTotal > 0 ? 100.0 * (1.0 - memAvailable / memTotal) : 0.0);

    const double milliCelsius =
            readFile(QStringLiteral("/sys/class/thermal/thermal_zone0/temp")).trimmed().toDouble();
    m_tempHistory.append(milliCelsius / 1000.0);

    for (QList<double>* pHistory : {&m_cpuHistory, &m_memHistory, &m_tempHistory}) {
        while (pHistory->size() > kHistoryLength) {
            pHistory->removeFirst();
        }
    }

    const int scrollPosition = verticalScrollBar()->value();
    setHtml(html());
    verticalScrollBar()->setValue(scrollPosition);
}

QString WDeckDiagnostics::html() const {
    QString out = QStringLiteral(
            "<style>"
            "body { color:#dddddd; font-family:'MesloLGL Nerd Font'; font-size:15px; }"
            "h2 { color:#88ff00; font-size:17px; margin-top:18px; }"
            "td { padding:2px 10px 2px 0; }"
            "td.k { color:#888888; }"
            ".warn { color:#ff6600; }"
            "</style>");

    // ---- identity ----------------------------------------------------------
    out += QStringLiteral("<h2>Identity</h2><table>");
    out += row(tr("Mixxx"), VersionStore::version().toHtmlEscaped());
    out += row(tr("Now"),
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    const QString uptime = readFile(QStringLiteral("/proc/uptime")).section(QChar(' '), 0, 0);
    out += row(tr("Uptime"),
            QStringLiteral("%1 h").arg(uptime.toDouble() / 3600.0, 0, 'f', 1));
    out += QStringLiteral("</table>");

    // ---- system ------------------------------------------------------------
    out += QStringLiteral("<h2>System</h2><table>");
    out += row(tr("CPU"),
            QStringLiteral("%1 %  <span style='color:#88ff00'>%2</span>")
                    .arg(m_cpuHistory.isEmpty() ? 0.0 : m_cpuHistory.last(), 0, 'f', 0)
                    .arg(sparkline(m_cpuHistory, 100.0)));
    out += row(tr("Memory"),
            QStringLiteral("%1 %  <span style='color:#88ff00'>%2</span>")
                    .arg(m_memHistory.isEmpty() ? 0.0 : m_memHistory.last(), 0, 'f', 0)
                    .arg(sparkline(m_memHistory, 100.0)));
    out += row(tr("Temperature"),
            QStringLiteral("%1 °C  <span style='color:#88ff00'>%2</span>")
                    .arg(m_tempHistory.isEmpty() ? 0.0 : m_tempHistory.last(), 0, 'f', 1)
                    .arg(sparkline(m_tempHistory, 90.0)));

    // Swap in use is the tell that the cache is sized wrong: the deck's swap is
    // zram, and compressed audio does not compress.
    const QString swaps = readFile(QStringLiteral("/proc/swaps"));
    qint64 swapUsed = 0;
    const QStringList swapLines = swaps.split(QChar('\n'));
    for (int i = 1; i < swapLines.size(); ++i) {
        const QStringList f = swapLines.at(i).simplified().split(QChar(' '));
        if (f.size() >= 4) {
            swapUsed += f.at(3).toLongLong() * 1024;
        }
    }
    out += row(tr("Swap in use"),
            swapUsed > 0
                    ? QStringLiteral("<span class='warn'>%1 — tier 1 may be too big</span>")
                              .arg(bytes(swapUsed))
                    : QStringLiteral("none"));

    // Under-voltage on a Pi is the single most common cause of inexplicable
    // behaviour, and it is invisible unless something asks.
    const QString throttled = runCommand(QStringLiteral("vcgencmd"),
            {QStringLiteral("get_throttled")});
    if (!throttled.isEmpty()) {
        const bool clean = throttled.endsWith(QStringLiteral("0x0"));
        out += row(tr("Throttling"),
                clean ? throttled
                      : QStringLiteral("<span class='warn'>%1</span>").arg(throttled));
    }

    const QStorageInfo root(QStringLiteral("/"));
    out += row(tr("Root filesystem"),
            QStringLiteral("%1 free of %2")
                    .arg(bytes(root.bytesAvailable()), bytes(root.bytesTotal())));
    out += QStringLiteral("</table>");

    // ---- audio -------------------------------------------------------------
    out += QStringLiteral("<h2>Audio</h2><table>");
    out += row(tr("Underruns"),
            QString::number(static_cast<int>(ControlObject::get(
                    ConfigKey(QStringLiteral("[App]"),
                            QStringLiteral("audio_latency_overload_count"))))));
    out += row(tr("Latency"),
            QStringLiteral("%1 ms").arg(
                    ControlObject::get(ConfigKey(QStringLiteral("[App]"),
                            QStringLiteral("output_latency_ms"))),
                    0, 'f', 1));
    out += QStringLiteral("</table>");

    // ---- media -------------------------------------------------------------
    out += QStringLiteral("<h2>Media</h2><table>");
    MediaRegistry* pRegistry = MediaRegistry::instance();
    if (pRegistry) {
        for (const MediumInfo& medium : pRegistry->media()) {
            QString state;
            switch (medium.state) {
            case MediumInfo::State::Reading: state = tr("reading"); break;
            case MediumInfo::State::Ready: state = tr("ready"); break;
            case MediumInfo::State::Offline: state = tr("offline"); break;
            case MediumInfo::State::Failed:
                state = QStringLiteral("<span class='warn'>%1</span>")
                                .arg(medium.error.toHtmlEscaped());
                break;
            }
            out += row(medium.name,
                    QStringLiteral("%1 · %2 tracks · %3 playlists · %4")
                            .arg(medium.id.key().toHtmlEscaped())
                            .arg(medium.trackCount)
                            .arg(medium.playlistCount)
                            .arg(state));
        }
        if (pRegistry->media().isEmpty()) {
            out += row(tr("Media"), tr("none"));
        }
    }
    out += QStringLiteral("</table>");

    // ---- what we are offering ----------------------------------------------
    //
    // A phantom row is the one worth spotting: it means a stick has been pulled
    // while a player was still playing off it, and that player is now being fed
    // from a copy in RAM. It should clear itself when the player moves on.
    if (pRegistry) {
        const mixxx::prolink::server::ServeStatus serve = pRegistry->serveStatus();
        if (serve.active && (!serve.media.isEmpty() || !serve.consumers.isEmpty())) {
            out += QStringLiteral("<h2>Serving</h2><table>");
            out += row(tr("As player"), QString::number(serve.deviceNumber));
            for (const mixxx::prolink::server::ServedSlot& slot : serve.media) {
                out += row(slot.volumeName.isEmpty() ? slot.exportPath : slot.volumeName,
                        slot.phantom
                                ? QStringLiteral("<span class='warn'>%1</span>")
                                          .arg(tr("gone — feeding a player from cache"))
                                : tr("%1 tracks").arg(slot.trackCount));
            }
            for (const mixxx::prolink::server::ServeConsumer& reader : serve.consumers) {
                out += row(tr("Player %1").arg(reader.deviceNumber),
                        tr("%1 track %2")
                                .arg(reader.playing ? tr("playing") : tr("holding"))
                                .arg(reader.trackId));
            }
            if (serve.consumers.isEmpty()) {
                out += row(tr("Consumers"), tr("none"));
            }
            out += QStringLiteral("</table>");
        }
    }

    // ---- streaming ---------------------------------------------------------
    //
    // The question this answers is whether the download is keeping ahead of the
    // playhead. A healthy remote track waits a handful of times while it opens
    // and then never again; a growing wait count is the warning that comes
    // before the audio stutters, and it is invisible everywhere else.
    const auto streams = StreamingFileRegistry::snapshot();
    if (!streams.isEmpty()) {
        out += QStringLiteral("<h2>Streaming</h2><table>");
        for (const auto& stream : streams) {
            const auto& pStream = stream.second;
            if (!pStream) {
                continue;
            }
            const QString waits = pStream->waitCount() == 0
                    ? tr("never waited")
                    : QStringLiteral("<span class='warn'>%1 waits / %2 ms</span>")
                              .arg(pStream->waitCount())
                              .arg(pStream->waitedMs());
            out += row(QFileInfo(stream.first).fileName(),
                    QStringLiteral("%1 · %2 · %3")
                            .arg(bytes(pStream->size()),
                                    pStream->isComplete() ? tr("complete")
                                                          : tr("still arriving"),
                                    waits));
        }
        out += QStringLiteral("</table>");
    }

    // ---- cache -------------------------------------------------------------
    out += QStringLiteral("<h2>Track cache</h2><table>");
    // Where the RAM caches actually landed, and what is left of it. The first
    // number to look at when caching misbehaves: a store that has filled makes
    // every downstream symptom look like a different fault.
    const qint64 storeFree = RamStore::available();
    out += row(tr("RAM store"),
            storeFree < 64LL * 1024 * 1024
                    ? QStringLiteral("%1 — <span class='warn'>%2 free</span>")
                              .arg(RamStore::root().toHtmlEscaped(), bytes(storeFree))
                    : QStringLiteral("%1 — %2 free")
                              .arg(RamStore::root().toHtmlEscaped(), bytes(storeFree)));
    TrackCache* pCache = TrackCache::instance();
    if (pCache) {
        out += row(tr("In RAM"), bytes(pCache->bytesInRam()));
        out += row(tr("On disk"), bytes(pCache->bytesOnDisk()));
        // The number that says whether the whole tiering scheme is holding. It
        // should read zero on a normal night.
        out += row(tr("Written to card"),
                pCache->bytesWrittenToDisk() == 0
                        ? QStringLiteral("none")
                        : QStringLiteral("<span class='warn'>%1</span>")
                                  .arg(bytes(pCache->bytesWrittenToDisk())));
    }
    out += QStringLiteral("</table>");

    return out;
}

} // namespace deck
} // namespace mixxx

#include "moc_wdeckdiagnostics.cpp"
