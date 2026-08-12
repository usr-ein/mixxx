#include "widget/deck/wdeckrack.h"

#include <QDate>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

#include "control/controlproxy.h"
#include "effects/defs.h"
#include "moc_wdeckrack.cpp"

namespace {

/// The pedal bus. Unit 2 rather than 1 because unit 1 is the deck's own, wired
/// to [Channel1]; this one is routed to [Auxiliary1] and to nothing else.
const QString kUnit = QStringLiteral("[EffectRack1_EffectUnit2]");

QString slotGroup(int slot) {
    return QStringLiteral("[EffectRack1_EffectUnit2_Effect%1]").arg(slot + 1);
}

// Palette, sampled off the reference Winamp skins (PRD §9.3). Winamp98 gives
// the bevel, Winamp5 Classified the steel, Purple Glow the gloss.
const QColor kPanelInk(0x22, 0x22, 0x22);
const QColor kPanelInkLight(0xEE, 0xEE, 0xEE);
const QColor kLcdBack(0x00, 0x22, 0x63);
const QColor kLcdInk(0x7E, 0xB6, 0xFF);
const QColor kLcdEdge(0x28, 0x4A, 0x89);

struct MaterialColors {
    QColor base;
    QColor highlight;
    QColor shadow;
};

MaterialColors colorsFor(int material) {
    switch (material) {
    case 0: // BrushedSteel
        return {QColor(0xAF, 0xB6, 0xC2), QColor(0xCF, 0xD2, 0xDB), QColor(0x8A, 0x90, 0x9B)};
    case 1: // YellowPlastic
        return {QColor(0xE8, 0xC5, 0x1F), QColor(0xF7, 0xE2, 0x7A), QColor(0x8A, 0x74, 0x10)};
    case 2: // GreenPlastic
        return {QColor(0x3F, 0xA6, 0x4B), QColor(0x8F, 0xD8, 0x97), QColor(0x1E, 0x5A, 0x26)};
    default: // PianoBlack
        return {QColor(0x0B, 0x0B, 0x0B), QColor(0x41, 0x41, 0x41), QColor(0x00, 0x00, 0x00)};
    }
}

/// Deterministic per-pixel jitter for the brushed grain. A hash rather than a
/// random number so the texture is identical every time it is rendered — it is
/// cached, but a cache that regenerates differently is a flicker waiting to
/// happen.
int grainAt(int x, int y) {
    const int h = (x * 73856093) ^ (y * 19349663);
    return (h >> 8) & 0x7;
}

} // namespace

namespace mixxx {
namespace deck {

namespace {

/// A kind of module: a preset plus a skin.
///
/// `loadedEffect` is a 1-based index into the VisibleEffects list that ships in
/// `mixxx_config/effects.xml`, because that is the only handle the control API
/// offers -- `loaded_effect` takes a position, not an id. It is the one place
/// this code is coupled to a file it does not own. The shipped order begins
/// whitenoise, tremolo, reverb, pitchshift, phaser, parametriceq,
/// moogladder4filter, metronome, linkwitzrileyeq, loudnesscontour, graphiceq,
/// glitch, flanger, filter, echo -- so reverb is 3, filter 14, echo 15.
struct CatalogueEntry {
    QString name;
    QString shortName;
    int material;
    int loadedEffect;
    bool generatesNewMaterial;
    QVector<QPair<QString, QString>> knobs;  // caption, item
    QVector<QPair<QString, double>> hidden;  // parameterN, value
};

const QVector<CatalogueEntry>& catalogue() {
    static const QVector<CatalogueEntry> kCatalogue = {
            {QStringLiteral("REVERB"),
                    QStringLiteral("RVB"),
                    0,
                    3,
                    true,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("DECAY"), QStringLiteral("parameter1")},
                            {QStringLiteral("BAND"), QStringLiteral("parameter2")},
                            {QStringLiteral("DAMP"), QStringLiteral("parameter3")}},
                    // send_amount wide open: the module's own WET knob is the
                    // level control, and feeding the tank less as well would
                    // make one gesture do two things.
                    {{QStringLiteral("parameter4"), 1.0}}},
            {QStringLiteral("DELAY"),
                    QStringLiteral("DLY"),
                    1,
                    15,
                    true,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("TIME"), QStringLiteral("parameter1")}},
                    // Feedback at zero is what makes this a delay rather than
                    // an echo: one repeat, not a train. Send wide open, as above.
                    {{QStringLiteral("parameter2"), 0.0},
                            {QStringLiteral("parameter3"), 0.0},
                            {QStringLiteral("parameter4"), 1.0}}},
            {QStringLiteral("ECHO"),
                    QStringLiteral("ECH"),
                    2,
                    15,
                    true,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("TIME"), QStringLiteral("parameter1")},
                            {QStringLiteral("FDBK"), QStringLiteral("parameter2")},
                            {QStringLiteral("P-PONG"), QStringLiteral("parameter3")}},
                    {{QStringLiteral("parameter4"), 1.0}}},
            {QStringLiteral("HPF"),
                    QStringLiteral("HPF"),
                    3,
                    14,
                    false,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("CUTOFF"), QStringLiteral("parameter3")},
                            {QStringLiteral("RES"), QStringLiteral("parameter2")}},
                    // lpf pinned wide open, so only the high-pass is in circuit.
                    {{QStringLiteral("parameter1"), 1.0}}},
            {QStringLiteral("LPF"),
                    QStringLiteral("LPF"),
                    3,
                    14,
                    false,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("CUTOFF"), QStringLiteral("parameter1")},
                            {QStringLiteral("RES"), QStringLiteral("parameter2")}},
                    // hpf pinned fully open (down), so only the low-pass acts.
                    {{QStringLiteral("parameter3"), 0.0}}},
    };
    return kCatalogue;
}

} // namespace

