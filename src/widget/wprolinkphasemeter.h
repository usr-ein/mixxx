#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <memory>

#include "widget/wwidget.h"

class ControlProxy;
class QDomNode;
class SkinContext;

/// Two bars showing where the Pro DJ Link tempo master is in its bar, and where
/// this deck is in its own.
///
///     |---|---|---|---|      master
///     --|---|---|---|--      us
///
/// The point is the offset between them. A DJ beatmatching by ear is judging
/// exactly this, and a CDJ shows it on its own display; without it, Mixxx is the
/// only device on the network flying blind.
///
/// **What each row is built from is not the same, and that matters.** The master
/// row comes off the network: beat packets on UDP 50001 arrive *on* each beat
/// and carry the beat's position in the bar, so both the phase and the bar
/// alignment are the master's own. Our row is derived from the playhead —
/// `beat_distance` for the sub-beat and the elapsed track time for which beat of
/// the bar. Deriving it from *position* rather than counting `beat_active`
/// pulses is what makes a loop behave: a one-beat loop replays the same beat,
/// and a counter would march on through the bar as though the track had.
/// Absolute bar alignment is still arbitrary — nothing in the engine names a
/// downbeat — so beat alignment is trustworthy and bar alignment is a display
/// convenience.
class WProLinkPhaseMeter : public WWidget {
    Q_OBJECT

  public:
    WProLinkPhaseMeter(QWidget* pParent, const QString& group);
    ~WProLinkPhaseMeter() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    /// Draw one row: four beat cells with a marker at *phase*.
    void paintRow(QPainter* pPainter,
            const QRectF& rect,
            double phase,
            const QColor& colour,
            const QString& label);
    /// The player number, over the ticks rather than beside them.
    void drawOverlayLabel(QPainter* pPainter, const QRectF& rect, const QString& label);
    /// Attach to the `[ProLink]` controls, if they exist yet.
    ///
    /// **They may well not.** A ControlProxy resolves its control once, in its
    /// constructor, and a control created afterwards is never picked up — the
    /// proxy just reads 0.0 for the rest of the session. The three this widget
    /// wants are created by ProLinkNetworkService, which is created by the
    /// MediaRegistry, which is created by WDeckBrowser — another widget in this
    /// same skin, built *after* the header this meter sits in. So on a cold
    /// start all three resolved to null, the log said so three times, and the
    /// master row has been frozen at phase zero ever since.
    void attachToProLink();

    const QString m_group;

    std::unique_ptr<ControlProxy> m_pMasterDevice;
    std::unique_ptr<ControlProxy> m_pMasterBarPhase;
    std::unique_ptr<ControlProxy> m_pMasterBpm;
    std::unique_ptr<ControlProxy> m_pBeatDistance;
    std::unique_ptr<ControlProxy> m_pPlay;
    std::unique_ptr<ControlProxy> m_pBpm;
    std::unique_ptr<ControlProxy> m_pFileBpm;
    std::unique_ptr<ControlProxy> m_pDuration;
    std::unique_ptr<ControlProxy> m_pPlayPosition;

    /// Fixed per row, deliberately. An earlier version turned both green when
    /// the decks agreed; it read as the meter changing meaning rather than the
    /// decks changing relationship, and the rows lining up already says it.
    QColor m_masterColour{0xff, 0x66, 0x00};
    QColor m_ourColour{0x44, 0xcc, 0xff};

    /// Repaints since the last attempt to attach. Retrying on every frame would
    /// be three failed lookups a frame, forever, on a deck that has no network.
    int m_sinceAttach = 0;
};
