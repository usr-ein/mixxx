#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <memory>

#include "preferences/usersettings.h"
#include "widget/deck/deckpage.h"

class ControlProxy;
class EffectsManager;

namespace mixxx {
namespace deck {

/// The effect rack: a row of effect modules, drawn as hardware, on the
/// touchscreen.
///
/// Drives the effect-pedal bus — `[EffectRack1_EffectUnit2]`, routed to
/// `[Auxiliary1]` and running in WET mix mode, so what leaves the deck is
/// effect and never the dry the mixer is already carrying.
///
/// Design and the reasoning behind it: `docs/effects-prd.md`.
///
/// **Signal order is left to right**, identical to the chain's slot order, so
/// the mapping between what is drawn and what the engine does is the identity.
///
/// This is painted rather than composed from child widgets. Every module is the
/// same handful of primitives — bevel, gradient, screws, knobs — and the chrome
/// is static, so it is rendered once into a pixmap per material and blitted;
/// only knobs repaint as they move. On a Pi 4 with no compositor help for
/// widgets, regenerating a brushed-metal gradient on every knob movement is
/// visible.
class WDeckRack : public QWidget, public DeckPage {
    Q_OBJECT

  public:
    /// *pEffectsManager* may be null in a build with no effects; the rack then
    /// still draws and still drives the chain through controls, and only the
    /// saving and loading of whole racks is unavailable.
    WDeckRack(EffectsManager* pEffectsManager,
            UserSettingsPointer pConfig,
            QWidget* pParent = nullptr);
    ~WDeckRack() override;

    /// Refresh only while on screen; values are read from controls rather than
    /// pushed by them.
    void setActive(bool active);

    // DeckPage. The encoder is the master module's, and nothing else:
    // scrolling and every module control is touch.
    bool handleMove(int steps) override;
    bool handleSelect() override;
    bool handleBack() override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;

  public:
    /// What a module is made of. Purely visual; two modules can share an effect.
    ///
    /// Public, with the four painters below, because the deck view's FX strip
    /// draws the same hardware and duplicating a bevel is how two panels start
    /// to look like two panels. If a third caller appears this wants lifting
    /// out into a `deckchrome` of its own; with two it is not yet worth the
    /// indirection.
    enum class Material {
        BrushedSteel,
        YellowPlastic,
        GreenPlastic,
        PianoBlack,
    };

    /// A module in the rack: which type, and which chain slot it occupies.
    struct Module {
        int type = -1;
        int slot = -1;
    };

    // ---- geometry ----------------------------------------------------------
    static constexpr int kNameBarHeight = 48;
    static constexpr int kBezelHeight = 56;
    static constexpr int kModuleWidth = 204;
    static constexpr int kGutter = 4;
    /// The INPUT module, pinned at the far left as the master is pinned right.
    /// Narrower than a module: it carries two dials and no name to speak of.
    static constexpr int kInputWidth = 156;
    /// Sentinels for what the encoder is pointed at, in place of a module
    /// index. Both are fixed modules that live outside m_modules.
    static constexpr int kFocusMaster = -1;
    static constexpr int kFocusInput = -2;

    int rackHeight() const;
    QRect moduleRect(int index) const; ///< in widget coordinates, scrolled
    QRect masterRect() const;
    /// The fixed INPUT module: the two send levels feeding the whole rack.
    QRect inputRect() const;
    QRect inputKnobRect(int knobIndex) const;
    /// Which INPUT dial is under a point, or -1.
    int inputKnobAt(const QPoint& point) const;
    ControlProxy* inputControl(int knobIndex) const;
    void paintInput(QPainter* pPainter);
    QRect plusRect() const;
    QRect binRect() const;
    int scrollSpan() const;

