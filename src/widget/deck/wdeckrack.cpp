#include "widget/deck/wdeckrack.h"

#include <QDate>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

#include <cmath>

#include "control/controlproxy.h"
#include "effects/defs.h"
#include "effects/effectchain.h"
#include "effects/effectsmanager.h"
#include "effects/presets/effectchainpreset.h"
#include "effects/presets/effectchainpresetmanager.h"
#include "moc_wdeckrack.cpp"

namespace {

/// The pedal bus. Unit 2 rather than 1 because unit 1 is the deck's own, wired
/// to [Channel1]; this one is routed to [Auxiliary1] and to nothing else.
const QString kUnit = QStringLiteral("[EffectRack1_EffectUnit2]");
/// The same unit, as the index EffectsManager takes: units are 1-based in the
/// control group and 0-based in the list.
constexpr int kUnitNumber = 1;

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

/// The caption/accent colour a material carries. Winamp's metal panels label
/// themselves in gold, which is most of why they read as equipment rather than
/// as grey boxes; the plastics take ink, the gloss takes cyan.
QColor accentFor(int material) {
    switch (material) {
    case 0:
        return QColor(0xF0, 0xC8, 0x60); // gold on steel
    case 1:
        return QColor(0x3A, 0x2E, 0x00); // dark ink on yellow
    case 2:
        return QColor(0x06, 0x2A, 0x0C); // dark ink on green
    default:
        return QColor(0x6C, 0xD8, 0xFF); // cyan on gloss black
    }
}

/// The knob cap, which is deliberately NOT the panel's colour.
///
/// Caps matching their panel is what the first pass did and it made the black
/// module's knobs invisible and the yellow one's gold-on-gold. Real equipment
/// goes the other way -- dark knobs on light panels, light knobs on dark -- for
/// exactly this reason, so the cap contrasts and the pointer can be read at a
/// glance from across the booth.
MaterialColors capColorsFor(int material) {
    switch (material) {
    case 0: // steel panel, graphite cap
        return {QColor(0x4A, 0x4E, 0x57), QColor(0x81, 0x86, 0x92), QColor(0x22, 0x24, 0x2A)};
    case 1: // yellow panel, near-black cap
    case 2: // green panel, near-black cap
        return {QColor(0x2A, 0x2A, 0x2C), QColor(0x60, 0x60, 0x64), QColor(0x0A, 0x0A, 0x0B)};
    default: // gloss black panel, bright silver cap
        return {QColor(0xB6, 0xBC, 0xC6), QColor(0xEC, 0xEF, 0xF4), QColor(0x6E, 0x74, 0x7E)};
    }
}

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

/// What a knob shows beside its caption, if anything.
///
/// A dial position is not a value. "Somewhere past halfway" is fine for a wet
/// blend and useless for a cutoff, so the ones that name a quantity say what it
/// is -- read back off the control in its own units, not recomputed here.
enum class Readout {
    None,
    Hertz,
    /// Also means the knob snaps: see kBeatDivisions.
    Beats,
};

/// The stops a delay time is allowed to take, in beats.
///
/// A continuous time knob is a blur to aim at mid-transition, and every value
/// between 1/4 and 1/2 is one nobody wants. These are the ones that mean
/// something now that the bus carries a tempo.
///
/// The floor is where it is for a reason worth remembering: at 128 BPM a 1/16
/// is 29 ms, shorter than the bus's own 32 ms round trip, so the shortest
/// divisions cannot be latency-compensated by subtraction and will sit late. A
/// late 1/16 is still a 1/16, which is why it is here at all.
const QVector<double> kBeatDivisions =
        {1 / 16.0, 1 / 8.0, 1 / 4.0, 1 / 2.0, 3 / 4.0, 1.0, 2.0, 4.0, 8.0};

const QStringList& beatDivisionLabels() {
    static const QStringList kLabels = {QStringLiteral("1/16"),
            QStringLiteral("1/8"),
            QStringLiteral("1/4"),
            QStringLiteral("1/2"),
            QStringLiteral("3/4"),
            QStringLiteral("1"),
            QStringLiteral("2"),
            QStringLiteral("4"),
            QStringLiteral("8")};
    return kLabels;
}

/// The stop nearest *beats*, compared as a ratio rather than a difference: the
/// list spans a factor of 128, so 1/16 and 1/8 are 0.06 apart while 4 and 8 are
/// 4 apart, and a linear nearest would put almost everything in the top half.
int beatDivisionIndex(double beats) {
    int best = 0;
    double bestDistance = -1.0;
    for (int i = 0; i < kBeatDivisions.size(); ++i) {
        const double distance = qAbs(qLn(qMax(beats, 1e-6) / kBeatDivisions.at(i)));
        if (bestDistance < 0.0 || distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

struct KnobSpec {
    QString caption;
    QString item;
    Readout readout = Readout::None;
};

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
    QVector<KnobSpec> knobs;
    /// Held at this value on every write: parameters the module does not
    /// expose, because exposing them would make it a different module.
    QVector<QPair<QString, double>> hidden;
    /// Written once, when the module first enters the chain. Unlike `hidden`
    /// these are only a starting point -- the knob is free afterwards.
    QVector<QPair<QString, double>> initial;
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
                    {{QStringLiteral("parameter4"), 1.0}},
                    {}},
            {QStringLiteral("DELAY"),
                    QStringLiteral("DLY"),
                    1,
                    15,
                    true,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("TIME"),
                                    QStringLiteral("parameter1"),
                                    Readout::Beats}},
                    // Feedback at zero is what makes this a delay rather than
                    // an echo: one repeat, not a train. Send wide open, as above.
                    //
                    // quantize off, and that one is not optional: it defaults
                    // ON and rounds the time to the nearest 1/4 beat, which
                    // would silently turn every 1/16 and 1/8 the rack offers
                    // into a 1/4.
                    {{QStringLiteral("parameter2"), 0.0},
                            {QStringLiteral("parameter3"), 0.0},
                            {QStringLiteral("parameter4"), 1.0},
                            {QStringLiteral("parameter5"), 0.0}},
                    {}},
            {QStringLiteral("ECHO"),
                    QStringLiteral("ECH"),
                    2,
                    15,
                    true,
                    {{QStringLiteral("WET"), QStringLiteral("wet")},
                            {QStringLiteral("TIME"),
                                    QStringLiteral("parameter1"),
                                    Readout::Beats},
                            {QStringLiteral("FDBK"), QStringLiteral("parameter2")},
                            {QStringLiteral("P-PONG"), QStringLiteral("parameter3")}},
                    // quantize off, as for Delay above.
                    {{QStringLiteral("parameter4"), 1.0},
                            {QStringLiteral("parameter5"), 0.0}},
                    {}},
            // The filters have no WET knob, and that is not an omission: see
            // PRD §15.6. Blending a filter against its own input puts
            // everything in the passband through twice, once filtered and once
            // not, which is a comb rather than a half-open filter. A filter's
            // output is the whole of its output, so `wet` is pinned open and
            // the knob it needs is the one it has -- the cutoff, whose extreme
            // already means "no effect".
            {QStringLiteral("HPF"),
                    QStringLiteral("HPF"),
                    3,
                    14,
                    false,
                    {{QStringLiteral("CUTOFF"), QStringLiteral("parameter3"), Readout::Hertz},
                            {QStringLiteral("RES"), QStringLiteral("parameter2")}},
                    // lpf pinned wide open, so only the high-pass is in circuit.
                    {{QStringLiteral("parameter1"), 1.0},
                            {QStringLiteral("wet"), 1.0}},
                    // Mid-band, so a filter dropped into the rack does
                    // something. Its own default is 13 Hz -- fully open, and
                    // indistinguishable from a module that failed to load.
                    {{QStringLiteral("parameter3"), 0.5}}},
            {QStringLiteral("LPF"),
                    QStringLiteral("LPF"),
                    3,
                    14,
                    false,
                    {{QStringLiteral("CUTOFF"), QStringLiteral("parameter1"), Readout::Hertz},
                            {QStringLiteral("RES"), QStringLiteral("parameter2")}},
                    // hpf pinned fully open (down), so only the low-pass acts.
                    {{QStringLiteral("parameter3"), 0.0},
                            {QStringLiteral("wet"), 1.0}},
                    {{QStringLiteral("parameter1"), 0.5}}},
    };
    return kCatalogue;
}

} // namespace

