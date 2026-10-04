#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <memory>

#include "widget/wwidget.h"

class ControlProxy;
namespace mixxx {
namespace prolink {
class AudibleBeatClock;
}
} // namespace mixxx
class QDomNode;
class SkinContext;

/// Two bars showing where the Pro DJ Link deck we are mixing against is in its
/// bar, and where this deck is in its own.
///
///     |---|---|---|---|      them
///     --|---|---|---|--      us
///
/// The point is the offset between them. A DJ beatmatching by ear is judging
/// exactly this, and a CDJ shows it on its own display; without it, Mixxx is the
/// only device on the network flying blind.
///
/// **What each row is built from is not the same, and that matters.** The top
/// row comes off the network: beat packets on UDP 50001 arrive *on* each beat
/// and carry the beat's position in the bar, so both the phase and the bar
/// alignment are the other deck's own. Our row is derived from the playhead —
/// `beat_distance` for the sub-beat and the elapsed track time for which beat of
/// the bar. Deriving it from *position* rather than counting `beat_active`
/// pulses is what makes a loop behave: a one-beat loop replays the same beat,
/// and a counter would march on through the bar as though the track had.
/// Absolute bar alignment is still arbitrary — nothing in the engine names a
/// downbeat — so beat alignment is trustworthy and bar alignment is a display
/// convenience.
///
/// # States
///
/// Which deck the top row follows is decided by ProLinkNetworkService, not
/// here: it publishes `[ProLink] master_bar_phase`, `-1` when there is nobody to
/// follow. So the meter has two states and no others:
///
///  * **Nobody to follow** (`master_bar_phase < 0`): both rows idle, `-` in
///    place of a player number. Our own ticks are not drawn on their own: a
///    comparison with one side missing reads as a meter that is working, and
///    one walking across an otherwise empty meter looked like it was following
///    something that was not there.
///  * **Following player N**: both rows drawn, N over the top row. Our row is
///    blank if this deck has no grid to place it on. A deck that is not
///    playing is held where its status says it stands, to the nearest beat,
///    and moves when its jog wheel moves it -- a CDJ sends no beats while
///    paused, and its status says no finer than the beat.
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

    const QString m_group;

    /// `[ProLink]`. Created by ProLinkControls, from CoreServices, before any
    /// skin is parsed -- so unlike when ProLinkNetworkService made them, they
    /// exist by the time this widget does and need no retrying.
    std::unique_ptr<ControlProxy> m_pMasterDevice;
    std::unique_ptr<ControlProxy> m_pMasterBarPhase;
    /// This deck.
    std::unique_ptr<ControlProxy> m_pBeatDistance;
    std::unique_ptr<ControlProxy> m_pBpm;
    std::unique_ptr<ControlProxy> m_pFileBpm;
    std::unique_ptr<ControlProxy> m_pDuration;
    std::unique_ptr<ControlProxy> m_pPlayPosition;
    /// Our place on the grid as heard; see mixxx::prolink::AudibleBeatClock.
    std::unique_ptr<mixxx::prolink::AudibleBeatClock> m_pOurBeat;

    /// Fixed per row, deliberately. An earlier version turned both green when
    /// the decks agreed; it read as the meter changing meaning rather than the
    /// decks changing relationship, and the rows lining up already says it.
    QColor m_masterColour{0xff, 0x66, 0x00};
    QColor m_ourColour{0x44, 0xcc, 0xff};

    /// Whether the last paint had nobody to follow. While it stays that way
    /// nothing on the meter moves, and the repaint timer skips the frame.
    bool m_idle = false;
};
