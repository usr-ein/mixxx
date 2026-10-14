#include "widget/deck/wdeckkeypicker.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <cmath>
#include <utility>

#include "control/controlencoder.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "library/deck/deckservices.h"
#include "mixer/playerinfo.h"
#include "moc_wdeckkeypicker.cpp"
#include "network/prolink/keysync.h"
#include "track/keyutils.h"
#include "widget/deck/deckaccent.h"
#include "widget/deck/decklistview.h"
#include "widget/deck/deckmenumodel.h"

using mixxx::prolink::KeySync;

namespace {

const QString kGroup = QStringLiteral("[KeyPicker]");

constexpr int kListWidth = 420;
constexpr int kRowHeight = 56;
/// Nine of the twelve at once: the list opens with the key in force in the
/// middle, and an odd count puts four whole rows either side of it -- with
/// eight, the rows at both edges were cut in half.
constexpr int kVisibleRows = 9;
/// The list's border, from the skin (`#DeckKeyPicker QListView`): a QListView
/// is a QFrame and insets its rows by it.
constexpr int kFrameWidth = 1;

// The delegates' palette (deckdelegates.cpp), and the skin's purple for a
// shifted key (#KeyBadgeSynced in style.qss).
const QColor kBackground(0x0c, 0x0c, 0x0c);
const QColor kSelectedText(0x0c, 0x0c, 0x0c);
const QColor kText(0xee, 0xee, 0xee);
const QColor kDetail(0xaa, 0xaa, 0xaa);
const QColor kShifted(0xb0, 0x6c, 0xff);

constexpr int kPadding = 16;
constexpr int kStripeWidth = 5;
constexpr int kCodeWidth = 64;
constexpr int kShiftWidth = 48;

/// The row a shift is on: +6 at the top.
int rowOf(int shift) {
    return KeySync::kHighestShift - shift;
}

QString shiftText(int shift) {
    if (shift > 0) {
        return QStringLiteral("+%1").arg(shift);
    }
    if (shift < 0) {
        // A real minus, the width of the plus, so the column lines up.
        return QStringLiteral("−%1").arg(-shift);
    }
    return QStringLiteral("0");
}

} // namespace

namespace mixxx {
namespace deck {

/// One key of the list: code, name, and the shift that reaches it.
///
/// Its own painter rather than MenuRowDelegate, because the row is three
/// columns that have to line up down the list -- the code, the name, the
/// shift -- and two marks no menu row has: the key in force, and the original.
class KeyRowDelegate : public QStyledItemDelegate {
  public:
    explicit KeyRowDelegate(QObject* pParent)
            : QStyledItemDelegate(pParent),
              m_accent(deckAccent()) {
    }

    void setShift(int shift) {
        m_shift = shift;
    }

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        const QVariant data = index.data(DeckMenuModel::RowDataRole);
        if (!data.canConvert<MenuRow>()) {
            return;
        }
        const MenuRow row = data.value<MenuRow>();
        const int shift = row.payload.toInt();
        const bool selected = option.state & QStyle::State_Selected;
        const bool inForce = shift == m_shift;
        // The key in force, in the colour the header's key readout has: the
        // accent while the track is in its own key, purple once it is not.
        const QColor inForceColour = m_shift != 0 ? kShifted : m_accent;

        pPainter->save();
        pPainter->setRenderHint(QPainter::Antialiasing, true);
        pPainter->fillRect(option.rect, selected ? m_accent : kBackground);
        if (inForce) {
            // Kept under the selection too: it is how the DJ tells the key
            // they are on from the one they are pointing at.
            pPainter->fillRect(QRect(option.rect.left(),
                                       option.rect.top(),
                                       kStripeWidth,
                                       option.rect.height()),
                    inForceColour);
        }
        const QColor text = selected ? kSelectedText : (inForce ? inForceColour : kText);
        const QColor detail = selected ? kSelectedText : (inForce ? inForceColour : kDetail);

        QRect content = option.rect.adjusted(kStripeWidth + kPadding, 0, -kPadding, 0);