WDeckRack::WDeckRack(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("DeckRack"));
    setAutoFillBackground(true);
    setMouseTracking(false);

    m_pMasterMix = std::make_unique<ControlProxy>(kUnit, QStringLiteral("mix"));

    // Long enough not to fire on a firm tap, short enough that it does not feel
    // like the deck ignored you.
    m_longPress.setSingleShot(true);
    m_longPress.setInterval(450);
    connect(&m_longPress, &QTimer::timeout, this, [this] {
        m_heldModule = moduleAt(m_heldPos);
        // A held module cancels whatever else the finger might have started.
        m_dragKnobModule = -1;
        m_scrolling = false;
        update();
    });

    // 10 Hz. The knobs move under the finger from the drag handler, not from
    // here; this is only so a value changed elsewhere -- the master by the
    // encoder, the lock moving after a reorder -- does not sit stale.
    m_refresh.setInterval(100);
    connect(&m_refresh, &QTimer::timeout, this, [this] { update(); });

    m_tapTimer.start();
    syncFromEngine();
}

WDeckRack::~WDeckRack() = default;

void WDeckRack::setActive(bool active) {
    if (active) {
        syncFromEngine();
        m_refresh.start();
    } else {
        m_refresh.stop();
        m_chooserOpen = false;
        m_heldModule = -1;
    }
    update();
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

ControlProxy* WDeckRack::slotControl(int slot, const QString& item) {
    const QString key = QStringLiteral("%1|%2").arg(slot).arg(item);
    auto it = m_controls.find(key);
    if (it != m_controls.end()) {
        return it.value();
    }
    auto* pControl = new ControlProxy(slotGroup(slot), item, this);
    m_controls.insert(key, pControl);
    return pControl;
}

void WDeckRack::syncFromEngine() {
    // The chain is the truth. A slot with something loaded is a module; which
    // module it is comes from the effect index the slot reports, and where two
    // catalogue entries share an effect -- HPF and LPF, Delay and Echo -- the
    // first match wins unless the rack already knew better.
    QVector<Module> found;
    for (int slot = 0; slot < kNumEffectsPerUnit; ++slot) {
        auto* pLoaded = slotControl(slot, QStringLiteral("loaded"));
        if (!pLoaded->valid() || !pLoaded->toBool()) {
            continue;
        }
        const int effect = static_cast<int>(
                slotControl(slot, QStringLiteral("loaded_effect"))->get());
        int type = -1;
        // Prefer what we already had in this slot, so a rack of HPF and LPF
        // survives a refresh instead of collapsing to whichever is listed first.
        for (const Module& previous : m_modules) {
            if (previous.slot == slot && previous.type >= 0 &&
                    catalogue().at(previous.type).loadedEffect == effect) {
                type = previous.type;
                break;
            }
        }
        if (type < 0) {
            for (int i = 0; i < catalogue().size(); ++i) {
                if (catalogue().at(i).loadedEffect == effect) {
                    type = i;
                    break;
                }
            }
        }
        if (type >= 0) {
            found.append({type, slot});
        }
    }
    m_modules = found;
    applyWetLock();
}

void WDeckRack::writeChainToEngine() {
    // Rewrite every slot from m_modules. Reordering is a chain reorder, not a
    // visual one, so this is what makes a drop change the audio.
    for (int i = 0; i < m_modules.size(); ++i) {
        Module& module = m_modules[i];
        const auto& type = catalogue().at(module.type);
        if (module.slot != i) {
            module.slot = i;
            slotControl(i, QStringLiteral("loaded_effect"))->set(type.loadedEffect);
        }
        slotControl(i, QStringLiteral("enabled"))->set(1.0);
        for (const auto& pinned : type.hidden) {
            slotControl(i, pinned.first)->setParameter(pinned.second);
        }
    }
    for (int slot = m_modules.size(); slot < kNumEffectsPerUnit; ++slot) {
        slotControl(slot, QStringLiteral("clear"))->set(1.0);
    }
    applyWetLock();
}

void WDeckRack::applyWetLock() {
    // PRD §3.2. The first module that generates new material destroys the dry,
    // so it runs fully wet and everything after it is free: a later blend mixes
    // two signals that are both already dry-free, and can never resurrect the
    // original.
    bool seen = false;
    for (const Module& module : m_modules) {
        if (module.type < 0) {
            continue;
        }
        const bool killer = catalogue().at(module.type).generatesNewMaterial;
        if (killer && !seen) {
            seen = true;
            slotControl(module.slot, QStringLiteral("wet"))->set(1.0);
        }
    }
}

double WDeckRack::knobValue(int moduleIndex, int knobIndex) const {
    const Module& module = m_modules.at(moduleIndex);
    const auto& spec = catalogue().at(module.type).knobs.at(knobIndex);
    auto* pControl = const_cast<WDeckRack*>(this)->slotControl(module.slot, spec.second);
    return pControl->valid() ? pControl->getParameter() : 0.0;
}

void WDeckRack::setKnobValue(int moduleIndex, int knobIndex, double parameter) {
    const Module& module = m_modules.at(moduleIndex);
    const auto& spec = catalogue().at(module.type).knobs.at(knobIndex);
    slotControl(module.slot, spec.second)->setParameter(qBound(0.0, parameter, 1.0));
}

QString WDeckRack::generatedName() const {
    // The rack's identity is its contents, so the name is derived rather than
    // typed -- a deck with no keyboard should not ask for one.
    QStringList parts;
    for (const Module& module : m_modules) {
        if (module.type >= 0) {
            parts << catalogue().at(module.type).shortName;
        }
    }
    return parts.isEmpty() ? tr("EMPTY RACK") : parts.join(QStringLiteral("  →  "));
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

int WDeckRack::rackHeight() const {
    return height() - kNameBarHeight - kBezelHeight;
}

QRect WDeckRack::moduleRect(int index) const {
    const int x = index * (kModuleWidth + kGutter) - m_scroll;
    return QRect(x, kNameBarHeight, kModuleWidth, rackHeight());
}

QRect WDeckRack::masterRect() const {
    return QRect(width() - kModuleWidth, kNameBarHeight, kModuleWidth, rackHeight());
}

QRect WDeckRack::plusRect() const {
    if (m_modules.size() >= kNumEffectsPerUnit) {
        return QRect(); // full: the (+) is not drawn and not tappable
    }
    const int x = m_modules.size() * (kModuleWidth + kGutter) - m_scroll;
    return QRect(x + 40, kNameBarHeight + rackHeight() / 2 - 40, 80, 80);
}

QRect WDeckRack::binRect() const {
    const int side = 96;
    return QRect((width() - kModuleWidth) / 2 - side / 2,
            kNameBarHeight + rackHeight() - side - 24,
            side,
            side);
}

int WDeckRack::scrollSpan() const {
    const int content = (m_modules.size() + 1) * (kModuleWidth + kGutter);
    const int viewport = width() - kModuleWidth;
    return qMax(0, content - viewport);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void WDeckRack::paintBevel(QPainter* pPainter, const QRect& rect, bool raised) {
    const QColor light = raised ? QColor(255, 255, 255, 140) : QColor(0, 0, 0, 150);
    const QColor dark = raised ? QColor(0, 0, 0, 150) : QColor(255, 255, 255, 120);
    pPainter->setPen(light);
    pPainter->drawLine(rect.topLeft(), rect.topRight());
    pPainter->drawLine(rect.topLeft(), rect.bottomLeft());
    pPainter->setPen(dark);
    pPainter->drawLine(rect.bottomLeft(), rect.bottomRight());
    pPainter->drawLine(rect.topRight(), rect.bottomRight());
}

void WDeckRack::paintEngraved(QPainter* pPainter,
        const QRect& rect,
        int flags,
        const QString& text,
        const QColor& ink) {
    pPainter->setPen(QColor(255, 255, 255, 90));
    pPainter->drawText(rect.translated(1, 1), flags, text);
    pPainter->setPen(ink);
    pPainter->drawText(rect, flags, text);
}

void WDeckRack::paintChrome(QPainter* pPainter, const QRect& rect, Material material) {
    const MaterialColors colors = colorsFor(static_cast<int>(material));

    // Every panel gets a vertical gradient. Without it a large flat area reads
    // as a rectangle of colour rather than an object.
    QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
    gradient.setColorAt(0.0, colors.highlight);
    gradient.setColorAt(0.45, colors.base);
    gradient.setColorAt(1.0, colors.shadow);
    pPainter->fillRect(rect, gradient);

    if (material == Material::BrushedSteel) {
        // The grain is the whole trick: without it this is just grey. One-pixel
        // horizontal streaks, a few percent of lightness either way.
        for (int y = rect.top(); y < rect.bottom(); ++y) {
            const int jitter = grainAt(0, y) - 3;
            pPainter->setPen(QColor(255, 255, 255, qMax(0, 10 + jitter * 3)));
            pPainter->drawLine(rect.left() + 2, y, rect.right() - 2, y);
            y += 1;
        }
    } else if (material == Material::PianoBlack) {
        // Gloss is one specular streak down the upper left, and a bevel bright
        // enough to catch the eye against the black.
        QLinearGradient sheen(rect.topLeft(), rect.bottomRight());
        sheen.setColorAt(0.0, QColor(255, 255, 255, 60));
        sheen.setColorAt(0.25, QColor(255, 255, 255, 12));
        sheen.setColorAt(0.5, QColor(255, 255, 255, 0));
        pPainter->fillRect(rect.adjusted(0, 0, -rect.width() / 2, 0), sheen);
    } else {
        // Moulded plastic: a soft gloss band across the top third, no grain.
        QLinearGradient band(rect.topLeft(), QPoint(rect.left(), rect.top() + rect.height() / 3));
        band.setColorAt(0.0, QColor(255, 255, 255, 70));
        band.setColorAt(1.0, QColor(255, 255, 255, 0));
        pPainter->fillRect(QRect(rect.left(), rect.top(), rect.width(), rect.height() / 3), band);
    }

    paintBevel(pPainter, rect.adjusted(0, 0, -1, -1), true);
    paintBevel(pPainter, rect.adjusted(3, 3, -4, -4), false);

    // Four screws, drawn rather than an asset.
    const int inset = 12;
    for (const QPoint& centre : {QPoint(rect.left() + inset, rect.top() + inset),
                 QPoint(rect.right() - inset, rect.top() + inset),
                 QPoint(rect.left() + inset, rect.bottom() - inset),
                 QPoint(rect.right() - inset, rect.bottom() - inset)}) {
        const QRect head(centre.x() - 5, centre.y() - 5, 10, 10);
        pPainter->setPen(Qt::NoPen);
        pPainter->setBrush(colors.shadow.darker(115));
        pPainter->drawEllipse(head);
        paintBevel(pPainter, head, false);
        pPainter->setPen(QPen(colors.shadow.darker(150), 1));
        pPainter->drawLine(head.left() + 2, centre.y(), head.right() - 2, centre.y());
    }
}

const QPixmap& WDeckRack::chromeFor(Material material) {
    const int key = static_cast<int>(material) * 10000 + rackHeight();
    auto it = m_chrome.find(key);
    if (it != m_chrome.end()) {
        return it.value();
    }
    QPixmap pixmap(kModuleWidth, rackHeight());
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintChrome(&painter, QRect(0, 0, kModuleWidth, rackHeight()), material);
    m_chrome.insert(key, pixmap);
    return m_chrome[key];
}

void WDeckRack::paintKnob(QPainter* pPainter,
        const QRect& rect,
        double parameter,
        const QString& caption,
        Material material,
        bool locked) {
    const MaterialColors colors = colorsFor(static_cast<int>(material));
    const QPoint centre = rect.center();
    const int radius = rect.width() / 2 - 2;

    // Recessed trough, then the value arc riding in it. 270 degrees of travel,
    // starting bottom-left, which is where a knob's travel is expected to be.
    const int startAngle = 225 * 16;
    const int span = -270 * 16;
    pPainter->setBrush(Qt::NoBrush);
    pPainter->setPen(QPen(QColor(0, 0, 0, 110), 4));
    pPainter->drawArc(rect.adjusted(-4, -4, 4, 4), startAngle, span);
    pPainter->setPen(QPen(QColor(0x88, 0xFF, 0x00), 3));
    pPainter->drawArc(rect.adjusted(-4, -4, 4, 4),
            startAngle,
            static_cast<int>(span * parameter));

    // The cap.
    QRadialGradient cap(QPointF(centre) - QPointF(radius / 3.0, radius / 3.0), radius * 1.6);
    cap.setColorAt(0.0, colors.highlight);
    cap.setColorAt(1.0, colors.shadow);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(cap);
    pPainter->drawEllipse(centre, radius, radius);
    pPainter->setPen(QPen(QColor(0, 0, 0, 120), 1));
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawEllipse(centre, radius, radius);

    if (locked) {
        // A knob that silently refuses to move is a fault report; one that is
        // visibly bolted down is a design. So: a slotted screw head instead of
        // a grip (PRD §6).
        pPainter->setPen(QPen(colors.shadow.darker(160), 3));
        pPainter->drawLine(centre.x() - radius / 2, centre.y(), centre.x() + radius / 2, centre.y());
    } else {
        const double angle = qDegreesToRadians(225.0 - 270.0 * parameter);
        pPainter->setPen(QPen(QColor(0x11, 0x11, 0x11), 3));
        pPainter->drawLine(centre,
                centre + QPoint(static_cast<int>(qCos(angle) * radius * 0.75),
                                static_cast<int>(-qSin(angle) * radius * 0.75)));
    }

    QFont font = pPainter->font();
    font.setPixelSize(13);
    font.setBold(true);
    pPainter->setFont(font);
    const QRect captionRect(rect.left() - 20, rect.bottom() + 4, rect.width() + 40, 18);
    const bool dark = material == Material::PianoBlack;
    paintEngraved(pPainter,
            captionRect,
            Qt::AlignCenter,
            locked ? caption + QStringLiteral(" · LOCKED") : caption,
            dark ? kPanelInkLight : kPanelInk);
}

void WDeckRack::paintModule(QPainter* pPainter, int index) {
    const Module& module = m_modules.at(index);
    const auto& type = catalogue().at(module.type);
    const QRect rect = moduleRect(index);
    if (!rect.intersects(QRect(0, 0, width() - kModuleWidth, height()))) {
        return; // scrolled off; nothing to draw
    }

    pPainter->drawPixmap(rect.topLeft(),
            chromeFor(static_cast<Material>(type.material)));

    // The dry-killer lock is a property of position, so it is recomputed here
    // rather than stored: the first module that generates new material.
    bool locked = false;
    for (int i = 0; i <= index; ++i) {
        if (catalogue().at(m_modules.at(i).type).generatesNewMaterial) {
            locked = (i == index);
            break;
        }
    }

    // The big knob first, then the rest in a column. 204 px is not wide enough
    // for two side by side with captions that can be read at arm's length, so
    // they stack.
    const int top = rect.top() + 46;
    for (int k = 0; k < type.knobs.size(); ++k) {
        const int size = k == 0 ? 78 : 58;
        const int y = top + (k == 0 ? 0 : 104 + (k - 1) * 82);
        const QRect knobRect(rect.center().x() - size / 2, y, size, size);
        paintKnob(pPainter,
                knobRect,
                knobValue(index, k),
                type.knobs.at(k).first,
                static_cast<Material>(type.material),
                k == 0 && locked);
    }

    // Name plate, bottom left (PRD §6).
    QFont font = pPainter->font();
    font.setPixelSize(20);
    font.setBold(true);
    pPainter->setFont(font);
    const QRect plate(rect.left() + 14, rect.bottom() - 52, rect.width() - 28, 28);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(QColor(0, 0, 0, 40));
    pPainter->drawRoundedRect(plate, 3, 3);
    paintBevel(pPainter, plate, false);
    paintEngraved(pPainter,
            plate,
            Qt::AlignCenter,
            type.name,
            type.material == 3 ? kPanelInkLight : kPanelInk);

    if (m_heldModule == index) {
        // Held: dashed outline, and it follows the finger.
        pPainter->setBrush(Qt::NoBrush);
        pPainter->setPen(QPen(QColor(0x88, 0xFF, 0x00), 3, Qt::DashLine));
        pPainter->drawRect(rect.adjusted(2, 2, -2, -2));
    }
}

void WDeckRack::paintMaster(QPainter* pPainter) {
    const QRect rect = masterRect();
    pPainter->drawPixmap(rect.topLeft(), chromeFor(Material::BrushedSteel));

    const double level = m_pMasterMix->valid() ? m_pMasterMix->getParameter() : 0.0;
    const bool muted = m_mutedLevel >= 0.0;
    const QRect knobRect(rect.center().x() - 55, rect.top() + 70, 110, 110);
    paintKnob(pPainter,
            knobRect,
            muted ? 0.0 : level,
            muted ? tr("MUTED") : tr("MASTER"),
            Material::BrushedSteel,
            false);

    QFont font = pPainter->font();
    font.setPixelSize(20);
    font.setBold(true);
    pPainter->setFont(font);
    const QRect plate(rect.left() + 14, rect.bottom() - 52, rect.width() - 28, 28);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(muted ? QColor(0x60, 0x00, 0x00, 90) : QColor(0, 0, 0, 40));
    pPainter->drawRoundedRect(plate, 3, 3);
    paintBevel(pPainter, plate, false);
    paintEngraved(pPainter, plate, Qt::AlignCenter, tr("OUT"), kPanelInk);

    font.setPixelSize(13);
    font.setBold(false);
    pPainter->setFont(font);
    pPainter->setPen(QColor(0x55, 0x55, 0x55));
    pPainter->drawText(QRect(rect.left(), rect.bottom() - 96, rect.width(), 20),
            Qt::AlignCenter,
            tr("encoder · press to mute"));
}

void WDeckRack::paintNameBar(QPainter* pPainter) {
    const QRect rect(0, 0, width(), kNameBarHeight);
    pPainter->fillRect(rect, kLcdBack);
    pPainter->setPen(kLcdEdge);
    pPainter->drawLine(rect.bottomLeft(), rect.bottomRight());

    QFont font = pPainter->font();
    font.setPixelSize(22);
    font.setBold(true);
    pPainter->setFont(font);
    pPainter->setPen(kLcdInk);
    pPainter->drawText(rect.adjusted(20, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft, generatedName());

    font.setPixelSize(15);
    font.setBold(false);
    pPainter->setFont(font);
    pPainter->setPen(kLcdEdge.lighter(160));
    pPainter->drawText(rect.adjusted(20, 0, -20, 0),
            Qt::AlignVCenter | Qt::AlignRight,
            QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
}

void WDeckRack::paintChooser(QPainter* pPainter) {
    pPainter->fillRect(rect(), QColor(0, 0, 0, 200));
    const int columns = catalogue().size();
    const int cellWidth = 150;
    const int cellHeight = 260;
    const int totalWidth = columns * (cellWidth + 12);
    int x = (width() - totalWidth) / 2;
    const int y = (height() - cellHeight) / 2;
    for (int i = 0; i < columns; ++i) {
        const QRect cell(x, y, cellWidth, cellHeight);
        // The module's own chrome in miniature: you pick the thing you are
        // going to see, not a row of text.
        paintChrome(pPainter, cell, static_cast<Material>(catalogue().at(i).material));
        QFont font = pPainter->font();
        font.setPixelSize(20);
        font.setBold(true);
        pPainter->setFont(font);
        paintEngraved(pPainter,
                cell.adjusted(0, cellHeight - 50, 0, 0),
                Qt::AlignHCenter | Qt::AlignTop,
                catalogue().at(i).name,
                catalogue().at(i).material == 3 ? kPanelInkLight : kPanelInk);
        x += cellWidth + 12;
    }
}

void WDeckRack::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(0x0C, 0x0C, 0x0C));

    for (int i = 0; i < m_modules.size(); ++i) {
        paintModule(&painter, i);
    }

    const QRect plus = plusRect();
    if (plus.isValid() && !m_modules.isEmpty()) {
        painter.setPen(QPen(QColor(0x88, 0xFF, 0x00), 4));
        painter.drawEllipse(plus);
        painter.drawLine(plus.center() - QPoint(20, 0), plus.center() + QPoint(20, 0));
        painter.drawLine(plus.center() - QPoint(0, 20), plus.center() + QPoint(0, 20));
    } else if (m_modules.isEmpty()) {
        painter.setPen(QPen(QColor(0x88, 0xFF, 0x00), 4));
        const QRect centred(width() / 2 - 150, height() / 2 - 60, 80, 80);
        painter.drawEllipse(centred);
        painter.drawLine(centred.center() - QPoint(20, 0), centred.center() + QPoint(20, 0));
        painter.drawLine(centred.center() - QPoint(0, 20), centred.center() + QPoint(0, 20));
        QFont font = painter.font();
        font.setPixelSize(20);
        painter.setFont(font);
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.drawText(QRect(centred.left() - 100, centred.bottom() + 16, 280, 30),
                Qt::AlignCenter,
                tr("Add an effect"));
    }

    paintMaster(&painter);
    paintNameBar(&painter);

    if (m_heldModule >= 0) {
        // The bin exists only while something is held, so it cannot be hit by
        // accident the rest of the time.
        const QRect bin = binRect();
        painter.setBrush(QColor(0x30, 0x00, 0x00, 220));
        painter.setPen(QPen(QColor(0xFF, 0x55, 0x55), 3));
        painter.drawRoundedRect(bin, 8, 8);
        painter.drawLine(bin.left() + 24, bin.top() + 28, bin.right() - 24, bin.top() + 28);
        painter.drawRect(QRect(bin.left() + 30, bin.top() + 28, bin.width() - 60, bin.height() - 52));
        QFont font = painter.font();
        font.setPixelSize(13);
        painter.setFont(font);
        painter.setPen(QColor(0xFF, 0x99, 0x99));
        painter.drawText(bin.adjusted(0, bin.height() - 18, 0, 0), Qt::AlignHCenter | Qt::AlignTop, tr("remove"));
    }

    if (m_chooserOpen) {
        paintChooser(&painter);
    }
}

// ---------------------------------------------------------------------------
// Hit testing and gestures
// ---------------------------------------------------------------------------

int WDeckRack::moduleAt(const QPoint& point) const {
    if (point.x() >= width() - kModuleWidth) {
        return -1; // the master is pinned there and is not draggable
    }
    for (int i = 0; i < m_modules.size(); ++i) {
        if (moduleRect(i).contains(point)) {
            return i;
        }
    }
    return -1;
}

bool WDeckRack::knobAt(const QPoint& point, int* pModule, int* pKnob) const {
    const int index = moduleAt(point);
    if (index < 0) {
        return false;
    }
    const QRect rect = moduleRect(index);
    const auto& type = catalogue().at(m_modules.at(index).type);
    const int top = rect.top() + 46;
    for (int k = 0; k < type.knobs.size(); ++k) {
        const int size = k == 0 ? 78 : 58;
        const int y = top + (k == 0 ? 0 : 104 + (k - 1) * 82);
        // Generous: a finger is wider than a knob, and the captions below are
        // dead space anyway.
        const QRect hit(rect.center().x() - size / 2 - 12, y - 8, size + 24, size + 26);
        if (hit.contains(point)) {
            *pModule = index;
            *pKnob = k;
            return true;
        }
    }
    return false;
}

void WDeckRack::mousePressEvent(QMouseEvent* pEvent) {
    const QPoint point = pEvent->pos();

    if (m_chooserOpen) {
        const int columns = catalogue().size();
        const int cellWidth = 150;
        const int totalWidth = columns * (cellWidth + 12);
        int x = (width() - totalWidth) / 2;
        const int y = (height() - 260) / 2;
        for (int i = 0; i < columns; ++i) {
            if (QRect(x, y, cellWidth, 260).contains(point)) {
                m_modules.append({i, m_modules.size()});
                writeChainToEngine();
                break;
            }
            x += cellWidth + 12;
        }
        m_chooserOpen = false;
        update();
        return;
    }

    if (plusRect().isValid() && plusRect().contains(point)) {
        m_chooserOpen = true;
        update();
        return;
    }
    if (m_modules.isEmpty() &&
            QRect(width() / 2 - 150, height() / 2 - 60, 80, 80).contains(point)) {
        m_chooserOpen = true;
        update();
        return;
    }

    int moduleIndex = -1;
    int knobIndex = -1;
    if (knobAt(point, &moduleIndex, &knobIndex)) {
        // Double-tap resets. Same knob, quickly, without much movement.
        if (m_tapTimer.elapsed() < 350 && (point - m_lastTapPos).manhattanLength() < 40) {
            const auto& spec = catalogue().at(m_modules.at(moduleIndex).type).knobs.at(knobIndex);
            auto* pControl = slotControl(m_modules.at(moduleIndex).slot, spec.second);
            pControl->reset();
            applyWetLock();
            m_tapTimer.restart();
            update();
            return;
        }
        m_tapTimer.restart();
        m_lastTapPos = point;
        m_dragKnobModule = moduleIndex;
        m_dragKnobIndex = knobIndex;
        m_dragStartValue = knobValue(moduleIndex, knobIndex);
        m_dragStart = point;
        // A knob under the finger is not a candidate for a long press: you
        // cannot pick a module up by its knob.
        return;
    }

    m_dragStart = point;
    m_heldPos = point;
    m_scrollStart = m_scroll;
    m_scrolling = true;
    m_longPress.start();
}

void WDeckRack::mouseMoveEvent(QMouseEvent* pEvent) {
    const QPoint point = pEvent->pos();

    if (m_dragKnobModule >= 0) {
        // Vertical only, so a sloppy diagonal still turns cleanly. Full travel
        // over ~200 px: fine control in one movement.
        const double delta = (m_dragStart.y() - point.y()) / 200.0;
        const Module& module = m_modules.at(m_dragKnobModule);
        const bool locked = m_dragKnobIndex == 0 &&
                catalogue().at(module.type).generatesNewMaterial &&
                [&] {
                    for (const Module& other : m_modules) {
                        if (catalogue().at(other.type).generatesNewMaterial) {
                            return other.slot == module.slot;
                        }
                    }
                    return false;
                }();
        if (!locked) {
            setKnobValue(m_dragKnobModule, m_dragKnobIndex, m_dragStartValue + delta);
            update();
        }
        return;
    }

    if (m_heldModule >= 0) {
        m_heldPos = point;
        update();
        return;
    }

    if (m_scrolling) {
        if ((point - m_dragStart).manhattanLength() > 12) {
            m_longPress.stop(); // moved: this is a scroll, not a hold
        }
        m_scroll = qBound(0, m_scrollStart - (point.x() - m_dragStart.x()), scrollSpan());
        update();
    }
}

void WDeckRack::mouseReleaseEvent(QMouseEvent* pEvent) {
    m_longPress.stop();

    if (m_heldModule >= 0) {
        const QPoint point = pEvent->pos();
        if (binRect().contains(point)) {
            m_modules.removeAt(m_heldModule);
        } else {
            // Drop where the finger is: the slot whose centre is nearest.
            int target = m_modules.size() - 1;
            for (int i = 0; i < m_modules.size(); ++i) {
                if (point.x() < moduleRect(i).center().x()) {
                    target = i;
                    break;
                }
            }
            const Module held = m_modules.at(m_heldModule);
            m_modules.removeAt(m_heldModule);
            m_modules.insert(qBound(0, target, m_modules.size()), held);
        }
        m_heldModule = -1;
        writeChainToEngine();
        update();
    }

    m_dragKnobModule = -1;
    m_dragKnobIndex = -1;
    m_scrolling = false;
}

// ---------------------------------------------------------------------------
// DeckPage
// ---------------------------------------------------------------------------

bool WDeckRack::handleMove(int steps) {
    if (!m_pMasterMix->valid()) {
        return true;
    }
    // The encoder is the master, and only the master. Unmuting by turning would
    // be a surprise, so a turn while muted sets the level it will come back to.
    const double base = m_mutedLevel >= 0.0 ? m_mutedLevel : m_pMasterMix->getParameter();
    const double next = qBound(0.0, base + steps * 0.02, 1.0);
    if (m_mutedLevel >= 0.0) {
        m_mutedLevel = next;
    } else {
        m_pMasterMix->setParameter(next);
    }
    update();
    return true;
}

bool WDeckRack::handleSelect() {
    if (!m_pMasterMix->valid()) {
        return true;
    }
    if (m_mutedLevel >= 0.0) {
        m_pMasterMix->setParameter(m_mutedLevel);
        m_mutedLevel = -1.0;
    } else {
        m_mutedLevel = m_pMasterMix->getParameter();
        m_pMasterMix->setParameter(0.0);
    }
    update();
    return true;
}

bool WDeckRack::handleBack() {
    // BACK leaves the innermost mode first: the chooser, then a held module,
    // and only then the page.
    if (m_chooserOpen) {
        m_chooserOpen = false;
        update();
        return true;
    }
    if (m_heldModule >= 0) {
        m_heldModule = -1;
        update();
        return true;
    }
    return false;
}

} // namespace deck
} // namespace mixxx
