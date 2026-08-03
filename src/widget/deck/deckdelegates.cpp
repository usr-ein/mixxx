#include "widget/deck/deckdelegates.h"

#include <QFileInfo>
#include <QPainter>
#include <QStringList>

#include "library/dao/trackschema.h"
#include "library/deck/deckqueries.h"
#include "library/deck/mediaregistry.h"
#include "widget/deck/deckmenumodel.h"

namespace {
// The palette. Deliberately the skin's, spelled out here rather than pulled
// from the stylesheet: a QStyledItemDelegate paints raw, and a half-styled row
// is worse than an unstyled one.
const QColor kBackground(0x0c, 0x0c, 0x0c);
const QColor kSelected(0x88, 0xff, 0x00);
const QColor kSelectedText(0x0c, 0x0c, 0x0c);
const QColor kText(0xee, 0xee, 0xee);
const QColor kDimText(0x77, 0x77, 0x77);
const QColor kDetail(0xaa, 0xaa, 0xaa);
const QColor kSeparator(0x22, 0x22, 0x22);
/// Harmonically compatible keys (browser-prd.md 8.3).
const QColor kKeyCompatible(0x44, 0xff, 0x66);
/// The bar down the left of the row that is on the deck right now.
const QColor kPlayingMark(0xff, 0x66, 0x00);

constexpr int kStripeWidth = 5;

constexpr int kPadding = 16;
constexpr int kCoverMargin = 8;

/// Load a cover, and ask for it if it is not there.
///
/// The request is what makes a remote medium's art appear at all: those images
/// live on the player, and only the rows a DJ is actually looking at are worth
/// the round trip -- a medium holds hundreds, and fetching the lot on detection
/// would put all of them in front of whatever the DJ does next.
///
/// Idempotent and cheap: MediaRegistry asks the network at most once per path,
/// ever. So calling this from a paint is safe, and a paint is the only place
/// that knows which covers are being looked at.
QPixmap loadCover(const QString& path) {
    QPixmap cover(path);
    if (cover.isNull() && !path.isEmpty()) {
        if (mixxx::deck::MediaRegistry* pRegistry = mixxx::deck::MediaRegistry::instance()) {
            pRegistry->requestArtwork(path);
        }
    }
    return cover;
}

/// The mark on the left of a menu row. Drawn rather than loaded: these are
/// four glyph-sized shapes, and an SVG each would be four files to keep in step
/// with the palette above.
void paintMark(QPainter* pPainter,
        const QRect& rect,
        mixxx::deck::MenuRow::Mark mark,
        const QColor& colour) {
    using Mark = mixxx::deck::MenuRow::Mark;
    if (mark == Mark::None) {
        return;
    }
    pPainter->save();
    pPainter->setPen(QPen(colour, 2));
    pPainter->setBrush(Qt::NoBrush);

    const int size = qMin(rect.width(), rect.height());
    const QRect box(rect.center().x() - size / 2, rect.center().y() - size / 2, size, size);

    switch (mark) {
    case Mark::Usb:
    case Mark::UsbLinked: {
        // A stick: a body with a connector at the top.
        const QRect body = box.adjusted(size / 4, size / 3, -size / 4, 0);
        pPainter->drawRect(body);
        pPainter->drawLine(box.center().x(), box.top() + size / 6,
                box.center().x(), body.top());
        break;
    }
    case Mark::Sd:
    case Mark::SdLinked: {
        // A card: a rectangle with the corner cut off.
        QPolygon card;
        card << QPoint(box.left() + size / 5, box.top() + size / 4)
             << QPoint(box.right() - size / 5 - size / 6, box.top() + size / 4)
             << QPoint(box.right() - size / 5, box.top() + size / 4 + size / 6)
             << QPoint(box.right() - size / 5, box.bottom() - size / 4)
             << QPoint(box.left() + size / 5, box.bottom() - size / 4);
        pPainter->drawPolygon(card);
        break;
    }
    case Mark::Folder: {
        const QRect body = box.adjusted(size / 6, size / 3, -size / 6, -size / 4);
        pPainter->drawRect(body);
        pPainter->drawLine(body.left(), body.top(),
                body.left() + size / 4, body.top() - size / 8);
        break;
    }
    case Mark::Search: {
        const int r = size / 4;
        pPainter->drawEllipse(box.center() - QPoint(r / 2, r / 2), r, r);
        pPainter->drawLine(box.center() + QPoint(r / 2, r / 2),
                box.center() + QPoint(r + 2, r + 2));
        break;
    }
    case Mark::Diagnostics: {
        // A gauge: an arc with a needle.
        pPainter->drawArc(box.adjusted(size / 5, size / 5, -size / 5, -size / 5),
                0 * 16, 180 * 16);
        pPainter->drawLine(box.center(), box.center() + QPoint(size / 5, -size / 5));
        break;
    }
    case Mark::Power: {
        const QRect ring = box.adjusted(size / 4, size / 4, -size / 4, -size / 4);
        pPainter->drawArc(ring, 60 * 16, 300 * 16);
        pPainter->drawLine(ring.center().x(), ring.top() - size / 12,
                ring.center().x(), ring.center().y() - size / 8);
        break;
    }
    case Mark::None:
        break;
    }

    // The link mark: two chevrons, drawn before the medium mark, saying this
    // one is somebody else's.
    if (mark == Mark::UsbLinked || mark == Mark::SdLinked) {
        const int x = rect.left() - size / 2;
        const int y = rect.center().y();
        pPainter->drawLine(x, y - 4, x + 6, y);
        pPainter->drawLine(x + 6, y, x, y + 4);
    }
    pPainter->restore();
}
} // namespace

