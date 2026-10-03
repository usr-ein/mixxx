#include "widget/deck/wdeckdiagnostics.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHideEvent>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QProcess>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QStorageInfo>
#include <QStringList>
#include <QSysInfo>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/deck/mediaregistry.h"
#include "library/deck/ramstore.h"
#include "library/deck/streamingfile.h"
#include "library/deck/trackcache.h"
#include "util/versionstore.h"
#include "widget/deck/deckaccent.h"
#include "widget/deck/decklevels.h"

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

/// A row of the Adjust section. The label is inverted in the accent while the
/// encoder is moving it, so there is no doubt what a turn will do; padded in
/// both states so it does not shift sideways when it lights.
QString levelRow(const QString& label, const QString& value, bool adjusting = false) {
    return QStringLiteral("<tr><td class='%1'>&nbsp;%2&nbsp;</td><td>%3</td></tr>")
            .arg(adjusting ? QStringLiteral("adj") : QStringLiteral("k"),
                    label.toHtmlEscaped(),
                    value);
}

/// Signed, to one decimal: "+1.5 dB", "−3.0 dB". Unity has no sign, and no
/// gain at all -- set from elsewhere; the trim stops well short -- is −∞.
QString decibels(double db) {
    if (db <= mixxx::deck::DeckLevels::kSilenceDb) {
        return QStringLiteral("−∞ dB");
    }
    if (std::abs(db) < 0.05) {
        return QStringLiteral("0.0 dB");
    }
    return QStringLiteral("%1%2 dB")
            .arg(db > 0 ? QStringLiteral("+") : QStringLiteral("−"))
            .arg(std::abs(db), 0, 'f', 1);
}

/// The interface's IPv4 address, or an empty string: no such interface, or no
/// address on it yet.
QString ipv4Of(const QNetworkInterface& iface) {
    const QList<QNetworkAddressEntry> entries = iface.addressEntries();
    for (const QNetworkAddressEntry& entry : entries) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
            return entry.ip().toString();
        }
    }
    return QString();
}

/// `key=value` lines, the format pi_config/wifi-fallback writes to
/// /run/trimixxx/wifi.
QHash<QString, QString> keyValues(const QString& text) {
    QHash<QString, QString> values;
    const QStringList lines = text.split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const int equals = line.indexOf(QChar('='));
        if (equals > 0) {
            values.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
        }
    }
    return values;
}
} // namespace

namespace mixxx {
namespace deck {

WDeckDiagnostics::WDeckDiagnostics(const QString& settingsPath, QWidget* pParent)
        : QTextBrowser(pParent),
          m_accent(deckAccent().name()),
          // Built with the page, so the levels saved last time are back on the
          // output and the panel as soon as the skin loads, not when someone
          // first opens Diagnostics.
          m_pLevels(std::make_unique<DeckLevels>(settingsPath)),
          m_pClipping(std::make_unique<ControlProxy>(QStringLiteral("[Main]"),
                  QStringLiteral("peak_indicator"),
                  this,
                  ControlFlag::NoAssertIfMissing)) {
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

    // Either edge: the light coming on is a clip starting, and going out is the
    // last one ending. Both mean the output was clipping just now.
    m_pClipping->connectValueChanged(this, [this](double) {
        m_sinceClipping.start();
    });
}

WDeckDiagnostics::~WDeckDiagnostics() = default;

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
        setAdjusting(Adjusting::Nothing);
    }
}

bool WDeckDiagnostics::handleMove(int steps) {
    // Clockwise is up, for both levels, as on any knob.
    switch (m_adjusting) {
    case Adjusting::Nothing:
        scrollBy(steps);
        break;
    case Adjusting::Output:
        m_pLevels->stepOutput(steps);
        render();
        break;
    case Adjusting::Brightness:
        m_pLevels->stepBrightness(steps);
        render();
        break;
    }
    return true;
}

bool WDeckDiagnostics::handleSelect() {
    // Output, then brightness, then back to scrolling: one press each, and a
    // level this deck does not have is skipped rather than offered dead.
    switch (m_adjusting) {
    case Adjusting::Nothing:
        if (m_pLevels->hasOutput()) {
            setAdjusting(Adjusting::Output);
        } else if (m_pLevels->hasBacklight()) {
            setAdjusting(Adjusting::Brightness);
        }
        break;
    case Adjusting::Output:
        setAdjusting(m_pLevels->hasBacklight() ? Adjusting::Brightness : Adjusting::Nothing);
        break;
    case Adjusting::Brightness:
        setAdjusting(Adjusting::Nothing);
        break;
    }
    // Claimed whatever happened. The menu view underneath still holds the
    // level we came from with a row selected, so a press falling through would
    // activate that.
    return true;
}

bool WDeckDiagnostics::handleBack() {
    if (m_adjusting == Adjusting::Nothing) {
        return false;
    }
    setAdjusting(Adjusting::Nothing);
    return true;
}

void WDeckDiagnostics::hideEvent(QHideEvent* pEvent) {
    setAdjusting(Adjusting::Nothing);
    QTextBrowser::hideEvent(pEvent);
}

