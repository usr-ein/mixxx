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
/// alignment are the master's own. Our row comes from the engine's
/// `beat_distance`, which is exact within the beat but says nothing about which
/// beat of the bar we are on — so the bar position is counted locally from
/// `beat_active` and is arbitrary until someone lines it up. Beat alignment is
/// therefore trustworthy; bar alignment is a display convenience.
class WProLinkPhaseMeter : public WWidget {
    Q_OBJECT

  public:
    WProLinkPhaseMeter(QWidget* pParent, const QString& group);
    ~WProLinkPhaseMeter() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private slots:
    void onBeatActive(double value);

  private:
    /// Draw one row: four beat cells with a marker at *phase*.
    void paintRow(QPainter* pPainter,
            const QRectF& rect,
            double phase,
            const QColor& colour,
            const QString& label);

    const QString m_group;

    std::unique_ptr<ControlProxy> m_pMasterDevice;
    std::unique_ptr<ControlProxy> m_pMasterBarPhase;
    std::unique_ptr<ControlProxy> m_pMasterBpm;
    std::unique_ptr<ControlProxy> m_pBeatDistance;
    std::unique_ptr<ControlProxy> m_pBeatActive;
    std::unique_ptr<ControlProxy> m_pPlay;
    std::unique_ptr<ControlProxy> m_pBpm;

    /// Which beat of the bar we believe we are on, 0-3. Counted from
    /// `beat_active` because nothing in the engine tracks a bar.
    int m_ourBeatInBar = 0;

    QColor m_masterColour{0xff, 0x66, 0x00};
    QColor m_ourColour{0x44, 0xcc, 0xff};
    QColor m_inSyncColour{0x44, 0xff, 0x88};
};
