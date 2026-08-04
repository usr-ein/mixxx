#include "widget/deck/wdeckinfopanel.h"

#include <QFileInfo>
#include <QPainter>

#include "library/coverartutils.h"

namespace {
const QColor kBackground(0x14, 0x14, 0x14);
const QColor kLabel(0x77, 0x77, 0x77);
const QColor kValue(0xee, 0xee, 0xee);
const QColor kCoverBackdrop(0x1a, 0x1a, 0x1a);
/// What the strip shows when there is no waveform: a hairline, so the space
/// reads as a waveform that is absent rather than as a panel that failed to
/// draw one.
const QColor kWaveBaseline(0x33, 0x33, 0x33);

/// The panel's height budget, which has to add up to 460 — 36 + 48 + **460** +
/// 56 = 600 down the screen:
///
///     16 padding + 160 cover + 12 + 44 strip + 12 + 8 rows x 27 = 460
///
/// The cover lost 20 px and the rows 3 each to pay for the strip, which is what
/// keeps the field list at the eight rows it could already draw. paintEvent
/// stops when it runs out of panel, so a ninth field was never shown anyway.
constexpr int kCoverSize = 160;
constexpr int kPadding = 16;
constexpr int kGap = 12;
constexpr int kRowHeight = 27;
/// Height of the waveform strip.
constexpr int kPreviewHeight = 44;
/// **Exactly one pixel per rekordbox column.** `PWAV` is 400 columns and the
/// panel is 464 wide, so the strip is drawn 400 px wide and centred, with 32 px
/// either side. No scaling, no interpolation, no resampling — which is most of
/// the reason this tag is the right one to draw.
constexpr int kPreviewWidth = mixxx::deck::PreviewWaveform::kColumns;
/// Width of the label column. Fixed, so the values line up down the panel
/// rather than stepping in and out with the length of each label.
constexpr int kLabelWidth = 110;

/// The bigger of the two images rekordbox wrote, where there is one.
///
/// A rekordbox export carries every cover twice: the path in the pdb points at
/// an **80x80** thumbnail, and beside it sits the same image at 240x240 with
/// `_m` before the extension. 80 pixels blown up to this panel's 180 is exactly
/// as bad as it sounds, and the better file was on the stick all along.
///
/// Falls back to the path it was given whenever the companion is absent, which
/// is the normal case for a remote medium: those covers come over dbserver, by
/// artwork id, and the protocol has no way to ask for the larger one.
QString bestQuality(const QString& path) {
    const int dot = path.lastIndexOf(QChar('.'));
    if (dot <= 0 || path.endsWith(QStringLiteral("_m"), Qt::CaseInsensitive)) {
        return path;
    }
    const QString larger = path.left(dot) + QStringLiteral("_m") + path.mid(dot);
    return QFileInfo::exists(larger) ? larger : path;
}

/// What a track with no cover is drawn as: Mixxx's own placeholder, the same
/// one the deck's header falls back to. A bare dark square read as a panel that
/// had failed to draw its picture rather than as a track without one.
QPixmap defaultCover() {
    return QPixmap(CoverArtUtils::defaultCoverLocation());
}
} // namespace

