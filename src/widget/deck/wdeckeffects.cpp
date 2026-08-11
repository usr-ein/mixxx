#include "widget/deck/wdeckeffects.h"

#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>

#include "control/controlproxy.h"
#include "moc_wdeckeffects.cpp"

namespace {
/// The pedal bus. Unit 2 rather than 1 because unit 1 is the deck's own, wired
/// to [Channel1]; this one is routed to [Auxiliary1] and to nothing else.
const QString kUnit = QStringLiteral("[EffectRack1_EffectUnit2]");
const QString kSlot = QStringLiteral("[EffectRack1_EffectUnit2_Effect1]");
const QString kAux = QStringLiteral("[Auxiliary1]");

QString statusRow(const QString& label, const QString& value, bool ok) {
    return QStringLiteral("<tr><td class='k'>%1</td><td class='%2'>%3</td></tr>")
            .arg(label.toHtmlEscaped(), ok ? QStringLiteral("ok") : QStringLiteral("warn"), value);
}
} // namespace

namespace mixxx {
namespace deck {

WDeckEffects::WDeckEffects(QWidget* pParent)
        : QTextBrowser(pParent) {
    setObjectName(QStringLiteral("DeckEffects"));
    setOpenExternalLinks(false);
    setOpenLinks(false);

    // Same finger-drag as the diagnostics page: a QTextBrowser scrolls with a
    // scrollbar and a wheel, and this deck has neither.
    QScroller::grabGesture(viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties properties = QScroller::scroller(viewport())->scrollerProperties();
    properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
            QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
            QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    QScroller::scroller(viewport())->setScrollerProperties(properties);

    m_pAuxConfigured = std::make_unique<ControlProxy>(kAux, QStringLiteral("input_configured"));
    m_pAuxMainMix = std::make_unique<ControlProxy>(kAux, QStringLiteral("main_mix"));
    m_pAuxVu = std::make_unique<ControlProxy>(kAux, QStringLiteral("vu_meter"));
    m_pUnitRouted = std::make_unique<ControlProxy>(
            kUnit, QStringLiteral("group_[Auxiliary1]_enable"));
    m_pUnitEnabled = std::make_unique<ControlProxy>(kUnit, QStringLiteral("enabled"));
    m_pMixMode = std::make_unique<ControlProxy>(kUnit, QStringLiteral("mix_mode"));
    m_pEffectLoaded = std::make_unique<ControlProxy>(kSlot, QStringLiteral("loaded"));

    // Wet first because it is the one that gets moved during a set, and the
    // send before the effect's own parameters because that is the order the
    // signal meets them: aux gain feeds the tank, the tank's controls shape
    // what comes out, wet decides how much of it is returned.
    addKnob(tr("Wet"), kUnit, QStringLiteral("mix"));
    addKnob(tr("Aux gain"), kAux, QStringLiteral("pregain"));
    addKnob(tr("Meta"), kUnit, QStringLiteral("super1"));
    // Reverb's four, in manifest order. Labelled rather than numbered because
    // parameter3 tells you nothing at arm's length.
    addKnob(tr("Decay"), kSlot, QStringLiteral("parameter1"));
    addKnob(tr("Bandwidth"), kSlot, QStringLiteral("parameter2"));
    addKnob(tr("Damping"), kSlot, QStringLiteral("parameter3"));
    addKnob(tr("Send"), kSlot, QStringLiteral("parameter4"));

    // 5 Hz. Fast enough that a value tracks the encoder without lagging behind
    // the finger, slow enough to be free.
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const int scrollPosition = verticalScrollBar()->value();
        setHtml(html());
        verticalScrollBar()->setValue(scrollPosition);
    });
}

WDeckEffects::~WDeckEffects() = default;

void WDeckEffects::addKnob(const QString& label,
        const QString& group,
        const QString& item,
        double step) {
    Knob knob;
    knob.label = label;
    knob.group = group;
    knob.item = item;
    knob.step = step;
    knob.pControl = new ControlProxy(group, item, this);
    m_knobs.append(knob);
}

void WDeckEffects::setActive(bool active) {
    if (active) {
        const int scrollPosition = verticalScrollBar()->value();
        setHtml(html());
        verticalScrollBar()->setValue(scrollPosition);
        m_timer.start();
    } else {
        m_timer.stop();
        // Leaving the page mid-adjust and coming back to find the encoder still
        // wired to a value is a trap, so the mode does not survive the exit.
        m_adjusting = false;
    }
}

void WDeckEffects::moveSelection(int steps) {
    if (m_knobs.isEmpty()) {
        return;
    }
    if (!m_adjusting) {
        m_selected = qBound(0, m_selected + steps, m_knobs.size() - 1);
    } else {
        const Knob& knob = m_knobs.at(m_selected);
        if (knob.pControl->valid()) {
            // Parameter space, not value space: one detent then means the same
            // fraction of travel whether the control runs 0..1 like a mix knob
            // or 0..4 like an audio-taper gain.
            const double parameter = knob.pControl->getParameter() + steps * knob.step;
            knob.pControl->setParameter(qBound(0.0, parameter, 1.0));
        }
    }
    const int scrollPosition = verticalScrollBar()->value();
    setHtml(html());
    verticalScrollBar()->setValue(scrollPosition);
}

void WDeckEffects::toggleAdjust() {
    if (m_knobs.isEmpty()) {
        return;
    }
    m_adjusting = !m_adjusting;
    const int scrollPosition = verticalScrollBar()->value();
    setHtml(html());
    verticalScrollBar()->setValue(scrollPosition);
}

QString WDeckEffects::value(const ControlProxy* pControl, int decimals) {
    if (pControl == nullptr || !pControl->valid()) {
        return QStringLiteral("&mdash;");
    }
    return QStringLiteral("%1").arg(pControl->get(), 0, 'f', decimals);
}

QString WDeckEffects::onOff(const ControlProxy* pControl) {
    if (pControl == nullptr || !pControl->valid()) {
        return QStringLiteral("&mdash;");
    }
    return pControl->toBool() ? QStringLiteral("yes") : QStringLiteral("NO");
}

QString WDeckEffects::bar(double fraction, int width) {
    const int filled = qBound(0, static_cast<int>(fraction * width + 0.5), width);
    QString out;
    for (int i = 0; i < width; ++i) {
        out += i < filled ? QChar(0x2588) : QChar(0x2591);
    }
    return out;
}

QString WDeckEffects::html() const {
    QString out = QStringLiteral(
            "<style>"
            "body { color:#dddddd; font-family:'MesloLGL Nerd Font'; font-size:15px; }"
            "h2 { color:#88ff00; font-size:17px; margin-top:16px; }"
            "td { padding:2px 10px 2px 0; }"
            "td.k { color:#888888; }"
            "td.ok { color:#dddddd; }"
            ".warn { color:#ff6600; }"
            ".sel { color:#88ff00; }"
            ".adj { color:#000000; background-color:#88ff00; }"
            "</style>");

    // ---- the signal path, in the order it is travelled -------------------
    //
    // Every one of these being wrong sounds exactly like every other one being
    // wrong, so none of them is left to be inferred from silence.
    out += QStringLiteral("<h2>%1</h2><table>").arg(tr("Pedal bus"));

    const bool configured = m_pAuxConfigured->valid() && m_pAuxConfigured->toBool();
    out += statusRow(tr("Aux input"),
            configured ? tr("open") : tr("NOT CONFIGURED"),
            configured);

    const bool onMain = m_pAuxMainMix->valid() && m_pAuxMainMix->toBool();
    out += statusRow(tr("Aux to main"), onOff(m_pAuxMainMix.get()), onMain);

    const double vu = m_pAuxVu->valid() ? m_pAuxVu->get() : 0.0;
    out += statusRow(tr("Aux level"),
            QStringLiteral("<span style='color:#88ff00'>%1</span>").arg(bar(vu)),
            true);

    const bool routed = m_pUnitRouted->valid() && m_pUnitRouted->toBool();
    out += statusRow(tr("Unit 2 routed"), onOff(m_pUnitRouted.get()), routed);

    const bool enabled = m_pUnitEnabled->valid() && m_pUnitEnabled->toBool();
    out += statusRow(tr("Unit 2 enabled"), onOff(m_pUnitEnabled.get()), enabled);

    // The whole point of the fork's patch. Anything but WET here means the dry
    // is coming back with the effect, and the mixer already has a dry.
    const int mode = m_pMixMode->valid() ? static_cast<int>(m_pMixMode->get()) : -1;
    const QString modeName = mode == 0 ? QStringLiteral("DRY/WET")
            : mode == 1                ? QStringLiteral("DRY+WET")
            : mode == 2                ? QStringLiteral("WET")
                                       : QStringLiteral("&mdash;");
    out += statusRow(tr("Mix mode"), modeName, mode == 2);

    const bool loaded = m_pEffectLoaded->valid() && m_pEffectLoaded->toBool();
    out += statusRow(tr("Effect loaded"), onOff(m_pEffectLoaded.get()), loaded);
    out += QStringLiteral("</table>");

    // ---- the knobs -------------------------------------------------------
    out += QStringLiteral("<h2>%1</h2><table>").arg(tr("Adjust"));
    for (int i = 0; i < m_knobs.size(); ++i) {
        const Knob& knob = m_knobs.at(i);
        const bool selected = i == m_selected;
        const double parameter = knob.pControl->valid() ? knob.pControl->getParameter() : 0.0;
        const QString cursor = selected
                ? (m_adjusting ? QStringLiteral("&#9654;") : QStringLiteral("&#9655;"))
                : QStringLiteral("&nbsp;");
        const QString cls = !selected ? QStringLiteral("k")
                : m_adjusting        ? QStringLiteral("adj")
                                     : QStringLiteral("sel");
        out += QStringLiteral(
                "<tr><td class='%1'>%2 %3</td><td>%4</td>"
                "<td><span style='color:#88ff00'>%5</span></td></tr>")
                       .arg(cls,
                               cursor,
                               knob.label.toHtmlEscaped(),
                               value(knob.pControl),
                               bar(parameter));
    }
    out += QStringLiteral("</table>");

    out += QStringLiteral("<p class='k'>%1</p>")
                   .arg(m_adjusting
                                   ? tr("Turn to change the value. Press to stop.")
                                   : tr("Turn to pick a row. Press to adjust it."));
    return out;
}

} // namespace deck
} // namespace mixxx