  public:
    // ---- painting ----------------------------------------------------------
    /// Chrome for one material, rendered once and reused.
    const QPixmap& chromeFor(Material material, int width);
    static void paintChrome(QPainter* pPainter, const QRect& rect, Material material);
    static void paintKnob(QPainter* pPainter,
            const QRect& rect,
            double parameter,
            const QString& caption,
            Material material,
            bool locked,
            /// Drawn in grey, still showing its value: the master while muted.
            bool greyed = false,
            /// The encoder is pointed at this one, and says so with a ring.
            /// A wheel that drives a different knob depending on history is
            /// worse than one that only ever drove the master.
            bool focused = false);
    /// A recessed slot with a segmented ladder in it, green through amber to
    /// red, the way the reference skins draw theirs. *level* is a linear
    /// amplitude, 0..1, as the engine publishes it; the scale is dB, because a
    /// linear meter spends nine tenths of its travel on the top 20 dB and
    /// shows nothing at all where a reverb tail lives.
    static void paintVu(QPainter* pPainter, const QRect& rect, double level, bool vertical);
    /// A bevel is two lines: light on top and left, dark on bottom and right.
    /// Swap them and the same shape reads as recessed, which is how knob
    /// troughs and screw holes are drawn.
    static void paintBevel(QPainter* pPainter, const QRect& rect, bool raised);
    /// The glyph twice, offset a pixel, so it reads as stamped into the panel.
    static void paintEngraved(QPainter* pPainter,
            const QRect& rect,
            int flags,
            const QString& text,
            const QColor& ink);
    /// Where knob *index* sits on a module of this size. Everything is derived
    /// from the module rect rather than from constants, because the rack's
    /// height depends on the browser's breadcrumb and is not knowable here.
    static QRect knobGeometry(const QRect& moduleRect, int count, int index);
    /// The ink a material's captions are printed in.
    static QColor materialInk(int material);

  private:
    void paintModule(QPainter* pPainter, int index, const QPoint& offset = QPoint());
    void paintMaster(QPainter* pPainter);
    void paintNameBar(QPainter* pPainter);
    void paintChooser(QPainter* pPainter);

    // ---- engine ------------------------------------------------------------
    ControlProxy* slotControl(int slot, const QString& item);
    double knobValue(int moduleIndex, int knobIndex) const;
    /// The value beside the caption, in the quantity the knob names -- a cutoff
    /// in Hz -- or empty for a knob that is only a proportion. Empty for all of
    /// them would be a rack you have to turn to find out what it is set to.
    QString knobReadout(int moduleIndex, int knobIndex) const;
    void setKnobValue(int moduleIndex, int knobIndex, double parameter);
    /// Read the chain back into m_modules. The engine is the truth; this
    /// widget holds no state the controls do not.
    void syncFromEngine();
    void writeChainToEngine();
    /// Apply the dry-killer lock (PRD §3.2): the first module that generates
    /// new material is pinned fully wet, and anything downstream is free.
    void applyWetLock();
    QString generatedName() const;

    // ---- saved racks -------------------------------------------------------
    /// The saved racks, newest first. Stock Mixxx chain presets, which live as
    /// XML under `~/.mixxx/effects/chains` -- the SD card, on this deck.
    QStringList savedRacks() const;
    void saveCurrentRack();
    void loadRack(const QString& name);
    void deleteRack(const QString& name);
    void paintRackBrowser(QPainter* pPainter);
    QRect rackBrowserRowRect(int index) const;
    /// Which row is under a point, or -1. Row 0 is "save this one".
    int rackBrowserRowAt(const QPoint& point) const;
    int rackBrowserScrollSpan() const;

    // ---- encoder focus (PRD §17.4) -----------------------------------------
    /// The knob the encoder is currently turning: a module index and a knob
    /// index, or {-1, -1} for the master.
    ///
    /// A filter sweep wants the detented wheel, not a fingertip dragged 200 px
    /// up a 204 px module -- so touching a knob claims the encoder. The press
    /// never moves: it is the master's mute whatever the rotation is pointed
    /// at, because that is the panic button.
    int m_focusModule = -1;
    int m_focusKnob = -1;
    /// Take focus if this knob can use it. A locked wet knob refuses, because
    /// claiming the encoder with a knob that cannot turn would silently
    /// disable the wheel.
    void focusKnob(int moduleIndex, int knobIndex);
    /// True while *knobIndex* on *moduleIndex* is the first dry-killer's wet
    /// knob, which §3.2 pins at 100%.
    bool knobIsLocked(int moduleIndex, int knobIndex) const;
    /// Move the focused knob by *steps* detents. Snapped knobs move one stop
    /// per detent; everything else moves by a fixed fraction of its travel.
    void turnFocusedKnob(int steps);

    // ---- hit testing -------------------------------------------------------
    /// Which module and knob is under a point, or -1.
    bool knobAt(const QPoint& point, int* pModule, int* pKnob) const;
    int moduleAt(const QPoint& point) const;

    QVector<Module> m_modules;
    QHash<QString, ControlProxy*> m_controls;
    QHash<int, QPixmap> m_chrome;