namespace mixxx {
namespace deck {

WDeckInfoPanel::WDeckInfoPanel(QWidget* pParent)
        : QWidget(pParent) {
    setObjectName(QStringLiteral("DeckInfoPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
}

void WDeckInfoPanel::setTrack(const QString& coverPath,
        const QList<QPair<QString, QString>>& fields) {
    if (coverPath != m_coverPath) {
        m_coverPath = coverPath;
        // Loaded here rather than in paintEvent: the panel repaints on every
        // encoder detent, and decoding a JPEG per frame is visible on a Pi.
        m_cover = coverPath.isEmpty() ? QPixmap() : QPixmap(bestQuality(coverPath));
        // Whether what is on screen is the track's own cover or the stand-in.
        // This is the question reloadCover() has to ask, and "is it null" stopped
        // answering it the moment there was always something to draw: the
        // placeholder would count as loaded and a cover arriving afterwards
        // would never replace it.
        m_coverIsPlaceholder = m_cover.isNull();
        if (m_coverIsPlaceholder) {
            m_cover = defaultCover();
        }
    }
    m_fields = fields;
    update();
}

void WDeckInfoPanel::reloadCover() {
    // The cover is decoded once, in setTrack, and setTrack short-circuits when
    // the path has not changed -- so a cover that arrives *after* the panel was
    // filled in would never be picked up without this.
    if (m_coverPath.isEmpty() || !m_coverIsPlaceholder) {
        return;
    }
    const QPixmap cover(bestQuality(m_coverPath));
    if (cover.isNull()) {
        return;
    }
    m_cover = cover;
    m_coverIsPlaceholder = false;
    update();
}

void WDeckInfoPanel::setPreview(const PreviewWaveform& preview) {
    if (preview.isNull()) {
        m_preview = QPixmap();
        update();
        return;
    }

    QPixmap strip(kPreviewWidth, kPreviewHeight);
    strip.fill(Qt::transparent);
    QPainter painter(&strip);
    // Off deliberately: every bar is one pixel wide and sits on an integer x,
    // so there is nothing to smooth and smoothing it would grey the edges of a
    // picture whose whole content is edges.
    painter.setRenderHint(QPainter::Antialiasing, false);

    for (int column = 0; column < kPreviewWidth; ++column) {
        const PreviewWaveform::Column bar = preview.at(column);
        const int height = bar.height * kPreviewHeight / 255;
        if (height <= 0) {
            continue;
        }
        // The colour is the column's own -- three frequency bands off the
        // colour preview, or blue going white with the shade when only the
        // monochrome one was there. Which of the two it came from is settled
        // before this and deliberately invisible here: the panel draws one kind
        // of picture.
        painter.setPen(QColor(bar.red, bar.green, bar.blue));
        // Drawn up from the baseline, the way a CDJ draws a preview, rather
        // than mirrored about the middle the way Mixxx draws an overview. Half
        // the pixels for the same information, and the shape a DJ already reads.
        painter.drawLine(column,
                kPreviewHeight - 1,
                column,
                kPreviewHeight - height);
    }

    m_preview = strip;
    update();
}

void WDeckInfoPanel::clear() {
    m_coverPath.clear();
    // Null, not the placeholder: there is no track selected at all, and a
    // record sleeve drawn for nothing is worse than an empty panel.
    m_cover = QPixmap();
    m_coverIsPlaceholder = false;
    m_fields.clear();
    m_preview = QPixmap();
    update();
}

void WDeckInfoPanel::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    const int coverLeft = (width() - kCoverSize) / 2;
    const QRect coverRect(coverLeft, kPadding, kCoverSize, kCoverSize);
    painter.fillRect(coverRect, kCoverBackdrop);
    if (!m_cover.isNull()) {
        painter.drawPixmap(coverRect,
                m_cover.scaled(kCoverSize,
                        kCoverSize,
                        Qt::KeepAspectRatioByExpanding,
                        Qt::SmoothTransformation));
    }

    // The strip, always in the same place whether or not there is a waveform to
    // put in it -- see setPreview() for why it does not collapse.
    const QRect previewRect((width() - kPreviewWidth) / 2,
            coverRect.bottom() + kGap,
            kPreviewWidth,
            kPreviewHeight);
    if (m_preview.isNull()) {
        painter.fillRect(previewRect.left(),
                previewRect.bottom() - 1,
                previewRect.width(),
                1,
                kWaveBaseline);
    } else {
        painter.drawPixmap(previewRect.topLeft(), m_preview);
    }

    QFont labelFont = font();
    labelFont.setPixelSize(15);
    QFont valueFont = font();
    valueFont.setPixelSize(17);

    int y = previewRect.bottom() + kGap;
    for (const auto& field : m_fields) {
        if (y + kRowHeight > height()) {
            break; // Ran out of panel; the rest is simply not shown.
        }
        const QRect labelRect(kPadding, y, kLabelWidth, kRowHeight);
        const QRect valueRect(kPadding + kLabelWidth,
                y,
                width() - kLabelWidth - 2 * kPadding,
                kRowHeight);

        painter.setFont(labelFont);
        painter.setPen(kLabel);
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, field.first);

        painter.setFont(valueFont);
        painter.setPen(kValue);
        const QFontMetrics metrics(valueFont);
        // Elided, not wrapped: a comment that ran to three lines would push
        // everything below it off the panel, and the fields underneath are
        // usually the ones being looked for.
        painter.drawText(valueRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                metrics.elidedText(field.second, Qt::ElideRight, valueRect.width()));
        y += kRowHeight;
    }
}

} // namespace deck
} // namespace mixxx

#include "moc_wdeckinfopanel.cpp"
