#pragma once

#include <QDialog>
#include <QString>

class QLabel;

namespace mixxx {

/// A circular progress ring, shown while a track is pulled off a player.
///
/// Custom-painted rather than a QProgressBar because the deck is a dark,
/// touch-oriented kiosk skin where a stock horizontal bar looks out of place,
/// and because a ring reads at a glance from arm's length.
///
/// Modal over the application, and deliberately without a Cancel button:
/// cancelling would have to abort a transfer the caller is synchronously
/// waiting on, and the caller cannot be handed "no track" halfway through
/// without hitting the index-invalidation hazard the whole fetch design exists
/// to avoid. ProLinkTrackFetcher's timeout is the escape hatch.
class DlgProLinkFetch : public QDialog {
    Q_OBJECT

  public:
    explicit DlgProLinkFetch(const QString& trackName, QWidget* pParent = nullptr);

    /// 0..1. Values outside that range are clamped; a total of 0 shows an
    /// indeterminate ring rather than a full one.
    void setProgress(double fraction);
    void setIndeterminate();

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    /// Swallowed: there is nothing to cancel to.
    void keyPressEvent(QKeyEvent* pEvent) override;

  private:
    QString m_trackName;
    double m_fraction = 0.0;
    bool m_indeterminate = true;
    /// Rotation for the indeterminate sweep, in degrees.
    int m_spin = 0;
};

} // namespace mixxx