    QTimer m_refresh;
    QTimer m_longPress;
    /// Writes effects.xml a moment after the rack stops changing.
    ///
    /// Mixxx only persists the effect state from ~EffectsManager, at the end
    /// of a teardown -- and this deck's teardown crashes before it gets there,
    /// so for a day and a half the rack silently did not persist at all. Even
    /// with that fixed it would be the wrong place to rely on: a booth deck
    /// gets switched off at the wall, and no destructor runs then either.
    ///
    /// Debounced rather than written on every change, because dragging a knob
    /// is hundreds of changes and this serialises every chain in the session.
    QTimer m_persist;
    void persistSoon();
    /// False until the constructor has finished reading the rack back, so the
    /// initial write is not mistaken for a change worth saving.
    bool m_ready = false;
    /// Runs only while something is sliding into its new place, and stops
    /// itself when everything has arrived.
    QTimer m_slideTimer;
    /// Per module index, how far left of where it now belongs it is still
    /// being drawn. A displaced neighbour is given the distance it just jumped
    /// and this decays it to zero, so the rack shows the move rather than
    /// having already made it.
    QVector<double> m_slide;
    void startSlide(int index, int fromX);
    void stepSlide();

    int m_scroll = 0;

    // Drag state. Exactly one of these is live at a time.
    int m_dragKnobModule = -1;
    int m_dragKnobIndex = -1;
    double m_dragStartValue = 0.0;
    /// For a snapped knob: which stop it was on when the drag began.
    int m_dragStartIndex = 0;
    QPoint m_dragStart;
    bool m_scrolling = false;
    int m_scrollStart = 0;
    int m_heldModule = -1; ///< long-pressed, being reordered
    QPoint m_heldPos;
    /// Where in the module the finger landed, so the drag holds that point
    /// rather than snapping the module's centre to the finger. Picking
    /// something up should not move it.
    QPoint m_heldGrab;

    bool m_chooserOpen = false;
    /// The saved-rack list, opened by tapping the name bar.
    bool m_browserOpen = false;
    int m_browserScroll = 0;
    int m_browserScrollStart = 0;
    /// The saved rack a hold has armed for deletion, or -1.
    ///
    /// Deleting on the hold itself would be a destructive gesture with no way
    /// out of it. The row goes red instead and the *release over it* is what
    /// commits, so sliding off the row before letting go cancels.
    int m_browserArmedRow = -1;
    /// Still a tap: nothing has scrolled and no hold has fired, so the release
    /// means the row it is over.
    bool m_browserTap = false;
    EffectsManager* m_pEffectsManager = nullptr;
    /// Last tap, for the double-tap reset.
    QElapsedTimer m_tapTimer;
    QPoint m_lastTapPos;

    /// Which gain the master knob and its mute drive.
    ///
    /// **Ring out** scales what goes *into* the chain -- the aux send -- so
    /// pulling it stops feeding the tank and whatever is in it decays on its
    /// own. **Cut** scales what comes *out*, so the tail stops dead. That is
    /// the send/return distinction from `docs/xone92-send-return.md` arriving
    /// inside the deck; both are musical and they are different gestures.
    ///
    /// Default ring out, because it is the one that cannot be got any other
    /// way. Kept in mixxx.cfg: it is a preference about how the deck plays,
    /// not a property of a rack.
    bool m_ringOut = true;
    /// The gain in force, whichever of the two it is.
    ControlProxy* masterControl() const;
    /// The other one: at unity in normal use, so only one stage has gain in it.
    ControlProxy* idleControl() const;
    void setRingOut(bool ringOut);
    QRect masterRockerRect() const;

    UserSettingsPointer m_pConfig;
    /// The rocker, shared with the deck view's FX strip.
    std::unique_ptr<ControlProxy> m_pRingOutControl;
    /// What the whole chain is returning, for the master's meter.
    std::unique_ptr<ControlProxy> m_pOutputLevel;
    std::unique_ptr<ControlProxy> m_pMasterMix;
    /// The send: [Auxiliary1] pregain, pre-effect, ±12 dB.
    std::unique_ptr<ControlProxy> m_pAuxPregain;
    /// What reaches the chain from each source, before `pregain` scales their
    /// sum. The INPUT module's two dials.
    std::unique_ptr<ControlProxy> m_pAuxSendLevel;
    std::unique_ptr<ControlProxy> m_pDeckSendLevel;
    /// The return: the chain's own output gain, the same ±12 dB.
    std::unique_ptr<ControlProxy> m_pMakeup;
    /// The tempo the rack is running at and which deck it came from, shown in
    /// the name bar. A beat-synced delay that is following the wrong deck looks
    /// identical to one that is broken, so it says which.
    std::unique_ptr<ControlProxy> m_pFxBpm;
    std::unique_ptr<ControlProxy> m_pFxBpmSource;
    double m_mutedLevel = -1.0; ///< >= 0 while muted: what to restore
};

} // namespace deck
} // namespace mixxx
