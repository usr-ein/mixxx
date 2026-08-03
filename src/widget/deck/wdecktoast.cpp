#include "widget/deck/wdecktoast.h"

#include <QDateTime>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("DeckToast");

constexpr int kWidth = 360;
constexpr int kWideWidth = 460;
constexpr int kHeight = 72;
constexpr int kMargin = 16;
constexpr int kGap = 8;
/// How long a toast stays up. Long enough to read across a booth, short enough
/// not to sit over the waveform through a mix.
constexpr qint64 kLifetimeMs = 3200;
/// Beyond this the oldest is retired immediately rather than queued: four
/// stacked notifications is a wall, not a notification.
constexpr int kMaxToasts = 3;

/// A toast is its own widget so it can be touched. The container above it is
/// transparent to the mouse; this is not.
class ToastWidget : public QLabel {
  public:
    ToastWidget(QWidget* pParent, const QString& text, int width)
            : QLabel(text, pParent) {
        setObjectName(QStringLiteral("DeckToastItem"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedSize(width, kHeight);
        setWordWrap(true);
        setMargin(12);
    }

  protected:
    void mousePressEvent(QMouseEvent* pEvent) override {
        // Touching one dismisses it. Deleting rather than hiding, so the stack
        // above closes up rather than leaving a gap.
        Q_UNUSED(pEvent);
        deleteLater();
    }
};
} // namespace

namespace mixxx {
namespace deck {

WDeckToast::WDeckToast(QWidget* pParent)
        : QWidget(pParent), WBaseWidget(this) {
    setObjectName(QStringLiteral("DeckToast"));
    // The whole point: this covers the panel so a stacked layout will centre it
    // at full size, and it must not eat a single tap meant for what is behind.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_tick.setInterval(200);
    connect(&m_tick, &QTimer::timeout, this, &WDeckToast::expire);

    MediaRegistry* pRegistry = MediaRegistry::instance();
    if (!pRegistry) {
        // The browser builds the registry, and the skin may put us first. Not
        // fatal, but it does mean no toasts, so it is worth saying.
        kLogger.warning() << "no media registry yet; no toasts will be shown";
        return;
    }
    connect(pRegistry, &MediaRegistry::mediumAppeared, this, &WDeckToast::onAppeared);
    connect(pRegistry, &MediaRegistry::mediumVanished, this, &WDeckToast::onVanished);
    connect(pRegistry, &MediaRegistry::mediumFailed, this, &WDeckToast::onFailed);
}

void WDeckToast::setup(const QDomNode& node, const SkinContext& context) {
    Q_UNUSED(node);
    Q_UNUSED(context);
}

void WDeckToast::onAppeared(mixxx::deck::MediumInfo medium) {
    show(medium, tr("%1 inserted").arg(medium.name), false);
}

void WDeckToast::onVanished(mixxx::deck::MediumInfo medium) {
    // TODO(cache): once tracks play from a local cache, an eject while playing
    // from this medium says so instead -- "SAM2 removed, current track is
    // cached and stays playable" (browser-prd.md 12.3). Until the cache exists
    // that sentence would be a lie, so it is not said.
    show(medium, tr("%1 ejected").arg(medium.name), false);
}

void WDeckToast::onFailed(mixxx::deck::MediumInfo medium) {
    show(medium,
            tr("%1 — %2").arg(medium.name, medium.error),
            true);
}

void WDeckToast::show(const MediumInfo& medium, const QString& text, bool wide) {
    Q_UNUSED(medium);
    while (m_toasts.size() >= kMaxToasts) {
        Toast oldest = m_toasts.takeFirst();
        if (oldest.pWidget) {
            oldest.pWidget->deleteLater();
        }
    }

    auto* pToast = new ToastWidget(this, text, wide ? kWideWidth : kWidth);
    // Deleted by its own tap as well as by expiry, so the list has to notice
    // either way.
    connect(pToast, &QObject::destroyed, this, [this](QObject* pObject) {
        for (int i = 0; i < m_toasts.size(); ++i) {
            if (m_toasts.at(i).pWidget == pObject) {
                m_toasts.removeAt(i);
                break;
            }
        }
        reposition();
    });

    Toast toast;
    toast.pWidget = pToast;
    toast.expiresAt = QDateTime::currentMSecsSinceEpoch() + kLifetimeMs;
    m_toasts.append(toast);

    pToast->show();
    pToast->raise();
    reposition();
    m_tick.start();

    kLogger.info() << text;
}

void WDeckToast::reposition() {
    // Newest at the top, stacking downward from the top right corner.
    int y = kMargin;
    for (const Toast& toast : m_toasts) {
        if (!toast.pWidget) {
            continue;
        }
        toast.pWidget->move(width() - toast.pWidget->width() - kMargin, y);
        y += toast.pWidget->height() + kGap;
    }
}

void WDeckToast::expire() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_toasts.size() - 1; i >= 0; --i) {
        if (m_toasts.at(i).expiresAt > now) {
            continue;
        }
        QWidget* pWidget = m_toasts.at(i).pWidget;
        m_toasts.removeAt(i);
        if (pWidget) {
            pWidget->deleteLater();
        }
    }
    reposition();
    if (m_toasts.isEmpty()) {
        m_tick.stop();
    }
}

} // namespace deck
} // namespace mixxx

#include "moc_wdecktoast.cpp"
