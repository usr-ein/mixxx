#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QTextBrowser>
#include <QTimer>
#include <memory>

#include "widget/deck/deckpage.h"
#include "widget/deck/deckrelease.h"

class ControlProxy;

namespace mixxx {
namespace deck {

class DeckLevels;

/// Everything needed to work out why the deck is misbehaving, on one page.
///
/// Reached from the root menu, scrolled by the encoder or a finger. Read-only
/// but for its first section, Adjust: the deck's output level and the panel's
/// brightness (decklevels.h), the two things a venue asks to change. Even those
/// change only once the encoder has been pressed to start adjusting, so the
/// page can still be opened mid-set without thinking about it -- turning the
/// encoder on arrival scrolls, as it always has.
///
/// The one other thing it does is on a release card: Slots says what each of
/// the card's A/B slots holds (deckrelease.h), and RESTART INTO, a tap only,
/// raises the skin's confirm for starting the other slot as a trial. The
/// restart itself is trimixxx-launchd's, as root: Mixxx only pulses a control.
///
/// **The sparklines are text.** Qt's rich text has no canvas and no JavaScript,
/// so a real chart would have to be painted to an image and inserted as a
/// document resource. Block characters cost none of that machinery and read
/// perfectly well at arm's length — and they keep the whole page editable as a
/// string, which is what makes it rearrangeable later.
class WDeckDiagnostics : public QTextBrowser, public DeckPage {
    Q_OBJECT

  public:
    /// *settingsPath* is the directory holding mixxx.cfg, where the levels set
    /// here are kept.
    explicit WDeckDiagnostics(const QString& settingsPath, QWidget* pParent = nullptr);
    ~WDeckDiagnostics() override;

    /// Start and stop sampling with visibility. A page nobody is looking at has
    /// no business reading /proc once a second.
    void setActive(bool active);

    /// Move the page by encoder detents. The encoder is the deck's own control
    /// and has to reach this page as well as the lists (browser-prd.md 14).
    void scrollBy(int steps);

    // DeckPage. The encoder scrolls the page; a press starts adjusting the
    // output, a second moves on to the brightness, a third stops. While a level
    // is being adjusted a turn moves it instead, and BACK stops adjusting
    // before it leaves the page -- otherwise the only way out of a value would
    // be the encoder, and BACK would take the page with it.
    bool handleMove(int steps) override;
    bool handleSelect() override;
    bool handleBack() override;

    /// How long the deck has been up, to the minute past the first: "40 s",
    /// "12 min", "2 h 05 min", "3 d 4 h".
    static QString uptimeText(qint64 seconds);

  protected:
    /// Adjusting stops when the page goes, BACK held to the deck included:
    /// coming back to an encoder still wired to a level is a trap.
    void hideEvent(QHideEvent* pEvent) override;

  private slots:
    void sample();
    /// RESTART INTO: the skin's confirm, if the slot can still be switched to.
    void onAnchorClicked(const QUrl& url);

  private:
    /// What a turn of the encoder moves. Nothing is the page's own default: it
    /// scrolls.
    enum class Adjusting {
        Nothing,
        Output,
        Brightness,
    };

    void setAdjusting(Adjusting adjusting);
    /// Re-render, keeping the scroll position.
    void render();
    QString html() const;
    /// The Adjust section: the two levels, and whether the output is clipping.
    QString adjustHtml() const;
    /// Identity's rows for the release running, or for a dev card's none.
    QString releaseRows() const;
    /// The Slots section and its RESTART INTO, on a release card only.
    QString slotsHtml() const;
    /// CPU busy fraction since the last sample, from /proc/stat.
    double sampleCpu();
    static QString readFile(const QString& path);
    static QString runCommand(const QString& program, const QStringList& args);
    /// A history rendered with block characters, oldest to newest.
    static QString sparkline(const QList<double>& history, double max);

    /// This deck's accent (deckaccent.h), as the CSS colour the page's
    /// headings and sparklines are drawn in.
    const QString m_accent;
    QTimer m_timer;
    QList<double> m_cpuHistory;
    QList<double> m_memHistory;
    QList<double> m_tempHistory;
    quint64 m_lastCpuIdle = 0;
    quint64 m_lastCpuTotal = 0;
    /// `vcgencmd get_throttled`, as of the last sample.
    QString m_throttled;
    /// The release and the slots, as of the last sample.
    DeckRelease m_release;

    std::unique_ptr<DeckLevels> m_pLevels;
    Adjusting m_adjusting = Adjusting::Nothing;
    /// The main output's clip light. Watched rather than sampled: it stays lit
    /// for a fraction of a second, and a page refreshed once a second would
    /// miss most clips.
    std::unique_ptr<ControlProxy> m_pClipping;
    /// Since the output last clipped, or invalid if it has not.
    QElapsedTimer m_sinceClipping;
};

} // namespace deck
} // namespace mixxx