namespace mixxx {
namespace deck {

MenuRowDelegate::MenuRowDelegate(QObject* pParent)
        : QStyledItemDelegate(pParent), m_coverCache(64) {
}

QSize MenuRowDelegate::sizeHint(const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    Q_UNUSED(index);
    return QSize(option.rect.width(), m_rowHeight);
}

QPixmap MenuRowDelegate::coverFor(const QStringList& paths, int size) const {
    if (paths.isEmpty()) {
        return QPixmap();
    }
    const QString cacheKey = paths.join(QChar('|')) + QStringLiteral("@%1").arg(size);
    if (QPixmap* pCached = m_coverCache.object(cacheKey)) {
        return *pCached;
    }

    QPixmap stitched(size, size);
    stitched.fill(QColor(0x1a, 0x1a, 0x1a));
    QPainter painter(&stitched);
    if (paths.size() == 1) {
        // One cover fills the square; a 2x2 with three empty cells would look
        // like three missing covers rather than one present one.
        QPixmap cover = loadCover(paths.first());
        if (!cover.isNull()) {
            painter.drawPixmap(stitched.rect(),
                    cover.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation));
        }
    } else {
        const int half = size / 2;
        for (int i = 0; i < paths.size() && i < 4; ++i) {
            QPixmap cover = loadCover(paths.at(i));
            if (cover.isNull()) {
                continue;
            }
            const QRect cell((i % 2) * half, (i / 2) * half, half, half);
            painter.drawPixmap(cell,
                    cover.scaled(half, half, Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation));
        }
    }
    painter.end();

    m_coverCache.insert(cacheKey, new QPixmap(stitched));
    return stitched;
}

void MenuRowDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    const QVariant data = index.data(DeckMenuModel::RowDataRole);
    if (!data.canConvert<MenuRow>()) {
        return;
    }
    const MenuRow row = data.value<MenuRow>();
    const bool selected = option.state & QStyle::State_Selected;

    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing, true);
    pPainter->fillRect(option.rect, selected ? kSelected : kBackground);

    QColor textColour = selected ? kSelectedText : (row.dimmed ? kDimText : kText);
    QColor detailColour = selected ? kSelectedText : (row.dimmed ? kDimText : kDetail);

    QRect content = option.rect.adjusted(kPadding, 0, -kPadding, 0);

    // Cover, where there is one, at the left.
    if (!row.coverPaths.isEmpty()) {
        const int coverSize = option.rect.height() - 2 * kCoverMargin;
        const QPixmap cover = coverFor(row.coverPaths, coverSize);
        if (!cover.isNull()) {
            pPainter->drawPixmap(content.left(), content.top() + kCoverMargin, cover);
        }
        content.setLeft(content.left() + coverSize + kPadding);
    } else if (row.mark != MenuRow::Mark::None) {
        const int markSize = qMin(32, option.rect.height() - 2 * kCoverMargin);
        const QRect markRect(content.left() + 8,
                content.center().y() - markSize / 2,
                markSize,
                markSize);
        paintMark(pPainter, markRect, row.mark, textColour);
        content.setLeft(content.left() + markSize + kPadding + 8);

        // Which physical port the stick is in, beside its mark. Slots are
        // handed out in plug order, so this is the only thing on screen that
        // says which of the two to pull -- and pulling the wrong one mid-set is
        // the mistake it exists to prevent.
        //
        // Dimmer than the name, and dimmer than a Pro DJ Link player number,
        // because the two sit in the same place and mean different things: a
        // port on this deck versus a device on the network.
        if (row.slot > 0) {
            QFont slotFont = option.font;
            slotFont.setPixelSize(17);
            pPainter->setFont(slotFont);
            pPainter->setPen(selected ? kSelectedText : kDimText);
            const QRect slotRect(content.left() - kPadding, content.top(), 18, content.height());
            pPainter->drawText(slotRect,
                    Qt::AlignLeft | Qt::AlignVCenter,
                    QString::number(row.slot));
            content.setLeft(content.left() + 14);
        }
    }

    QFont titleFont = option.font;
    titleFont.setPixelSize(22);
    pPainter->setFont(titleFont);

    // The detail is measured first and reserved, so a long title elides into
    // whatever is left rather than running underneath the counts.
    int detailWidth = 0;
    if (!row.detail.isEmpty()) {
        QFont detailFont = option.font;
        detailFont.setPixelSize(17);
        const QFontMetrics detailMetrics(detailFont);
        detailWidth = detailMetrics.horizontalAdvance(row.detail) + kPadding;
        pPainter->setFont(detailFont);
        pPainter->setPen(detailColour);
        pPainter->drawText(content, Qt::AlignRight | Qt::AlignVCenter, row.detail);
        pPainter->setFont(titleFont);
    }

    QString title = row.title;
    if (row.playerNumber > 0) {
        // The player's number leads the name, as it does on a CDJ's own screen.
        title = QStringLiteral("%1  %2").arg(row.playerNumber).arg(title);
    }
    const QRect titleRect = content.adjusted(0, 0, -detailWidth, 0);
    const QFontMetrics titleMetrics(titleFont);
    pPainter->setPen(textColour);
    pPainter->drawText(titleRect,
            Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(title, Qt::ElideRight, titleRect.width()));

    if (!selected) {
        pPainter->setPen(kSeparator);
        pPainter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    }
    pPainter->restore();
}

