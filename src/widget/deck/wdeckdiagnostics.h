#pragma once

#include <QHash>
#include <QList>
#include <QTextBrowser>
#include <QTimer>
#include <memory>

#include "widget/deck/deckpage.h"

class ControlProxy;

namespace mixxx {
namespace deck {

/// Everything needed to work out why the deck is misbehaving, on one page.
///
/// Reached from the root menu, scrolled by the encoder or a finger, and
/// read-only: nothing here changes deck state, so it can be opened mid-set
/// without thinking about it.
///
/// **The sparklines are text.** Qt's rich text has no canvas and no JavaScript,
/// so a real chart would have to be painted to an image and inserted as a
/// document resource. Block characters cost none of that machinery and read
/// perfectly well at arm's length — and they keep the whole page editable as a
/// string, which is what makes it rearrangeable later.
class WDeckDiagnostics : public QTextBrowser, public DeckPage {
    Q_OBJECT

  public:
    explicit WDeckDiagnostics(QWidget* pParent = nullptr);

    /// Start and stop sampling with visibility. A page nobody is looking at has
    /// no business reading /proc once a second.
    void setActive(bool active);

    /// Move the page by encoder detents. The encoder is the deck's own control
    /// and has to reach this page as well as the lists (browser-prd.md 14).
    void scrollBy(int steps);

    // DeckPage. One long page, so the encoder scrolls it; nothing to activate,
    // and the press must not fall through to the menu underneath.
    bool handleMove(int steps) override {
        scrollBy(steps);
        return true;
    }
    bool handleSelect() override {
        return true;
    }

  private slots:
    void sample();

  private:
    QString html() const;
    /// CPU busy fraction since the last sample, from /proc/stat.
    double sampleCpu();
    static QString readFile(const QString& path);
    static QString runCommand(const QString& program, const QStringList& args);
    /// A history rendered with block characters, oldest to newest.
    static QString sparkline(const QList<double>& history, double max);

    /// The effect-pedal bus, read straight off the controls. Every one of these
    /// can be silently wrong and they all sound identical -- which is to say
    /// silent -- so the page states each separately rather than leaving it to
    /// be inferred.
    std::unique_ptr<ControlProxy> m_pAuxConfigured;
    std::unique_ptr<ControlProxy> m_pAuxMainMix;
    std::unique_ptr<ControlProxy> m_pAuxVu;
    std::unique_ptr<ControlProxy> m_pUnitRouted;
    std::unique_ptr<ControlProxy> m_pUnitEnabled;
    std::unique_ptr<ControlProxy> m_pMixMode;
    std::unique_ptr<ControlProxy> m_pEffectLoaded;
    std::unique_ptr<ControlProxy> m_pEffectOn;

    QTimer m_timer;
    QList<double> m_cpuHistory;
    QList<double> m_memHistory;
    QList<double> m_tempHistory;
    quint64 m_lastCpuIdle = 0;
    quint64 m_lastCpuTotal = 0;
};

} // namespace deck
} // namespace mixxx