        QFont font = option.font;
        font.setPixelSize(22);
        pPainter->setFont(font);
        pPainter->setPen(text);
        pPainter->drawText(QRect(content.right() - kShiftWidth,
                                   content.top(),
                                   kShiftWidth,
                                   content.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                shiftText(shift));
        content.setRight(content.right() - kShiftWidth - kPadding);

        if (shift == 0) {
            QFont tagFont = option.font;
            tagFont.setPixelSize(15);
            pPainter->setFont(tagFont);
            pPainter->setPen(detail);
            const QString tag = QStringLiteral("original");
            pPainter->drawText(content, Qt::AlignRight | Qt::AlignVCenter, tag);
            content.setRight(content.right() -
                    QFontMetrics(tagFont).horizontalAdvance(tag) - kPadding);
        }

        // Code and name. Both empty for a track whose own key is unknown:
        // its list is twelve shifts, and says nothing it does not know.
        QFont codeFont = option.font;
        codeFont.setPixelSize(22);
        codeFont.setBold(true);
        pPainter->setFont(codeFont);
        pPainter->setPen(text);
        pPainter->drawText(QRect(content.left(), content.top(), kCodeWidth, content.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                row.title);
        content.setLeft(content.left() + kCodeWidth);
        pPainter->setFont(font);
        pPainter->setPen(detail);
        pPainter->drawText(content,
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(font).elidedText(row.detail, Qt::ElideRight, content.width()));
        pPainter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        Q_UNUSED(index);
        return QSize(option.rect.width(), kRowHeight);
    }

  private:
    const QColor m_accent;
    int m_shift = 0;
};

WDeckKeyPicker::WDeckKeyPicker(QWidget* pParent)
        : QWidget(pParent),
          WBaseWidget(this) {
    setObjectName(QStringLiteral("DeckKeyPicker"));

    m_pView = new DeckListView(this);
    m_pModel = new DeckMenuModel(this);
    m_pDelegate = new KeyRowDelegate(this);
    m_pView->setRowHeight(kRowHeight);
    m_pView->setModel(m_pModel);
    m_pView->setItemDelegate(m_pDelegate);
    m_pView->setFixedSize(kListWidth, kVisibleRows * kRowHeight + 2 * kFrameWidth);
    // A pop-over, as the sort menu is: a finger scrolls to reach a row and the
    // selection stays put, and one tap on a row picks it.
    m_pView->setCentreSelection(false);
    m_pView->setTapActivates(true);
    connect(m_pView, &DeckListView::activated, this, &WDeckKeyPicker::pick);
    connect(m_pView, &DeckListView::backRequested, this, &WDeckKeyPicker::dismiss);

    m_pOpen = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("open")));
    connect(m_pOpen.get(), &ControlObject::valueChanged, this, [this](double value) {
        if (value > 0.0) {
            open();
        } else {
            dismiss();
        }
    });
    m_pMove = std::make_unique<ControlEncoder>(ConfigKey(kGroup, QStringLiteral("move")), false);
    connect(m_pMove.get(), &ControlEncoder::valueChanged, this, [this](double value) {
        const int steps = static_cast<int>(value);
        if (steps != 0 && isVisible()) {
            m_pView->moveSelection(steps);
        }
    });
    m_pSelect = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("select")));
    connect(m_pSelect.get(), &ControlPushButton::valueChanged, this, [this](double value) {
        if (value > 0.0 && isVisible()) {
            pick(m_pView->selectedRow());
        }
    });

    const QString deck = DeckServices::deckGroup();
    m_pKeyShift = std::make_unique<ControlProxy>(QStringLiteral("[ProLink]"),
            QStringLiteral("key_shift"),
            this,
            ControlFlag::NoWarnIfMissing);
    m_pKeyShift->connectValueChanged(this, [this](double) {
        if (isVisible()) {
            showShift();
        }
    });
    m_pFileKey = std::make_unique<ControlProxy>(deck, QStringLiteral("file_key"), this);
    // A key arriving while the list is up, analysed on the deck: the rows get
    // their names, and the selection stays where the DJ left it.
    m_pFileKey->connectValueChanged(this, [this](double) {
        if (isVisible()) {
            const int row = m_pView->selectedRow();
            showRows();
            m_pView->selectRow(row);
        }
    });
    m_pTrackLoaded = std::make_unique<ControlProxy>(deck, QStringLiteral("track_loaded"), this);
    m_pShowLibrary = std::make_unique<ControlProxy>(
            QStringLiteral("[Master]"), QStringLiteral("show_library"), this);
    m_pShowLibrary->connectValueChanged(this, [this](double value) {
        if (value > 0.0) {
            dismiss();
        }
    });
    // A list of the keys of a track that has gone would pitch the next one
    // by a shift chosen for another key.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            [this](const QString& group, TrackPointer, TrackPointer) {
                if (group == DeckServices::deckGroup()) {
                    dismiss();
                }
            });

    hide();
}

