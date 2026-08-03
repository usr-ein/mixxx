#pragma once

#include <QList>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QWidget>

namespace mixxx {
namespace deck {

/// Everything known about the selected track (browser-prd.md 8.2).
///
/// Sits to the right of the list in the info layout, and follows the selection
/// as the encoder turns — no click, no load, no waiting. It is the answer to
/// "what *is* this" without committing a deck to finding out.
///
/// Painted rather than laid out in labels, for the same reason the rows are:
/// the field list is dynamic — anything empty is dropped, and whatever the list
/// is sorted by is dropped too, because it is already beside every title on the
/// left — so a fixed set of QLabels would spend most of its life hidden.
class WDeckInfoPanel : public QWidget {
    Q_OBJECT

  public:
    explicit WDeckInfoPanel(QWidget* pParent = nullptr);

    /// Fields are drawn in the order given; the caller decides what to omit.
    void setTrack(const QString& coverPath, const QList<QPair<QString, QString>>& fields);
    /// Try the cover file again, for one that arrived after the panel was
    /// filled in. Nothing if it is already loaded, or if there is no path.
    void reloadCover();
    void clear();

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    QString m_coverPath;
    QPixmap m_cover;
    QList<QPair<QString, QString>> m_fields;
};

} // namespace deck
} // namespace mixxx