void WDeckDiagnostics::setAdjusting(Adjusting adjusting) {
    if (adjusting == m_adjusting) {
        return;
    }
    const bool starting = m_adjusting == Adjusting::Nothing;
    m_adjusting = adjusting;
    render();
    if (starting) {
        // The levels are the first section. The page may have been scrolled
        // anywhere, and what the encoder now moves has to be on screen.
        verticalScrollBar()->setValue(0);
    }
}

void WDeckDiagnostics::render() {
    const int scrollPosition = verticalScrollBar()->value();
    setHtml(html());
    verticalScrollBar()->setValue(scrollPosition);
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

    // Asked here, once a second, and not in html(): that also runs on every
    // detent while a level is being adjusted, and a process per detent makes
    // the encoder lag behind the hand.
    m_throttled = runCommand(QStringLiteral("vcgencmd"), {QStringLiteral("get_throttled")});

    render();
}

QString WDeckDiagnostics::html() const {
    // The headings and the sparklines are this deck's accent (deckaccent.h).
    QString out = QStringLiteral(
            "<style>"
            "body { color:#dddddd; font-family:'MesloLGL Nerd Font'; font-size:15px; }"
            "h2 { color:%1; font-size:17px; margin-top:18px; }"
            "td { padding:2px 10px 2px 0; }"
            "td.k { color:#888888; }"
            "td.adj { color:#0c0c0c; background-color:%1; }"
            ".warn { color:#ff6600; }"
            ".spark { color:%1; }"
            ".hint { color:#888888; }"
            ".live { color:%1; }"
            "</style>")
                          .arg(m_accent);

    out += adjustHtml();

    // ---- identity ----------------------------------------------------------
    out += QStringLiteral("<h2>Identity</h2><table>");
    out += row(tr("Mixxx"), VersionStore::version().toHtmlEscaped());
    out += row(tr("Now"),
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    const QString uptime = readFile(QStringLiteral("/proc/uptime")).section(QChar(' '), 0, 0);
    out += row(tr("Uptime"),
            QStringLiteral("%1 h").arg(uptime.toDouble() / 3600.0, 0, 'f', 1));
    out += QStringLiteral("</table>");

    // ---- network -----------------------------------------------------------
    //
    // How to reach the deck, which is what this page gets opened for at a
    // venue. Addresses come live from the kernel, so a cable plugged in after
    // boot shows up within the second. What the Wi-Fi *is* -- home, or the
    // deck's own hotspot and its password -- comes from /run/trimixxx/wifi,
    // which pi_config/wifi-fallback writes once it has decided; until then,
    // and on a deck without it, the address alone.
    //
    // Ethernet is the Pro DJ Link port: on the CDJs' switch it holds a
    // 169.254 link-local address, and that is the one a laptop on the same
    // switch can ssh to.
    out += QStringLiteral("<h2>Network</h2><table>");
    const QString host = QSysInfo::machineHostName().toHtmlEscaped();
    const QString user = qEnvironmentVariable("USER").toHtmlEscaped();
    out += row(tr("Host"),
            QStringLiteral("%1 &nbsp;<span style='color:#888888'>ssh %2%1.local</span>")
                    .arg(host, user.isEmpty() ? QString() : user + QChar('@')));

    const QNetworkInterface ethernet =
            QNetworkInterface::interfaceFromName(QStringLiteral("eth0"));
    const QString ethernetAddress = ipv4Of(ethernet);
    if (!ethernet.isValid()) {
        out += row(tr("Ethernet"), tr("no eth0"));
    } else if (!ethernet.flags().testFlag(QNetworkInterface::IsRunning)) {
        out += row(tr("Ethernet"), tr("no cable"));
    } else if (ethernetAddress.isEmpty()) {
        out += row(tr("Ethernet"), tr("linked, no address yet"));
    } else {
        out += row(tr("Ethernet"), ethernetAddress);
    }

    const QHash<QString, QString> wifi =
            keyValues(readFile(QStringLiteral("/run/trimixxx/wifi")));
    const QString wifiAddress =
            ipv4Of(QNetworkInterface::interfaceFromName(QStringLiteral("wlan0")));
    const QString ssid = wifi.value(QStringLiteral("ssid")).toHtmlEscaped();
    if (wifi.value(QStringLiteral("mode")) == QStringLiteral("hotspot")) {
        // The deck's own network: everything a phone needs to join it and ssh
        // in, on one line, so nobody at the venue needs the repo to hand.
        out += row(tr("Wi-Fi"),
                QStringLiteral("<span class='warn'>%1</span> %2 &middot; %3 %4 "
                               "&middot; %5")
                        .arg(tr("HOTSPOT"),
                                ssid,
                                tr("password"),
                                wifi.value(QStringLiteral("password")).toHtmlEscaped(),
                                wifiAddress.isEmpty()
                                        ? wifi.value(QStringLiteral("address"))
                                                  .toHtmlEscaped()
                                        : wifiAddress));
    } else if (wifiAddress.isEmpty()) {
        out += row(tr("Wi-Fi"), tr("not connected"));
    } else if (ssid.isEmpty()) {
        out += row(tr("Wi-Fi"), wifiAddress);
    } else {
        out += row(tr("Wi-Fi"), QStringLiteral("%1 &middot; %2").arg(ssid, wifiAddress));
    }
    out += QStringLiteral("</table>");

    // ---- system ------------------------------------------------------------
    out += QStringLiteral("<h2>System</h2><table>");
    out += row(tr("CPU"),
            QStringLiteral("%1 %  <span class='spark'>%2</span>")
                    .arg(m_cpuHistory.isEmpty() ? 0.0 : m_cpuHistory.last(), 0, 'f', 0)
                    .arg(sparkline(m_cpuHistory, 100.0)));
    out += row(tr("Memory"),
            QStringLiteral("%1 %  <span class='spark'>%2</span>")
                    .arg(m_memHistory.isEmpty() ? 0.0 : m_memHistory.last(), 0, 'f', 0)
                    .arg(sparkline(m_memHistory, 100.0)));
    out += row(tr("Temperature"),
            QStringLiteral("%1 °C  <span class='spark'>%2</span>")
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
    if (!m_throttled.isEmpty()) {
        const bool clean = m_throttled.endsWith(QStringLiteral("0x0"));
        out += row(tr("Throttling"),
                clean ? m_throttled
                      : QStringLiteral("<span class='warn'>%1</span>").arg(m_throttled));
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

QString WDeckDiagnostics::adjustHtml() const {
    // ---- adjust ------------------------------------------------------------
    //
    // The one part of the page that changes anything, and first, so it is on
    // screen as the page opens with the line saying how to use it right under
    // it. The range shows only while adjusting, which is when it is the answer
    // to "why has it stopped moving".
    QString out = QStringLiteral("<h2>%1</h2><table>").arg(tr("Adjust"));

    const bool adjustingOutput = m_adjusting == Adjusting::Output;
    QString output = m_pLevels->hasOutput() ? decibels(m_pLevels->outputDb())
                                            : QStringLiteral("&mdash;");
    if (adjustingOutput) {
        output += QStringLiteral(" &nbsp;<span class='hint'>%1</span>")
                          .arg(tr("%1 to %2").arg(decibels(DeckLevels::kOutputMinDb), decibels(DeckLevels::kOutputMaxDb)));
    }
    out += levelRow(tr("Output"), output, adjustingOutput);

    // Beside the output because above unity the trim is what causes it.
    QString clipping;
    if (!m_pClipping->valid()) {
        clipping = QStringLiteral("&mdash;");
    } else if (m_pClipping->toBool()) {
        clipping = QStringLiteral("<span class='warn'>%1</span>").arg(tr("now"));
    } else if (!m_sinceClipping.isValid()) {
        clipping = tr("none");
    } else {
        const qint64 seconds = m_sinceClipping.elapsed() / 1000;
        const QString ago = seconds < 60 ? tr("%1 s ago").arg(seconds)
                : seconds < 3600         ? tr("%1 min ago").arg(seconds / 60)
                                         : tr("%1 h ago").arg(seconds / 3600);
        // Within the minute is recent enough to be the trim's doing; older is
        // history.
        clipping = seconds < 60 ? QStringLiteral("<span class='warn'>%1</span>").arg(ago) : ago;
    }
    out += levelRow(tr("Clipping"), clipping);

    const bool adjustingBrightness = m_adjusting == Adjusting::Brightness;
    QString brightness;
    if (!m_pLevels->hasBacklight()) {
        brightness = QStringLiteral("<span class='hint'>%1</span>").arg(tr("no backlight to set"));
    } else {
        const int percent = m_pLevels->brightness();
        brightness = percent < 0 ? QStringLiteral("&mdash;") : QStringLiteral("%1 %").arg(percent);
        if (adjustingBrightness) {
            brightness += QStringLiteral(" &nbsp;<span class='hint'>%1</span>")
                                  .arg(tr("%1 to %2 %")
                                                  .arg(DeckLevels::kBrightnessMin)
                                                  .arg(DeckLevels::kBrightnessMax));
        }
    }
    out += levelRow(tr("Brightness"), brightness, adjustingBrightness);
    out += QStringLiteral("</table>");

    QString hint;
    switch (m_adjusting) {
    case Adjusting::Nothing:
        hint = m_pLevels->hasBacklight()
                ? tr("Press the encoder to adjust the output, then the brightness.")
                : tr("Press the encoder to adjust the output.");
        break;
    case Adjusting::Output:
        hint = m_pLevels->hasBacklight()
                ? tr("Turn to set the output. Press for the brightness, BACK when done.")
                : tr("Turn to set the output. Press or BACK when done.");
        break;
    case Adjusting::Brightness:
        hint = tr("Turn to set the brightness. Press or BACK when done.");
        break;
    }
    out += QStringLiteral("<p class='%1'>%2</p>")
                   .arg(m_adjusting == Adjusting::Nothing ? QStringLiteral("hint")
                                                          : QStringLiteral("live"),
                           hint.toHtmlEscaped());
    return out;
}

} // namespace deck
} // namespace mixxx

#include "moc_wdeckdiagnostics.cpp"
