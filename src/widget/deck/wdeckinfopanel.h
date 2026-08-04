#pragma once

#include <QList>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "library/deck/previewwaveform.h"

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
    /// filled in. Nothing if the track's own cover is already up, or if there
    /// is no path.
    void reloadCover();

    /// The waveform strip under the cover.
    ///
    /// A null preview is drawn as a baseline rather than as nothing, and **the
    /// strip is reserved whether or not there is one**: the panel follows the
    /// selection with no click, so a strip that collapsed would move every
    /// field 56 px up and down as the encoder turned. Unreadable, and a far
    /// worse cost than 44 px of background.
    ///
    /// Rendered here, once, rather than in paintEvent — the panel repaints on
    /// every detent, and 400 lines drawn per frame for a picture that has not
    /// changed is the same mistake the cover already taught this class.
    void setPreview(const PreviewWaveform& preview);
    void clear();

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    QString m_coverPath;
    QPixmap m_cover;
    /// `m_cover` is the stand-in rather than this track's own artwork, so a
    /// file arriving later is still worth loading.
    bool m_coverIsPlaceholder = false;
    QList<QPair<QString, QString>> m_fields;
    /// The strip, already drawn. Null when this track has no preview, or when
    /// one has been asked for and not arrived.
    QPixmap m_preview;
};

} // namespace deck
} // namespace mixxx
