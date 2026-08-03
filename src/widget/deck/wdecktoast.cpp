#include "widget/deck/wdecktoast.h"

#include <QDateTime>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "library/deck/trackcache.h"
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
    // Somebody else's deck is playing off this stick, and it now goes on doing
    // so from a copy in our RAM. Said first, because it is the one a DJ has to
    // act on: our own deck recovering is our business, but a CDJ on the other
    // side of the booth about to stop is theirs.
    MediaRegistry* pRegistry = MediaRegistry::instance();
    if (pRegistry) {
        const mixxx::prolink::server::ServeStatus serve = pRegistry->serveStatus();
        QList<int> fed;
        for (const mixxx::prolink::server::ServedSlot& slot : serve.media) {
            if (!slot.phantom || slot.localPath != medium.id.mountPoint()) {
                continue;
            }
            for (const mixxx::prolink::server::ServeConsumer& reader : serve.consumers) {
                if (reader.slot == slot.slot && !fed.contains(reader.deviceNumber)) {
                    fed.append(reader.deviceNumber);
                }
            }
        }
        if (!fed.isEmpty()) {
            QStringList numbers;
            for (int number : fed) {
                numbers.append(QString::number(number));
            }
            show(medium,
                    tr("%1 removed — player %2 is still being fed from cache.")
                            .arg(medium.name, numbers.join(QStringLiteral(", "))),
                    true);
            return;
        }
    }

    // The deck plays from a copy, so pulling a stick mid-track is survivable --
    // and worth saying, because everything a DJ knows about CDJs says it should
    // not be. The message is only earned when there is genuinely a pinned copy,
    // though: claiming it and then falling silent would be worse than saying
    // nothing.
    TrackCache* pCache = TrackCache::instance();
    if (pCache && pCache->hasPinnedFrom(medium.id)) {
        show(medium,
                tr("%1 removed — current track is cached and stays playable.")
                        .arg(medium.name),
                true);
        return;
    }
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