WDeckKeyPicker::~WDeckKeyPicker() = default;

void WDeckKeyPicker::setup(const QDomNode& node, const SkinContext& context) {
    Q_UNUSED(node);
    Q_UNUSED(context);
}

void WDeckKeyPicker::open() {
    if (m_pShowLibrary->get() > 0.0 || m_pTrackLoaded->get() <= 0.0) {
        // Refused, and said so: the mapping reads `open` back to know where
        // the encoder goes.
        m_pOpen->set(0.0);
        return;
    }
    showRows();
    place();
    // A press off the list that something else closed it under never saw its
    // release here; this one starts clean.
    m_pressedOff = false;
    show();
    // Over the deck, and over whatever the stacked layout last raised.
    raise();
    m_pView->setFocus();
    const int row = rowOf(KeySync::foldShift(static_cast<int>(std::lround(m_pKeyShift->get()))));
    m_pView->selectRow(row);
    m_pView->scrollTo(m_pModel->index(row, 0), QAbstractItemView::PositionAtCenter);
}

void WDeckKeyPicker::dismiss() {
    if (isVisible()) {
        hide();
    }
    m_pOpen->set(0.0);
}

void WDeckKeyPicker::pick(int row) {
    if (row < 0 || row >= m_pModel->rowCount()) {
        return;
    }
    const int shift = m_pModel->rowAt(row).payload.toInt();
    dismiss();
    // ProLinkKeySync applies it, turns keylock on for a shift (owner decision
    // 12), and the pad and the header follow from there. Picking the key in
    // force writes what is already there, which changes nothing.
    m_pKeyShift->set(shift);
}

void WDeckKeyPicker::showRows() {
    const auto fileKey = KeyUtils::keyFromNumericValue(m_pFileKey->get());
    QList<MenuRow> rows;
    for (int shift = KeySync::kHighestShift; shift >= KeySync::kLowestShift; --shift) {
        MenuRow row;
        const auto key = KeySync::keyAtShift(fileKey, shift);
        if (key != mixxx::track::io::key::INVALID) {
            row.title = KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot);
            row.detail = KeyUtils::keyToString(key, KeyUtils::KeyNotation::Traditional);
        }
        row.payload = shift;
        rows.append(row);
    }
    m_pModel->setRows(rows);
    showShift();
}

void WDeckKeyPicker::showShift() {
    m_pDelegate->setShift(KeySync::foldShift(static_cast<int>(std::lround(m_pKeyShift->get()))));
    m_pView->viewport()->update();
}

void WDeckKeyPicker::place() {
    m_pView->move((width() - m_pView->width()) / 2, (height() - m_pView->height()) / 2);
}

void WDeckKeyPicker::resizeEvent(QResizeEvent* pEvent) {
    QWidget::resizeEvent(pEvent);
    place();
}

void WDeckKeyPicker::mousePressEvent(QMouseEvent* pEvent) {
    // Consumed either way: nothing under the list may hear a tap meant for it.
    pEvent->accept();
    // The list's own border is part of the list, not "outside" it.
    m_pressedOff = !m_pView->geometry().contains(pEvent->pos());
}

void WDeckKeyPicker::mouseReleaseEvent(QMouseEvent* pEvent) {
    pEvent->accept();
    // On the release, so the press and the release both end here: closed on
    // the press, the release went to whatever was under the finger.
    if (std::exchange(m_pressedOff, false)) {
        dismiss();
    }
}

} // namespace deck
} // namespace mixxx