TrackRowDelegate::TrackRowDelegate(QObject* pParent)
        : QStyledItemDelegate(pParent), m_coverCache(128) {
}

QSize TrackRowDelegate::sizeHint(const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    Q_UNUSED(index);
    return QSize(option.rect.width(), m_rowHeight);
}

QPixmap TrackRowDelegate::coverFor(const QString& path, int size) const {
    if (path.isEmpty()) {
        return QPixmap();
    }
    const QString cacheKey = path + QStringLiteral("@%1").arg(size);
    if (QPixmap* pCached = m_coverCache.object(cacheKey)) {
        return *pCached;
    }
    QPixmap cover = loadCover(path);
    if (cover.isNull()) {
        // Cached as null too: a missing cover is a disk hit per redraw
        // otherwise, and on a remote medium it is missing on purpose until the
        // artwork lands.
        m_coverCache.insert(cacheKey, new QPixmap());
        return QPixmap();
    }
    const QPixmap scaled = cover.scaled(size,
            size,
            Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation);
    m_coverCache.insert(cacheKey, new QPixmap(scaled));
    return scaled;
}

void TrackRowDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    const QAbstractItemModel* pModel = index.model();
    if (!pModel) {
        return;
    }
    const bool selected = option.state & QStyle::State_Selected;

    // Indices, resolved once by the browser when the model changed. A column
    // that is not there yields an empty string rather than a crash: a query
    // that omits one is a thin list, not a broken one.
    const auto columnValue = [&](int column) {
        if (column < 0) {
            return QString();
        }
        return index.sibling(index.row(), column).data(Qt::DisplayRole).toString();
    };

    pPainter->save();
    pPainter->fillRect(option.rect, selected ? kSelected : kBackground);

    const QColor textColour = selected ? kSelectedText : kText;
    const QColor detailColour = selected ? kSelectedText : kDetail;

    QRect content = option.rect.adjusted(kPadding, 0, -kPadding, 0);

    // Two marks down the left edge, in the same few pixels because they are
    // never both interesting at once: the rekordbox colour tag, and -- taking
    // precedence -- "this is the one playing".
    const bool isLoaded = m_columns.trackId >= 0 && m_loadedTrackId >= 0 &&
            columnValue(m_columns.trackId).toInt() == m_loadedTrackId;
    if (isLoaded) {
        pPainter->fillRect(QRect(option.rect.left(), option.rect.top(),
                                  kStripeWidth, option.rect.height()),
                kPlayingMark);
    } else if (m_columns.color >= 0) {
        bool ok = false;
        const int packed = columnValue(m_columns.color).toInt(&ok);
        if (ok && packed > 0) {
            pPainter->fillRect(QRect(option.rect.left(), option.rect.top(),
                                      kStripeWidth, option.rect.height()),
                    QColor((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF));
        }
    }

    const int coverSize = option.rect.height() - 2 * kCoverMargin;
    const QPixmap cover = coverFor(columnValue(m_columns.cover), coverSize);
    if (!cover.isNull()) {
        pPainter->drawPixmap(content.left(), content.top() + kCoverMargin, cover);
    }
    content.setLeft(content.left() + coverSize + kPadding);

    const QString title = columnValue(m_columns.title);
    const QString artist = columnValue(m_columns.artist);

    QFont titleFont = option.font;
    titleFont.setPixelSize(21);
    QFont smallFont = option.font;
    smallFont.setPixelSize(18);

    QRect leftRect = content;

    if (m_infoLayout) {
        // **Three things and no more: the cover, the title, and one value.**
        // The panel on the right is the whole point of this layout -- it says
        // everything about the selected track -- so anything the row repeats is
        // width taken from the title for no information. BPM and key are drawn
        // in the default layout and deliberately not here.
        //
        // The one value is whatever the list is sorted by, because that is what
        // the DJ is reading the list *along*; unsorted, it is the artist, which
        // is what the default layout would have shown.
        const int secondaryColumn =
                m_secondaryColumn >= 0 ? m_secondaryColumn : m_columns.artist;
        QString secondary;
        if (secondaryColumn >= 0) {
            secondary = index.sibling(index.row(), secondaryColumn)
                                .data(Qt::DisplayRole)
                                .toString();
        }
        const QFontMetrics smallMetrics(smallFont);
        const int secondaryWidth = secondary.isEmpty()
                ? 0
                : smallMetrics.horizontalAdvance(secondary) + kPadding;
        pPainter->setFont(smallFont);
        pPainter->setPen(detailColour);
        pPainter->drawText(leftRect, Qt::AlignRight | Qt::AlignVCenter, secondary);

        pPainter->setFont(titleFont);
        pPainter->setPen(textColour);
        const QFontMetrics titleMetrics(titleFont);
        const QRect titleRect = leftRect.adjusted(0, 0, -secondaryWidth, 0);
        pPainter->drawText(titleRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(title, Qt::ElideRight, titleRect.width()));
    } else {
        // Tempo and key down the right-hand edge, measured from the right
        // inwards so the title and artist share whatever is left rather than
        // pushing them off. Only in this layout: with the info panel open they
        // are on the panel, in a column that lines up down the screen.
        const int keyWidth = 70;
        const int bpmWidth = 90;
        const int keyId = columnValue(m_columns.keyId).toInt();

        const QRect keyRect(
                content.right() - keyWidth, content.top(), keyWidth, content.height());
        // Green AND bold when it will mix with what is playing. Colour alone is
        // a weak signal on a panel seen at arm's length in a dark room, and it
        // is the one piece of colour in the list that carries information
        // rather than state -- so it gets weight as well as hue.
        const bool compatible = key::isCompatible(m_playingKeyId, keyId);
        QFont keyFont = smallFont;
        keyFont.setBold(compatible);
        pPainter->setFont(keyFont);
        pPainter->setPen(selected ? kSelectedText : (compatible ? kKeyCompatible : kText));
        pPainter->drawText(keyRect,
                Qt::AlignRight | Qt::AlignVCenter,
                columnValue(m_columns.key));

        const QRect bpmRect(
                keyRect.left() - bpmWidth, content.top(), bpmWidth, content.height());
        pPainter->setFont(smallFont);
        pPainter->setPen(detailColour);
        pPainter->drawText(bpmRect,
                Qt::AlignRight | Qt::AlignVCenter,
                columnValue(m_columns.bpm));

        leftRect = content.adjusted(0, 0, -(keyWidth + bpmWidth + kPadding), 0);

        // Two columns: the title takes two thirds and the artist one.
        const int artistWidth = leftRect.width() / 3;
        const QRect titleRect = leftRect.adjusted(0, 0, -artistWidth, 0);
        const QRect artistRect(titleRect.right() + kPadding,
                leftRect.top(),
                artistWidth - kPadding,
                leftRect.height());

        pPainter->setFont(titleFont);
        pPainter->setPen(textColour);
        const QFontMetrics titleMetrics(titleFont);
        pPainter->drawText(titleRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(title, Qt::ElideRight, titleRect.width()));

        pPainter->setFont(smallFont);
        pPainter->setPen(detailColour);
        const QFontMetrics smallMetrics(smallFont);
        pPainter->drawText(artistRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                smallMetrics.elidedText(artist, Qt::ElideRight, artistRect.width()));
    }

    if (!selected) {
        pPainter->setPen(kSeparator);
        pPainter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    }
    pPainter->restore();
}

} // namespace deck
} // namespace mixxx

#include "moc_deckdelegates.cpp"
