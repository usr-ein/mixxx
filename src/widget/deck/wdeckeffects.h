#pragma once

#include <QList>
#include <QString>
#include <QTextBrowser>
#include <QTimer>
#include <memory>

class ControlProxy;

namespace mixxx {
namespace deck {

/// The effect rack, on one page, reached from the root menu.
///
/// This is the debugging face of the effect-pedal bus: the Xone's aux send
/// arrives at `[Auxiliary1]`, `[EffectRack1_EffectUnit2]` processes it in WET
/// mix mode, and the result leaves on the deck's only output. Every stage of
/// that has a way of being silently wrong -- the aux input not configured, the
/// channel not on the main mix, the unit not routed to it, the chain in the
/// wrong mix mode, no effect loaded -- and each failure sounds identical from
/// the booth, which is to say silent. So the top half states all of them
/// outright rather than making them inferable.
///
/// Unlike the diagnostics page this one is **not** read-only: the deck has no
/// spare knob for a wet level, and editing a file and restarting to move one
/// value is no way to judge a reverb. The encoder does both jobs, one gesture
/// each -- turn to pick a row, press to start and stop adjusting it -- because
/// those are the only two the browser gives a page that is not a list.
///
/// Deliberately a scaffold. The real effects UI goes here later; this exists to
/// make the signal path legible while it is being built.
class WDeckEffects : public QTextBrowser {
    Q_OBJECT

  public:
    explicit WDeckEffects(QWidget* pParent = nullptr);
    ~WDeckEffects() override;

    /// Refresh only while the page is on screen. Values are read from controls
    /// rather than pushed by them, so an unwatched page is pure waste.
    void setActive(bool active);

    /// An encoder detent: move the cursor, or change the selected value when
    /// adjusting.
    void moveSelection(int steps);

    /// Encoder press: start adjusting the selected row, or stop.
    void toggleAdjust();

    /// True while a value is being adjusted, so the browser knows the press
    /// belongs to this page and BACK should leave adjust mode before it pops
    /// the level.
    bool isAdjusting() const {
        return m_adjusting;
    }

  private:
    /// One adjustable control. Values are held in the control, never here --
    /// this only knows how to reach one and what to call it.
    ///
    /// The proxy is a raw pointer parented to the widget rather than a
    /// unique_ptr: this lives in a QList, and QList wants an element type it
    /// can copy. Qt owns the proxies through the parent instead.
    struct Knob {
        QString label;
        QString group;
        QString item;
        /// Step per detent in parameter space (0..1), so one turn crosses a
        /// sensible fraction of any control's range whatever its units.
        double step = 0.02;
        ControlProxy* pControl = nullptr;
    };

    void addKnob(const QString& label,
            const QString& group,
            const QString& item,
            double step = 0.02);
    QString html() const;
    /// A control's value, or an em dash if nothing has created it -- which is
    /// itself the answer when a group is misspelled or a unit does not exist.
    static QString value(const ControlProxy* pControl, int decimals = 3);
    static QString onOff(const ControlProxy* pControl);
    /// A 0..1 fraction as a bar of block characters. Same reasoning as the
    /// diagnostics sparklines: Qt rich text has no canvas, and blocks read fine
    /// at arm's length.
    static QString bar(double fraction, int width = 16);

    QTimer m_timer;
    QList<Knob> m_knobs;
    int m_selected = 0;
    bool m_adjusting = false;

    /// Read-only status, in the order the signal travels.
    std::unique_ptr<ControlProxy> m_pAuxConfigured;
    std::unique_ptr<ControlProxy> m_pAuxMainMix;
    std::unique_ptr<ControlProxy> m_pAuxVu;
    std::unique_ptr<ControlProxy> m_pUnitRouted;
    std::unique_ptr<ControlProxy> m_pUnitEnabled;
    std::unique_ptr<ControlProxy> m_pMixMode;
    std::unique_ptr<ControlProxy> m_pEffectLoaded;
};

} // namespace deck
} // namespace mixxx
