#pragma once

#include <QColor>
#include <QFont>
#include <memory>

#include "widget/wwidget.h"

class ControlProxy;
class QDomNode;
class SkinContext;

/// The tempo read-out: playing BPM, how far the pitch fader is from the track's
/// own tempo, how wide that fader's range is — and, while the fader is not the
/// thing deciding the tempo, what the fader is *asking for*.
///
/// **That last one is what makes the fader usable again after following a
/// master.** Soft-takeover leaves the fader connected to nothing until it comes
/// back to meet the playing tempo, and until this the screen said nothing about
/// where it was: the DJ moved a dead fader toward a number they could not see,
/// and found the catch by accident. The dim second number is the sight, and it
/// goes away at the moment the fader takes over, because from then on the one
/// number is the answer.
///
/// One widget that paints all three rather than three widgets in a layout,
/// because this is laid *over* the waveform. Mixxx's stacked layout applies
/// `Qt::AlignCenter` to every layout it builds, including nested ones, so a
/// group of labels inside it sizes to its hint instead of filling and collapses
/// to a few pixels at the top. Painting it directly sidesteps the layout
/// engine entirely, and the background stays transparent so the waveform reads
/// through everywhere a digit is not.
class WTempoPanel : public WWidget {
    Q_OBJECT

  public:
    WTempoPanel(QWidget* pParent, const QString& group);

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    /// Raise above the waveform.
    ///
    /// A stacked layout raises only its *current* child, and the current one
    /// here is the zero-sized dummy Mixxx inserts first; everything else keeps
    /// whatever stacking order it was given, and the waveform was winning. Done
    /// on show rather than at construction because the layout places children
    /// after they are built.
    void showEvent(QShowEvent* pEvent) override;

  private:
    /// Colour for a pitch range, by how wide it is.
    QColor rangeColour(double range) const;

    std::unique_ptr<ControlProxy> m_pBpm;
    std::unique_ptr<ControlProxy> m_pRateRatio;
    std::unique_ptr<ControlProxy> m_pRateRange;
    std::unique_ptr<ControlProxy> m_pFileBpm;
    /// Where the tempo fader physically is, in the same -1..1 as `rate`,
    /// published by the mapping. See the skin's `[TriMixxx],tempo_fader`.
    std::unique_ptr<ControlProxy> m_pTempoFader;
    std::unique_ptr<ControlProxy> m_pRateDir;
    /// Whether this deck holds Pro DJ Link tempo master, and so whether the
    /// fader is a control rather than a disconnected stick.
    std::unique_ptr<ControlProxy> m_pIsMaster;

    /// The seven-segment face, for the tempo only. The smaller text stays in the
    /// UI font: Digital-7 has digits and almost nothing else, so "WIDE" would
    /// come out as blanks.
    QString m_tempoFamily = QStringLiteral("Digital-7 Mono");
    int m_tempoSize = 50;
    QColor m_tempoColour{0xff, 0x66, 0x00};
    /// At or above this the range is called WIDE: past it the number has
    /// stopped being something anyone acts on.
    double m_wideAbove = 0.9;
};
