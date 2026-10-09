#include "widget/deck/wdeckautoplaybadge.h"

#include <QFontMetrics>
#include <QPainter>
#include <algorithm>

#include "library/deck/deckautoplay.h"
#include "moc_wdeckautoplaybadge.cpp"
#include "widget/deck/deckaccent.h"

namespace {
/// The tempo box's margins (wtempopanel.cpp), mirrored: the two boxes sit in
/// the waveform's bottom corners, the same distance in from each side.
constexpr int kPadLeft = 44;
constexpr int kPadBottom = 26;
constexpr int kBoxPadX = 14;
constexpr int kBoxPadY = 12;
constexpr int kBoxRadius = 4;
constexpr int kLineGap = 2;
/// A long genre is cut short rather than run across the waveform.
constexpr int kMaxGenreWidth = 320;
} // namespace

namespace mixxx {
namespace deck {

WDeckAutoplayBadge::WDeckAutoplayBadge(QWidget* pParent, DeckAutoplay* pAutoplay)
        : QWidget(pParent),
          WBaseWidget(this),
          m_pAutoplay(pAutoplay),
          m_accent(deckAccent()) {
    setObjectName(QStringLiteral("DeckAutoplayBadge"));
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    if (m_pAutoplay) {
        connect(m_pAutoplay, &DeckAutoplay::stateChanged, this, [this]() {
            update();
        });
    }
}

void WDeckAutoplayBadge::setup(const QDomNode& node, const SkinContext& context) {
    Q_UNUSED(node);
    Q_UNUSED(context);
}

void WDeckAutoplayBadge::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    raise();
}

void WDeckAutoplayBadge::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    if (!m_pAutoplay || !m_pAutoplay->isOn()) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    QFont labelFont = font();
    labelFont.setPixelSize(16);
    labelFont.setBold(true);
    labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    const QFontMetrics labelMetrics(labelFont);
    const QString label = tr("AUTOPLAY");

    QFont genreFont = font();
    genreFont.setPixelSize(22);
    const QFontMetrics genreMetrics(genreFont);
    const QString genre = genreMetrics.elidedText(
            m_pAutoplay->genreTitle(), Qt::ElideRight, kMaxGenreWidth);

    const int contentWidth = std::max(labelMetrics.horizontalAdvance(label),
            genreMetrics.horizontalAdvance(genre));
    const int contentHeight = labelMetrics.height() + kLineGap + genreMetrics.height();
    const QRect box(kPadLeft,
            height() - kPadBottom - contentHeight - 2 * kBoxPadY,
            contentWidth + 2 * kBoxPadX,
            contentHeight + 2 * kBoxPadY);

    // The tempo box's: solid black, so it reads over any part of the waveform,
    // and a faint border for the dark passages where the fill alone would not.
    painter.setPen(QPen(QColor(0x55, 0x55, 0x55), 1));
    painter.setBrush(QColor(0x00, 0x00, 0x00));
    painter.drawRoundedRect(box, kBoxRadius, kBoxRadius);

    const int left = box.left() + kBoxPadX;
    int baseline = box.top() + kBoxPadY + labelMetrics.ascent();
    painter.setFont(labelFont);
    painter.setPen(m_accent);
    painter.drawText(left, baseline, label);

    baseline += labelMetrics.descent() + kLineGap + genreMetrics.ascent();
    painter.setFont(genreFont);
    painter.setPen(QColor(0xee, 0xee, 0xee));
    painter.drawText(left, baseline, genre);
}

} // namespace deck
} // namespace mixxx