WDeckRack::WDeckRack(EffectsManager* pEffectsManager,
        UserSettingsPointer pConfig,
        QWidget* pParent)
        : QWidget(pParent),
          m_pEffectsManager(pEffectsManager),
          m_pConfig(pConfig) {
    setObjectName(QStringLiteral("DeckRack"));
    setAutoFillBackground(true);
    setMouseTracking(false);

    m_pMasterMix = std::make_unique<ControlProxy>(kUnit, QStringLiteral("mix"));
    m_pOutputLevel = std::make_unique<ControlProxy>(kUnit, QStringLiteral("output_level"));
    m_pMakeup = std::make_unique<ControlProxy>(kUnit, QStringLiteral("makeup"));
    m_pAuxPregain = std::make_unique<ControlProxy>(
            QStringLiteral("[Auxiliary1]"), QStringLiteral("pregain"));
    if (m_pConfig) {
        m_ringOut = m_pConfig->getValue(
                ConfigKey(QStringLiteral("[TriMixxx]"), QStringLiteral("fx_master_ring_out")),
                true);
    }
    // The chain's own mix is not a gain the rack uses: with a makeup control
    // on the output there is one gain stage, and two would be two things to
    // find when it comes back quiet. Held wide open.
    if (m_pMasterMix->valid()) {
        m_pMasterMix->set(1.0);
    }
    setRingOut(m_ringOut);
    m_pFxBpm = std::make_unique<ControlProxy>(
            QStringLiteral("[EffectTempo]"), QStringLiteral("bpm"));
    m_pFxBpmSource = std::make_unique<ControlProxy>(
            QStringLiteral("[EffectTempo]"), QStringLiteral("source"));

    // Long enough not to fire on a firm tap, short enough that it does not feel
    // like the deck ignored you.
    m_longPress.setSingleShot(true);
    m_longPress.setInterval(450);
    connect(&m_longPress, &QTimer::timeout, this, [this] {
        if (m_browserOpen) {
            // A hold over a saved rack arms it for deletion; the release over
            // it is what commits. Row 0 is Save and is not a rack.
            const int row = rackBrowserRowAt(m_heldPos);
            m_browserArmedRow = row >= 1 ? row : -1;
            m_browserTap = false;
            update();
            return;
        }
        m_heldModule = moduleAt(m_heldPos);
        if (m_heldModule >= 0) {
            m_heldGrab = m_heldPos - moduleRect(m_heldModule).topLeft();
        }
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

    // 40 Hz, and only while a module is actually travelling. 60 would be
    // smoother and this is a Pi 4 repainting the whole rack per tick; at ~100
    // ms of travel the difference is four frames nobody can see.
    m_slideTimer.setInterval(25);
    connect(&m_slideTimer, &QTimer::timeout, this, [this] { stepSlide(); });

    // Two seconds after the last change. Long enough that a knob sweep writes
    // once, short enough that the rack is on disk before a DJ has moved on to
    // the next thing.
    m_persist.setSingleShot(true);
    m_persist.setInterval(2000);
    connect(&m_persist, &QTimer::timeout, this, [this] {
        if (m_pEffectsManager) {
            m_pEffectsManager->saveEffectsXml();
        }
    });

    m_tapTimer.start();
    // Read back the rack the deck was left with -- EffectsManager restores the
    // standard chains from effects.xml before the skin is parsed -- and then
    // write it straight back out.
    //
    // The write is not redundant. StandardEffectChain is the one chain type
    // that does not enable its slots, and EffectPreset has no field to carry
    // the enable either, so a restored rack comes back *drawn and silent*.
    // This is also why it happens here rather than when the page is first
    // opened: a DJ who never opens the Effects page should still have the rack
    // they were using last night.
    syncFromEngine();
    writeChainToEngine();
    m_ready = true;
}

WDeckRack::~WDeckRack() = default;

void WDeckRack::setActive(bool active) {
    if (active) {
        syncFromEngine();
        m_refresh.start();
    } else {
        m_refresh.stop();
        m_slideTimer.stop();
        m_slide.fill(0.0, m_modules.size());
        m_chooserOpen = false;
        m_browserOpen = false;
        m_browserArmedRow = -1;
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
            // Nothing remembered -- a rack just loaded from a preset, or the
            // first sync after startup. The effect index alone does not say
            // which module this is, because two entries can share an effect:
            // HPF and LPF are both Filter. What separates them is the
            // parameters they pin, so match on those. An HPF is a Filter with
            // its low-pass held wide open, and that is readable off the slot.
            for (int i = 0; i < catalogue().size(); ++i) {
                const auto& entry = catalogue().at(i);
                if (entry.loadedEffect != effect || entry.hidden.isEmpty()) {
                    continue;
                }
                bool matches = true;
                for (const auto& pinned : entry.hidden) {
                    const double held = slotControl(slot, pinned.first)->getParameter();
                    if (qAbs(held - pinned.second) > 0.001) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    type = i;
                    break;
                }
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
    m_slide.fill(0.0, m_modules.size());
    applyWetLock();
}

void WDeckRack::writeChainToEngine() {
    // Rewrite every slot from m_modules. Reordering is a chain reorder, not a
    // visual one, so this is what makes a drop change the audio.
    //
    // What each slot holds is read off first, because "move slot 3 to slot 1"
    // is not something the engine can be asked: slots are rewritten in place,
    // and telling one to load an effect loads it *with defaults*. Carrying the
    // values across is what stops a drag two places left from silently
    // resetting the reverb that was just dialled in.
    QVector<int> previousType(kNumEffectsPerUnit, -1);
    QVector<QHash<QString, double>> previousValues(kNumEffectsPerUnit);
    for (const Module& module : m_modules) {
        if (module.slot < 0 || module.slot >= kNumEffectsPerUnit) {
            continue; // straight from the chooser: it has never been in a slot
        }
        previousType[module.slot] = module.type;
        for (const auto& knob : catalogue().at(module.type).knobs) {
            previousValues[module.slot].insert(knob.item,
                    slotControl(module.slot, knob.item)->getParameter());
        }
    }

    for (int i = 0; i < m_modules.size(); ++i) {
        Module& module = m_modules[i];
        const auto& type = catalogue().at(module.type);
        const QHash<QString, double> carried = module.slot >= 0
                ? previousValues.value(module.slot)
                : QHash<QString, double>();
        // Load whenever the slot is not already holding this kind of module.
        // Compared by catalogue type rather than by effect index because two
        // types share an effect -- HPF and LPF are both Filter -- and they pin
        // different parameters, so swapping one for the other has to reload
        // even though `loaded_effect` would not change.
        const bool loading = previousType.value(i) != module.type ||
                !slotControl(i, QStringLiteral("loaded"))->toBool();
        module.slot = i;
        if (loading) {
            // This is the write the rack used to skip. The guard was
            // `module.slot != i`, and the chooser appends a module already
            // carrying the index it is about to take -- so for the one case
            // that needed a load it was false, and the module was drawn but
            // never put into the chain. Leaving the page rebuilt from the
            // chain, found nothing, and emptied the rack.
            slotControl(i, QStringLiteral("loaded_effect"))->set(type.loadedEffect);
        }
        slotControl(i, QStringLiteral("enabled"))->set(1.0);
        for (const auto& pinned : type.hidden) {
            slotControl(i, pinned.first)->setParameter(pinned.second);
        }
        if (carried.isEmpty()) {
            // New to the chain, so it is holding the effect's own defaults.
            // Those are right for a reverb and wrong for a filter, whose
            // default is fully open -- which on a rack full of modules is
            // indistinguishable from one that failed to load.
            for (const auto& start : type.initial) {
                slotControl(i, start.first)->setParameter(start.second);
            }
        }
        for (auto it = carried.constBegin(); it != carried.constEnd(); ++it) {
            slotControl(i, it.key())->setParameter(it.value());
        }
        // Put snapped knobs back on a stop. Values reach a slot from three
        // places -- carried across a reorder as a 0..1 position, restored from
        // a saved rack, or left by an older build with a continuous time knob
        // -- and only the last of those is even approximately on one.
        for (const auto& knob : type.knobs) {
            if (knob.readout == Readout::Beats) {
                auto* pControl = slotControl(i, knob.item);
                pControl->set(kBeatDivisions.at(beatDivisionIndex(pControl->get())));
            }
        }
    }
    for (int slot = m_modules.size(); slot < kNumEffectsPerUnit; ++slot) {
        slotControl(slot, QStringLiteral("clear"))->set(1.0);
    }
    // The chain's mix defaults to 0 -- a fresh or freshly-loaded chain is
    // silent until something opens it -- and the rack's gain is the makeup
    // control, not this. Held wide open, here as well as at construction,
    // because loading a saved rack brings the preset's own value with it.
    if (m_pMasterMix->valid()) {
        m_pMasterMix->set(1.0);
    }
    applyWetLock();
    persistSoon();
}

void WDeckRack::persistSoon() {
    // Not during construction. The first writeChainToEngine() happens before
    // anyone has touched anything, so persisting it would rewrite the file
    // from whatever was just read -- and a single bad read would then be
    // permanent, having overwritten the good copy two seconds into the boot
    // that misread it. Only changes a DJ actually made are worth saving.
    if (!m_ready) {
        return;
    }
    m_persist.start();
}

ControlProxy* WDeckRack::masterControl() const {
    return m_ringOut ? m_pAuxPregain.get() : m_pMakeup.get();
}

void WDeckRack::setRingOut(bool ringOut) {
    // Carry the level across rather than leaving it on the control being
    // abandoned: flipping the rocker changes *where* the gain is applied, not
    // how loud the rack is, and a jump would make it useless mid-transition.
    ControlProxy* pWas = masterControl();
    // The level to carry across is the one the DJ would come back to. While
    // muted the driven control is holding zero and the real level is in
    // m_mutedLevel, so reading the control would arrive at the far side muted
    // at both ends, with the readout showing a level neither stage has.
    double level = 0.5;
    if (m_mutedLevel >= 0.0) {
        level = m_mutedLevel;
    } else if (pWas && pWas->valid()) {
        level = pWas->getParameter();
    }
    m_ringOut = ringOut;

    // The arriving stage takes its level BEFORE the departing one is parked at
    // unity, and the order is the whole fix. The two gains multiply, and
    // nothing makes a pair of separate controls atomic -- each sends its own
    // message and the audio thread can run a buffer between them. Parking
    // first puts both at unity for that buffer, which is full wet whatever the
    // knob said: a burst of effect mid-transition. This way the odd buffer is
    // level x level instead -- momentarily quiet, and nobody hears a dip of
    // one buffer.
    ControlProxy* pNow = masterControl();
    if (pNow && pNow->valid()) {
        pNow->setParameter(m_mutedLevel >= 0.0 ? 0.0 : level);
    }
    // The one not in use is parked at unity, which on a ±12 dB taper is half
    // travel and not zero -- parking it at zero would silence the rack through
    // the stage nothing is driving.
    ControlProxy* pIdle = m_ringOut ? m_pMakeup.get() : m_pAuxPregain.get();
    if (pIdle && pIdle->valid()) {
        pIdle->setParameter(0.5);
    }
    if (m_pConfig) {
        m_pConfig->setValue(
                ConfigKey(QStringLiteral("[TriMixxx]"), QStringLiteral("fx_master_ring_out")),
                m_ringOut);
    }
}

void WDeckRack::applyWetLock() {
    // PRD §3.2. The first module that generates new material destroys the dry,
    // so it runs fully wet and everything after it is free: a later blend mixes
    // two signals that are both already dry-free, and can never resurrect the
    // original.
    bool seen = false;
    for (const Module& module : m_modules) {
        if (module.type < 0 || module.slot < 0) {
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
    if (module.slot < 0) {
        return 0.0; // appended but not yet written to the chain
    }
    const auto& spec = catalogue().at(module.type).knobs.at(knobIndex);
    auto* pControl = const_cast<WDeckRack*>(this)->slotControl(module.slot, spec.item);
    if (!pControl->valid()) {
        return 0.0;
    }
    if (spec.readout == Readout::Beats) {
        // A snapped knob shows its position in the list, not its value. The
        // stops span a factor of 128, so a dial drawn to the value would put
        // everything from 1/16 to 1 in the first eighth of the travel.
        return beatDivisionIndex(pControl->get()) /
                static_cast<double>(kBeatDivisions.size() - 1);
    }
    return pControl->getParameter();
}

QString WDeckRack::knobReadout(int moduleIndex, int knobIndex) const {
    const Module& module = m_modules.at(moduleIndex);
    if (module.slot < 0) {
        return QString();
    }
    const auto& spec = catalogue().at(module.type).knobs.at(knobIndex);
    if (spec.readout == Readout::None) {
        return QString();
    }
    auto* pControl = const_cast<WDeckRack*>(this)->slotControl(module.slot, spec.item);
    if (!pControl->valid()) {
        return QString();
    }
    // Read in the control's own units rather than recomputed from the dial
    // position: the effect owns its scale -- the filter's cutoff is
    // logarithmic between 13 Hz and 22 kHz -- and a second copy of that curve
    // here is a second thing to keep true.
    const double value = pControl->get();
    if (spec.readout == Readout::Hertz) {
        return value >= 1000.0
                ? QStringLiteral("%1k").arg(value / 1000.0, 0, 'f', 1)
                : QStringLiteral("%1Hz").arg(qRound(value));
    }
    if (spec.readout == Readout::Beats) {
        return beatDivisionLabels().at(beatDivisionIndex(value));
    }
    return QString();
}

void WDeckRack::setKnobValue(int moduleIndex, int knobIndex, double parameter) {
    const Module& module = m_modules.at(moduleIndex);
    if (module.slot < 0) {
        return;
    }
    const auto& spec = catalogue().at(module.type).knobs.at(knobIndex);
    slotControl(module.slot, spec.item)->setParameter(qBound(0.0, parameter, 1.0));
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
// Saved racks
//
// Stock Mixxx chain presets, which is why there is so little here: they are
// already XML files under ~/.mixxx/effects/chains -- the SD card on this deck
// -- already loaded at startup, already listed, already deletable. A rack is a
// chain, so saving one is saving a chain preset and nothing needed inventing.
// ---------------------------------------------------------------------------

QStringList WDeckRack::savedRacks() const {
    QStringList names;
    if (!m_pEffectsManager) {
        return names;
    }
    const auto pManager = m_pEffectsManager->getChainPresetManager();
    if (!pManager) {
        return names;
    }
    for (const auto& pPreset : pManager->getPresetsSorted()) {
        // The nameless placeholder Mixxx keeps for "no effects" is not a rack.
        if (pPreset && !pPreset->name().isEmpty()) {
            names << pPreset->name();
        }
    }
    return names;
}

void WDeckRack::saveCurrentRack() {
    if (!m_pEffectsManager) {
        return;
    }
    const auto pManager = m_pEffectsManager->getChainPresetManager();
    EffectChainPointer pChain = m_pEffectsManager->getStandardEffectChain(kUnitNumber);
    if (!pManager || !pChain) {
        return;
    }
    auto pPreset = EffectChainPresetPointer::create(pChain.data());
    // Generated, never typed. A deck with no keyboard should not ask for one,
    // and a rack's identity really is its contents -- the date is what
    // separates two racks built from the same modules.
    //
    // savePresetAs() rather than savePreset(), which puts up a QInputDialog:
    // on this deck that dialog appeared behind a full-screen window with no
    // keyboard to answer it, so the save never happened and the rack never
    // appeared in the list. That is the whole of "saving does not work".
    const QString base = QStringLiteral("%1  %2")
                                 .arg(generatedName().replace(
                                         QStringLiteral("  →  "), QStringLiteral("-")))
                                 .arg(QDate::currentDate().toString(
                                         QStringLiteral("yyyy-MM-dd")));
    // Two racks of the same modules saved on the same day are a normal evening,
    // so the name is made unique here -- savePresetAs() refuses a collision
    // rather than silently overwriting the earlier one.
    QString name = base;
    for (int suffix = 2; !pManager->savePresetAs(pPreset, name) && suffix < 100; ++suffix) {
        name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    }
}

void WDeckRack::loadRack(const QString& name) {
    if (!m_pEffectsManager) {
        return;
    }
    const auto pManager = m_pEffectsManager->getChainPresetManager();
    EffectChainPointer pChain = m_pEffectsManager->getStandardEffectChain(kUnitNumber);
    if (!pManager || !pChain) {
        return;
    }
    const auto pPreset = pManager->getPreset(name);
    if (!pPreset) {
        return;
    }
    pChain->loadChainPreset(pPreset);
    // A loaded preset carries its own mix mode, and a rack that came back as
    // DRY/WET would put the dry into a mixer that already has it.
    pChain->setMixMode(EffectChainMixMode::WetOnly);
    syncFromEngine();
    // And back out again, for the enable a preset cannot carry. Without this a
    // loaded rack draws perfectly and makes no sound.
    writeChainToEngine();
}

void WDeckRack::deleteRack(const QString& name) {
    if (!m_pEffectsManager) {
        return;
    }
    const auto pManager = m_pEffectsManager->getChainPresetManager();
    if (pManager) {
        // Silently, because the rack has already confirmed in its own idiom --
        // the row went red and was released over. deletePreset() would put a
        // QMessageBox on top of a full-screen deck, which is the same trap
        // saving fell into.
        pManager->deletePresetSilently(name);
    }
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

void WDeckRack::startSlide(int index, int fromX) {
    if (index < 0 || index >= m_modules.size()) {
        return;
    }
    if (m_slide.size() != m_modules.size()) {
        m_slide.fill(0.0, m_modules.size());
    }
    m_slide[index] = fromX - moduleRect(index).x();
    m_slideTimer.start();
}

void WDeckRack::stepSlide() {
    bool moving = false;
    for (double& offset : m_slide) {
        // Exponential, not linear: it leaves fast and settles, which is what
        // makes a displacement read as a thing moving rather than a thing
        // being redrawn somewhere else. ~100 ms to under a pixel.
        offset *= 0.55;
        if (qAbs(offset) < 1.0) {
            offset = 0.0;
        } else {
            moving = true;
        }
    }
    if (!moving) {
        m_slideTimer.stop();
    }
    update();
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
    const int index = static_cast<int>(material);
    const MaterialColors colors = colorsFor(index);
    const QColor accent = accentFor(index);

    // A frame within a frame, which is most of what makes the reference skins
    // read as equipment: a dark outer edge, a raised face inside it, and the
    // working area recessed into that again. One bevel alone looks like a
    // button; three depths look like a panel.
    pPainter->fillRect(rect, QColor(0x14, 0x14, 0x16));
    const QRect face = rect.adjusted(3, 3, -3, -3);

    QLinearGradient gradient(face.topLeft(), face.bottomLeft());
    gradient.setColorAt(0.0, colors.highlight);
    gradient.setColorAt(0.45, colors.base);
    gradient.setColorAt(1.0, colors.shadow);
    pPainter->fillRect(face, gradient);

    if (material == Material::BrushedSteel) {
        // The grain is the whole trick: without it this is just grey.
        for (int y = face.top(); y < face.bottom(); y += 2) {
            const int jitter = grainAt(0, y) - 3;
            pPainter->setPen(QColor(255, 255, 255, qMax(0, 10 + jitter * 3)));
            pPainter->drawLine(face.left() + 1, y, face.right() - 1, y);
        }
    } else if (material == Material::PianoBlack) {
        QLinearGradient sheen(face.topLeft(), face.bottomRight());
        sheen.setColorAt(0.0, QColor(255, 255, 255, 70));
        sheen.setColorAt(0.30, QColor(255, 255, 255, 10));
        sheen.setColorAt(0.55, QColor(255, 255, 255, 0));
        pPainter->fillRect(face.adjusted(0, 0, -face.width() / 2, 0), sheen);
    } else {
        QLinearGradient band(face.topLeft(),
                QPoint(face.left(), face.top() + face.height() / 3));
        band.setColorAt(0.0, QColor(255, 255, 255, 80));
        band.setColorAt(1.0, QColor(255, 255, 255, 0));
        pPainter->fillRect(QRect(face.left(), face.top(), face.width(), face.height() / 3), band);
    }

    paintBevel(pPainter, rect, false);
    paintBevel(pPainter, face, true);

    // ---- title strip, at the top -------------------------------------------
    // Hatched, like the ribbed bands in the reference skins, with the name
    // engraved into it. The strip is where the eye lands, so the name belongs
    // here rather than at the foot of the module.
    const QRect strip(face.left() + 5, face.top() + 5, face.width() - 10, 30);
    pPainter->fillRect(strip, QColor(0, 0, 0, 70));
    pPainter->setClipRect(strip);
    pPainter->setPen(QPen(QColor(255, 255, 255, 26), 1));
    for (int x = strip.left() - strip.height(); x < strip.right(); x += 4) {
        pPainter->drawLine(x, strip.bottom(), x + strip.height(), strip.top());
    }
    pPainter->setClipping(false);
    paintBevel(pPainter, strip, false);

    // A lit pilot at the left of the strip: the module is in circuit.
    const QRect lamp(strip.left() + 7, strip.center().y() - 4, 8, 8);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(accent);
    pPainter->drawEllipse(lamp);
    pPainter->setBrush(QColor(255, 255, 255, 150));
    pPainter->drawEllipse(lamp.adjusted(2, 2, -4, -4));

    // ---- the well the knobs sit in -----------------------------------------
    const QRect well(face.left() + 5, strip.bottom() + 6, face.width() - 10,
            face.bottom() - strip.bottom() - 28);
    pPainter->fillRect(well, QColor(0, 0, 0, 46));
    paintBevel(pPainter, well, false);

    // ---- screws, and a corner grip -----------------------------------------
    const int inset = 11;
    for (const QPoint& centre : {QPoint(face.left() + inset, face.top() + inset),
                 QPoint(face.right() - inset, face.top() + inset),
                 QPoint(face.left() + inset, face.bottom() - inset),
                 QPoint(face.right() - inset, face.bottom() - inset)}) {
        const QRect head(centre.x() - 5, centre.y() - 5, 10, 10);
        pPainter->setPen(Qt::NoPen);
        pPainter->setBrush(colors.shadow.darker(118));
        pPainter->drawEllipse(head);
        paintBevel(pPainter, head, false);
        pPainter->setPen(QPen(colors.shadow.darker(155), 1));
        pPainter->drawLine(head.left() + 2, centre.y(), head.right() - 2, centre.y());
    }

    // Diagonal hatch in the bottom right, straight out of the reference skins'
    // resize grip. Pure decoration, and it is the kind of decoration that makes
    // a flat panel look manufactured.
    pPainter->setPen(QPen(QColor(255, 255, 255, 40), 1));
    for (int i = 0; i < 4; ++i) {
        const int off = 6 + i * 4;
        pPainter->drawLine(face.right() - off - 14, face.bottom() - 6,
                face.right() - 6, face.bottom() - off - 14);
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
        bool locked,
        bool greyed) {
    const int index = static_cast<int>(material);
    const MaterialColors colors = capColorsFor(index);
    const bool paleCap = index == 0 ? false : index == 3;
    const QPoint centre = rect.center();
    const int radius = rect.width() / 2 - 2;
    const QColor live = greyed ? QColor(0x77, 0x7D, 0x86) : QColor(0x88, 0xFF, 0x00);

    // Tick marks around the travel, like the scale printed round a real pot.
    // Cheap, and it is most of what tells you where the middle is.
    pPainter->setPen(QPen(QColor(0, 0, 0, 90), 1));
    for (int i = 0; i <= 10; ++i) {
        const double angle = qDegreesToRadians(225.0 - 27.0 * i);
        const QPointF outer(centre.x() + qCos(angle) * (radius + 9),
                centre.y() - qSin(angle) * (radius + 9));
        const QPointF inner(centre.x() + qCos(angle) * (radius + 5),
                centre.y() - qSin(angle) * (radius + 5));
        pPainter->drawLine(inner, outer);
    }

    // Recessed trough, then the value arc riding in it. 270 degrees starting
    // bottom-left, which is where a knob's travel is expected to be.
    const int startAngle = 225 * 16;
    const int span = -270 * 16;
    pPainter->setBrush(Qt::NoBrush);
    pPainter->setPen(QPen(QColor(0, 0, 0, 120), 4));
    pPainter->drawArc(rect.adjusted(-4, -4, 4, 4), startAngle, span);
    pPainter->setPen(QPen(live, 3));
    pPainter->drawArc(rect.adjusted(-4, -4, 4, 4), startAngle, static_cast<int>(span * parameter));

    // The cap: a lit rim, then the body, then a knurled edge.
    QRadialGradient cap(QPointF(centre) - QPointF(radius / 3.0, radius / 3.0), radius * 1.7);
    cap.setColorAt(0.0, colors.highlight);
    cap.setColorAt(0.75, colors.base);
    cap.setColorAt(1.0, colors.shadow);
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(cap);
    pPainter->drawEllipse(centre, radius, radius);

    pPainter->setBrush(Qt::NoBrush);
    pPainter->setPen(QPen(QColor(255, 255, 255, 70), 1));
    pPainter->drawArc(QRect(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2),
            30 * 16,
            140 * 16);
    pPainter->setPen(QPen(QColor(0, 0, 0, 140), 1));
    pPainter->drawArc(QRect(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2),
            210 * 16,
            140 * 16);

    // Knurling: short radial nicks around the cap, the detail that stops a
    // circle reading as a circle.
    pPainter->setPen(QPen(paleCap ? QColor(0, 0, 0, 70) : QColor(255, 255, 255, 45), 1));
    for (int i = 0; i < 24; ++i) {
        const double angle = qDegreesToRadians(i * 15.0);
        pPainter->drawLine(
                QPointF(centre.x() + qCos(angle) * (radius - 4),
                        centre.y() - qSin(angle) * (radius - 4)),
                QPointF(centre.x() + qCos(angle) * (radius - 1),
                        centre.y() - qSin(angle) * (radius - 1)));
    }

    if (locked) {
        // A knob that silently refuses to move is a fault report; one that is
        // visibly bolted down is a design. So: a slotted screw head instead of
        // a pointer (PRD §6).
        pPainter->setPen(QPen(paleCap ? colors.shadow.darker(150) : colors.highlight.lighter(130), 3));
        pPainter->drawLine(centre.x() - radius / 2, centre.y(), centre.x() + radius / 2, centre.y());
    } else {
        const double angle = qDegreesToRadians(225.0 - 270.0 * parameter);
        const QPoint tip(centre.x() + static_cast<int>(qCos(angle) * radius * 0.78),
                centre.y() - static_cast<int>(qSin(angle) * radius * 0.78));
        // Dark halo then a bright core, so the pointer reads on any cap.
        pPainter->setPen(QPen(QColor(0, 0, 0, 190), 4));
        pPainter->drawLine(centre, tip);
        const QColor pointer = greyed ? QColor(0xBB, 0xBB, 0xBB)
                                      : (paleCap ? QColor(0x1A, 0x1A, 0x1A) : QColor(0xF4, 0xF4, 0xF4));
        pPainter->setPen(QPen(pointer, 2));
        pPainter->drawLine(centre, tip);
    }

    const QString label = locked ? caption + QStringLiteral(" ·LOCK") : caption;
    QFont font = pPainter->font();
    font.setBold(true);
    const QRect captionRect(rect.left() - 30, rect.bottom() + 3, rect.width() + 60, 16);
    // A caption carrying a readout is longer than one that is not, and how much
    // longer depends on the value -- "5.4k" and "220Hz" are different widths.
    // Shrink to fit rather than clip: a cutoff silently truncated to "CUTOFF
    // 22" is worse than a small one that reads.
    for (int size = 12; size >= 9; --size) {
        font.setPixelSize(size);
        if (QFontMetrics(font).horizontalAdvance(label) <= captionRect.width() || size == 9) {
            break;
        }
    }
    pPainter->setFont(font);
    paintEngraved(pPainter, captionRect, Qt::AlignCenter, label, accentFor(index));
}

void WDeckRack::paintVu(QPainter* pPainter, const QRect& rect, double level, bool vertical) {
    // Recessed slot first, so an idle meter still reads as a piece of hardware
    // rather than as a gap in the panel.
    pPainter->fillRect(rect, QColor(0x08, 0x09, 0x0A));
    paintBevel(pPainter, rect, false);

    // dB, not amplitude. A linear meter puts everything below −20 dB in the
    // bottom tenth, which is exactly where a reverb tail and a closed filter
    // live -- the two things this meter exists to tell apart.
    constexpr double kFloorDb = -48.0;
    double lit = 0.0;
    if (level > 0.0) {
        const double db = 20.0 * std::log10(level);
        lit = qBound(0.0, (db - kFloorDb) / -kFloorDb, 1.0);
    }

    constexpr int kSegments = 12;
    const int inner = vertical ? rect.height() - 4 : rect.width() - 4;
    const int step = inner / kSegments;
    if (step <= 0) {
        return;
    }
    const int litCount = static_cast<int>(lit * kSegments + 0.5);
    for (int i = 0; i < kSegments; ++i) {
        // Green to amber to red, so the top of the scale reads as the top of
        // the scale without needing a legend.
        QColor on(0x55, 0xE0, 0x40);
        if (i >= kSegments - 2) {
            on = QColor(0xFF, 0x40, 0x30);
        } else if (i >= kSegments - 4) {
            on = QColor(0xFF, 0xC0, 0x20);
        }
        const QColor colour = i < litCount ? on : QColor(on.red() / 7, on.green() / 7, on.blue() / 7);
        QRect seg;
        if (vertical) {
            seg = QRect(rect.left() + 2,
                    rect.bottom() - 2 - (i + 1) * step + 1,
                    rect.width() - 4,
                    step - 1);
        } else {
            seg = QRect(rect.left() + 2 + i * step, rect.top() + 2, step - 1, rect.height() - 4);
        }
        pPainter->fillRect(seg, colour);
    }
}

QRect WDeckRack::knobGeometry(const QRect& moduleRect, int count, int index) {
    const QRect face = moduleRect.adjusted(3, 3, -3, -3);
    const int wellTop = face.top() + 5 + 30 + 6;
    const int wellBottom = face.bottom() - 28;

    // The headline knob keeps the centre line and its own row. It is the one
    // reached for mid-transition, so it stays big and stays where the thumb
    // expects it.
    constexpr int kHeadSize = 62;
    const int headTop = wellTop + 6;
    if (index == 0) {
        return QRect(moduleRect.center().x() - kHeadSize / 2, headTop, kHeadSize, kHeadSize);
    }

    // The rest go in two columns, the right one dropped half a row. Staggering
    // buys vertical room -- the captions of one column sit beside the knobs of
    // the other instead of under them -- and 204 px will not take two knobs
    // side by side at the same height with captions that read at arm's length.
    constexpr int kSize = 44;
    constexpr int kCaption = 15;
    const int params = count - 1;
    const int rows = (params + 1) / 2;
    const int top = headTop + kHeadSize + kCaption + 8;
    const int span = wellBottom - top - kSize - kCaption;
    // Solved so the lowest knob -- the last of the dropped column -- still
    // clears the well: top + (rows - 0.5) * step + size + caption <= bottom.
    const int step = rows > 0
            ? qBound(kSize + 10, static_cast<int>(span / qMax(0.5, rows - 0.5)), 78)
            : kSize;
    const int p = index - 1;
    const int column = p % 2;
    const int row = p / 2;
    const int x = column == 0 ? face.left() + face.width() / 4
                              : face.left() + face.width() * 3 / 4;
    const int y = top + row * step + (column == 1 ? step / 2 : 0);
    return QRect(x - kSize / 2, y, kSize, kSize);
}

void WDeckRack::paintModule(QPainter* pPainter, int index, const QPoint& offset) {
    const Module& module = m_modules.at(index);
    const auto& type = catalogue().at(module.type);
    const QRect rect = moduleRect(index).translated(
            offset + QPoint(qRound(m_slide.value(index)), 0));
    if (offset.isNull() && !rect.intersects(QRect(0, 0, width() - kModuleWidth, height()))) {
        return; // scrolled off; nothing to draw
    }

    pPainter->drawPixmap(rect.topLeft(), chromeFor(static_cast<Material>(type.material)));

    // The dry-killer lock is a property of position, so it is recomputed rather
    // than stored: the first module that generates new material.
    bool locked = false;
    for (int i = 0; i <= index; ++i) {
        if (catalogue().at(m_modules.at(i).type).generatesNewMaterial) {
            locked = (i == index);
            break;
        }
    }

    // The name, engraved into the title strip at the top.
    const QRect face = rect.adjusted(3, 3, -3, -3);
    const QRect strip(face.left() + 5, face.top() + 5, face.width() - 10, 30);
    QFont font = pPainter->font();
    font.setPixelSize(17);
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    pPainter->setFont(font);
    paintEngraved(pPainter,
            strip.adjusted(22, 0, -6, 0),
            Qt::AlignVCenter | Qt::AlignLeft,
            type.name,
            accentFor(type.material));
    font.setLetterSpacing(QFont::PercentageSpacing, 100.0);
    pPainter->setFont(font);

    // The module's meter lives in the title strip, right of its name. It costs
    // no vertical room -- there is none to spare -- and it is where the eye
    // already goes. What it answers is "is anything coming out of this?", which
    // is the question a rack of six healthy-looking modules cannot otherwise
    // be asked.
    if (module.slot >= 0) {
        auto* pLevel = slotControl(module.slot, QStringLiteral("output_level"));
        paintVu(pPainter,
                QRect(strip.right() - 74, strip.center().y() - 5, 70, 10),
                pLevel->valid() ? pLevel->get() : 0.0,
                false);
    }

    for (int k = 0; k < type.knobs.size(); ++k) {
        // The value, where the knob names a quantity, goes on the caption line
        // beside the name. Anywhere else costs vertical room the module does
        // not have, and a dial with a number beside it is how the reference
        // skins label theirs.
        QString caption = type.knobs.at(k).caption;
        const QString readout = knobReadout(index, k);
        if (!readout.isEmpty()) {
            caption += QStringLiteral("  ") + readout;
        }
        paintKnob(pPainter,
                knobGeometry(rect, type.knobs.size(), k),
                knobValue(index, k),
                caption,
                static_cast<Material>(type.material),
                k == 0 && locked);
    }

    if (m_heldModule == index) {
        pPainter->setBrush(Qt::NoBrush);
        pPainter->setPen(QPen(QColor(0x88, 0xFF, 0x00), 3, Qt::DashLine));
        pPainter->drawRect(rect.adjusted(2, 2, -2, -2));
    }
}

void WDeckRack::paintMaster(QPainter* pPainter) {
    const QRect rect = masterRect();
    pPainter->drawPixmap(rect.topLeft(), chromeFor(Material::BrushedSteel));

    const QRect face = rect.adjusted(3, 3, -3, -3);
    const QRect strip(face.left() + 5, face.top() + 5, face.width() - 10, 30);
    QFont font = pPainter->font();
    font.setPixelSize(17);
    font.setBold(true);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    pPainter->setFont(font);
    paintEngraved(pPainter,
            strip.adjusted(22, 0, -6, 0),
            Qt::AlignVCenter | Qt::AlignLeft,
            tr("MASTER"),
            accentFor(0));
    font.setLetterSpacing(QFont::PercentageSpacing, 100.0);

    // While muted the knob keeps showing the level it will come back to, drawn
    // grey. Snapping to zero would lose the one thing worth knowing -- how loud
    // unmuting is about to be.
    const bool muted = m_mutedLevel >= 0.0;
    ControlProxy* pMaster = masterControl();
    const double level = muted
            ? m_mutedLevel
            : (pMaster && pMaster->valid() ? pMaster->getParameter() : 0.5);
    const QRect knobRect(rect.center().x() - 52, strip.bottom() + 26, 104, 104);
    paintKnob(pPainter,
            knobRect,
            level,
            muted ? tr("MUTED") : (m_ringOut ? tr("SEND") : tr("RETURN")),
            Material::BrushedSteel,
            false,
            muted);

    // The number, because a knob pointer is not a reading. Big enough to catch
    // from across the booth, which is the whole job of a master. In dB, since
    // the knob has gain above unity now and "112" would not say which side of
    // it you were on.
    font.setPixelSize(30);
    font.setBold(true);
    pPainter->setFont(font);
    // Read off the dial position rather than the control, so a muted master
    // shows the level it will come back to instead of the silence it is at.
    // Both stages are ±12 dB audio tapers with unity at half travel, so the
    // position is the scale printed round the knob.
    const QString reading = level <= 0.0
            ? QStringLiteral("−∞")
            : QStringLiteral("%1 dB").arg(level * 24.0 - 12.0, 0, 'f', 1);
    const QRect readout(face.left(), knobRect.bottom() + 20, face.width(), 36);
    paintEngraved(pPainter,
            readout,
            Qt::AlignCenter,
            reading,
            muted ? QColor(0x8A, 0x90, 0x9B) : accentFor(0));

    // The master's meter: what the rack is returning, after the mix and the
    // makeup gain. Wider and taller than a module's because it is the one that
    // answers "is the rack making any sound at all".
    paintVu(pPainter,
            QRect(face.left() + 14, masterRockerRect().top() - 30, face.width() - 28, 16),
            m_pOutputLevel && m_pOutputLevel->valid() ? m_pOutputLevel->get() : 0.0,
            false);

    // The rocker: which end of the chain the knob is on.
    const QRect rocker = masterRockerRect();
    pPainter->fillRect(rocker, QColor(0, 0, 0, 90));
    paintBevel(pPainter, rocker, false);
    const QRect ring(rocker.left() + 2, rocker.top() + 2, rocker.width() / 2 - 3, rocker.height() - 4);
    const QRect cut(rocker.center().x() + 1, rocker.top() + 2, rocker.width() / 2 - 3, rocker.height() - 4);
    font.setPixelSize(13);
    font.setBold(true);
    pPainter->setFont(font);
    for (int i = 0; i < 2; ++i) {
        const QRect half = i == 0 ? ring : cut;
        const bool on = (i == 0) == m_ringOut;
        pPainter->fillRect(half, on ? QColor(0x3A, 0x4A, 0x22) : QColor(0x1A, 0x1C, 0x1F));
        paintBevel(pPainter, half, on);
        pPainter->setPen(on ? QColor(0xAE, 0xE8, 0x4A) : QColor(0x77, 0x7D, 0x86));
        pPainter->drawText(half, Qt::AlignCenter, i == 0 ? tr("RING OUT") : tr("CUT"));
    }

    font.setPixelSize(12);
    font.setBold(false);
    pPainter->setFont(font);
    pPainter->setPen(QColor(0x5A, 0x5F, 0x68));
    pPainter->drawText(QRect(face.left(), face.bottom() - 34, face.width(), 18),
            Qt::AlignCenter,
            muted ? tr("press to unmute") : tr("press to mute"));
}

QRect WDeckRack::masterRockerRect() const {
    const QRect rect = masterRect();
    const QRect face = rect.adjusted(3, 3, -3, -3);
    // Between the readout and the mute hint, both of which are placed off the
    // module rather than off constants -- the rack's height depends on the
    // browser's breadcrumb.
    return QRect(face.left() + 14, face.bottom() - 78, face.width() - 28, 30);
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
    pPainter->drawText(rect.adjusted(20, 0, -20, 0),
            Qt::AlignVCenter | Qt::AlignLeft,
            generatedName() + QStringLiteral("   ▾"));

    font.setPixelSize(15);
    font.setBold(false);
    pPainter->setFont(font);
    pPainter->setPen(kLcdEdge.lighter(160));
    pPainter->drawText(rect.adjusted(20, 0, -20, 0),
            Qt::AlignVCenter | Qt::AlignRight,
            QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));

    // Which deck the beat-aware effects are following, and at what tempo. A
    // delay quantised to the wrong deck sounds exactly like a broken one, so
    // this is not a nicety: it is the difference between "the sync is on the
    // other CDJ" and "the sync is broken".
    const double bpm = m_pFxBpm->valid() ? m_pFxBpm->get() : 0.0;
    const int source = m_pFxBpmSource->valid() ? static_cast<int>(m_pFxBpmSource->get()) : 0;
    QString tempo;
    if (source == 5) {
        tempo = tr("♪ THIS DECK  %1").arg(bpm, 0, 'f', 1);
    } else if (source >= 1 && source <= 4) {
        tempo = tr("♪ CDJ %1  %2").arg(source).arg(bpm, 0, 'f', 1);
    } else {
        tempo = tr("♪ no deck playing");
    }
    font.setBold(source != 0);
    pPainter->setFont(font);
    pPainter->setPen(source == 0 ? kLcdEdge.lighter(140) : kLcdInk);
    pPainter->drawText(rect.adjusted(0, 0, -140, 0), Qt::AlignVCenter | Qt::AlignRight, tempo);
}

namespace {
constexpr int kBrowserRowHeight = 58;
constexpr int kBrowserRowGap = 6;
constexpr int kBrowserTop = 24;
} // namespace

QRect WDeckRack::rackBrowserRowRect(int index) const {
    const int top = kNameBarHeight + kBrowserTop +
            index * (kBrowserRowHeight + kBrowserRowGap) - m_browserScroll;
    return QRect(60, top, width() - 120, kBrowserRowHeight);
}

int WDeckRack::rackBrowserScrollSpan() const {
    const int rows = savedRacks().size() + 1;
    const int content = kBrowserTop + rows * (kBrowserRowHeight + kBrowserRowGap);
    const int viewport = height() - kNameBarHeight - kBezelHeight;
    return qMax(0, content - viewport);
}

int WDeckRack::rackBrowserRowAt(const QPoint& point) const {
    const int rows = savedRacks().size() + 1;
    for (int i = 0; i < rows; ++i) {
        if (rackBrowserRowRect(i).contains(point)) {
            return i;
        }
    }
    return -1;
}

void WDeckRack::paintRackBrowser(QPainter* pPainter) {
    pPainter->fillRect(rect(), QColor(0, 0, 0, 216));

    QFont font = pPainter->font();
    font.setPixelSize(16);
    font.setBold(true);
    pPainter->setFont(font);
    pPainter->setPen(kLcdInk);
    pPainter->drawText(QRect(60, kNameBarHeight - 6, width() - 120, 24),
            Qt::AlignVCenter | Qt::AlignLeft,
            tr("SAVED RACKS   ·   drag to scroll   ·   hold a row to delete"));

    const QStringList names = savedRacks();
    // Clipped to the area below the header, so a scrolled row leaves the list
    // rather than sliding up over the title.
    pPainter->setClipRect(
            QRect(0, kNameBarHeight + 18, width(), height() - kNameBarHeight - kBezelHeight - 18));
    // Row 0 is always "save this one", so an empty list is still a usable
    // screen rather than a dead end.
    for (int i = 0; i <= names.size(); ++i) {
        const QRect row = rackBrowserRowRect(i);
        if (row.bottom() < kNameBarHeight || row.top() > height()) {
            continue; // scrolled out of sight
        }
        const bool isSave = i == 0;
        const bool armed = i == m_browserArmedRow;
        pPainter->fillRect(row,
                armed ? QColor(0x4A, 0x10, 0x10)
                      : (isSave ? QColor(0x18, 0x2C, 0x10) : QColor(0x16, 0x18, 0x1C)));
        if (armed) {
            pPainter->setBrush(Qt::NoBrush);
            pPainter->setPen(QPen(QColor(0xFF, 0x55, 0x55), 2));
            pPainter->drawRect(row.adjusted(1, 1, -1, -1));
        } else {
            paintBevel(pPainter, row, true);
        }
        font.setPixelSize(20);
        pPainter->setFont(font);
        pPainter->setPen(armed ? QColor(0xFF, 0xAA, 0xAA)
                               : (isSave ? QColor(0x88, 0xFF, 0x00) : QColor(0xDD, 0xDD, 0xDD)));
        pPainter->drawText(row.adjusted(20, 0, -20, 0),
                Qt::AlignVCenter | Qt::AlignLeft,
                isSave ? tr("+  Save this rack") : names.at(i - 1));
        if (armed) {
            font.setPixelSize(14);
            pPainter->setFont(font);
            pPainter->drawText(row.adjusted(20, 0, -20, 0),
                    Qt::AlignVCenter | Qt::AlignRight,
                    tr("release to delete  ·  slide off to keep"));
        }
    }
    pPainter->setClipping(false);
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
        if (i != m_heldModule) {
            paintModule(&painter, i);
        }
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

    if (m_heldModule >= 0) {
        // Last, and under the finger. Dragging something you cannot see is the
        // difference between a reorder that works and one that is believed.
        // Held at the point it was grabbed by, not by its centre. Snapping the
        // centre to the finger made the module jump the moment it was picked
        // up, which reads as having grabbed the wrong one.
        const QRect home = moduleRect(m_heldModule);
        const QPoint offset = m_heldPos - (home.topLeft() + m_heldGrab);
        painter.setOpacity(0.92);
        paintModule(&painter, m_heldModule, offset);
        painter.setOpacity(1.0);
    }

    if (m_chooserOpen) {
        paintChooser(&painter);
    }
    if (m_browserOpen) {
        paintRackBrowser(&painter);
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
    for (int k = 0; k < type.knobs.size(); ++k) {
        // Generous: a finger is wider than a knob, and the caption below it is
        // dead space anyway.
        const QRect hit = knobGeometry(rect, type.knobs.size(), k).adjusted(-10, -8, 10, 22);
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

    if (m_browserOpen) {
        // Nothing happens on the press: the list scrolls, and a hold means
        // delete, so what the gesture was is only known at the release.
        m_dragStart = point;
        m_heldPos = point;
        m_browserScrollStart = m_browserScroll;
        m_browserArmedRow = -1;
        m_browserTap = true;
        m_scrolling = true;
        m_longPress.start();
        return;
    }

    // The name bar is the handle for the saved racks: it already names the one
    // on screen, so it is where a DJ looks for the others.
    if (point.y() < kNameBarHeight) {
        m_browserOpen = true;
        m_browserScroll = 0;
        m_browserArmedRow = -1;
        m_browserTap = false;
        update();
        return;
    }

    if (m_chooserOpen) {
        const int columns = catalogue().size();
        const int cellWidth = 150;
        const int totalWidth = columns * (cellWidth + 12);
        int x = (width() - totalWidth) / 2;
        const int y = (height() - 260) / 2;
        for (int i = 0; i < columns; ++i) {
            if (QRect(x, y, cellWidth, 260).contains(point)) {
                // Slot -1: not in the chain yet. writeChainToEngine() gives it
                // one, and knows from the -1 that it has no values to carry.
                m_modules.append({i, -1});
                writeChainToEngine();
                break;
            }
            x += cellWidth + 12;
        }
        m_chooserOpen = false;
        update();
        return;
    }

    if (masterRockerRect().contains(point)) {
        // Which half was tapped, so tapping the side already lit does nothing
        // rather than toggling away from it.
        setRingOut(point.x() < masterRockerRect().center().x());
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
            auto* pControl = slotControl(m_modules.at(moduleIndex).slot, spec.item);
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
        // A snapped knob counts from the stop it was on, so a drag out and
        // back returns to exactly where it started.
        const auto& spec = catalogue().at(m_modules.at(moduleIndex).type).knobs.at(knobIndex);
        m_dragStartIndex = spec.readout == Readout::Beats
                ? beatDivisionIndex(slotControl(m_modules.at(moduleIndex).slot, spec.item)->get())
                : 0;
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

    if (m_browserOpen) {
        if (m_browserArmedRow >= 0) {
            // Armed: sliding off the row is the way out, so track whether the
            // finger is still on it and let the release decide.
            update();
            return;
        }
        if ((point - m_dragStart).manhattanLength() > 12) {
            m_longPress.stop();
            m_browserTap = false;
        }
        if (m_scrolling) {
            m_browserScroll = qBound(0,
                    m_browserScrollStart - (point.y() - m_dragStart.y()),
                    rackBrowserScrollSpan());
            update();
        }
        return;
    }

    if (m_dragKnobModule >= 0) {
        const Module& module = m_modules.at(m_dragKnobModule);
        const auto& spec = catalogue().at(module.type).knobs.at(m_dragKnobIndex);
        if (spec.readout == Readout::Beats) {
            // Clicks, not a sweep. 26 px a stop puts the whole list inside a
            // comfortable thumb travel and makes each one land deliberately.
            const int steps = qRound((m_dragStart.y() - point.y()) / 26.0);
            const int index = qBound(0,
                    m_dragStartIndex + steps,
                    static_cast<int>(kBeatDivisions.size()) - 1);
            slotControl(module.slot, spec.item)->set(kBeatDivisions.at(index));
            persistSoon();
            update();
            return;
        }
        // Vertical only, so a sloppy diagonal still turns cleanly. Full travel
        // over ~200 px: fine control in one movement.
        const double delta = (m_dragStart.y() - point.y()) / 200.0;
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
            persistSoon();
            update();
        }
        return;
    }

    if (m_heldModule >= 0) {
        m_heldPos = point;
        // Displace live: the rack always shows the order that would result, so
        // there is no moment where the outcome has to be imagined. The swap
        // happens when the held module's centre crosses a neighbour's, and the
        // neighbour slides into the place just vacated.
        const int centre = point.x() - m_heldGrab.x() + kModuleWidth / 2;
        int target = m_heldModule;
        while (target > 0 && centre < moduleRect(target - 1).center().x()) {
            --target;
        }
        while (target < m_modules.size() - 1 && centre > moduleRect(target + 1).center().x()) {
            ++target;
        }
        if (target != m_heldModule) {
            const Module held = m_modules.at(m_heldModule);
            m_modules.removeAt(m_heldModule);
            m_modules.insert(target, held);
            // Everything between the two positions shifted by exactly one
            // place, in the opposite direction to the held module. A fast drag
            // can cross several at once, so this is a range rather than a
            // single neighbour.
            const int step = m_heldModule < target ? -1 : 1;
            for (int i = qMin(m_heldModule, target); i <= qMax(m_heldModule, target); ++i) {
                if (i != target) {
                    startSlide(i, moduleRect(i - step).x());
                }
            }
            m_heldModule = target;
        }
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

    if (m_browserOpen) {
        const QPoint point = pEvent->pos();
        const int row = rackBrowserRowAt(point);
        if (m_browserArmedRow >= 0) {
            // Committed only if the finger is still on the row it armed.
            if (row == m_browserArmedRow) {
                const QStringList names = savedRacks();
                if (m_browserArmedRow - 1 < names.size()) {
                    deleteRack(names.at(m_browserArmedRow - 1));
                }
            }
            m_browserArmedRow = -1;
        } else if (m_browserTap && row >= 0) {
            if (row == 0) {
                saveCurrentRack();
            } else {
                const QStringList names = savedRacks();
                if (row - 1 < names.size()) {
                    loadRack(names.at(row - 1));
                }
            }
            m_browserOpen = false;
        }
        // A scroll leaves the list open, which is the whole point of it.
        m_browserTap = false;
        m_scrolling = false;
        update();
        return;
    }

    if (m_heldModule >= 0) {
        const QPoint point = pEvent->pos();
        if (binRect().contains(point)) {
            m_modules.removeAt(m_heldModule);
            m_slide.fill(0.0, m_modules.size());
        } else {
            // Nothing to decide here any more: the rack has been showing the
            // resulting order the whole way across, so the drop is only
            // letting go. The module flies from where the finger left it into
            // the slot it is already in.
            startSlide(m_heldModule, point.x() - m_heldGrab.x());
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
    ControlProxy* pMaster = masterControl();
    if (!pMaster || !pMaster->valid()) {
        return true;
    }
    // The encoder is the master, and only the master. Unmuting by turning would
    // be a surprise, so a turn while muted sets the level it will come back to.
    const double base = m_mutedLevel >= 0.0 ? m_mutedLevel : pMaster->getParameter();
    // 0.028 per detent: 40% faster than it was, which is the difference
    // between riding the master and winding it.
    const double next = qBound(0.0, base + steps * 0.028, 1.0);
    if (m_mutedLevel >= 0.0) {
        m_mutedLevel = next;
    } else {
        pMaster->setParameter(next);
    }
    update();
    return true;
}

bool WDeckRack::handleSelect() {
    ControlProxy* pMaster = masterControl();
    if (!pMaster || !pMaster->valid()) {
        return true;
    }
    // Which of the two this mutes is the rocker's whole point: in ring-out it
    // is the send, so the tail decays; in cut it is the return, so it stops.
    if (m_mutedLevel >= 0.0) {
        pMaster->setParameter(m_mutedLevel);
        m_mutedLevel = -1.0;
    } else {
        m_mutedLevel = pMaster->getParameter();
        pMaster->setParameter(0.0);
    }
    update();
    return true;
}

bool WDeckRack::handleBack() {
    // BACK leaves the innermost mode first: the chooser, then a held module,
    // and only then the page.
    if (m_browserOpen) {
        m_browserOpen = false;
        m_browserArmedRow = -1;
        update();
        return true;
    }
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
