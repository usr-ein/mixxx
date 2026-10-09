#include "widget/deck/menubarreveal.h"

#include <QApplication>
#include <QMenuBar>
#include <QMouseEvent>

#include "moc_menubarreveal.cpp"

namespace mixxx {
namespace deck {

MenuBarReveal::MenuBarReveal(QMenuBar* pMenuBar, QWidget* pCentralWidget, QObject* pParent)
        : QObject(pParent),
          m_pMenuBar(pMenuBar),
          m_pCentralWidget(pCentralWidget) {
    m_pMenuBar->setParent(m_pCentralWidget);
    m_pMenuBar->raise();
    m_pMenuBar->hide();
    m_pCentralWidget->window()->setMouseTracking(true);
    m_pCentralWidget->setMouseTracking(true);
    qApp->installEventFilter(this);
}

bool MenuBarReveal::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pEvent->type() == QEvent::MouseMove) {
        update(static_cast<QMouseEvent*>(pEvent));
    }
    return QObject::eventFilter(pObject, pEvent);
}

void MenuBarReveal::update(QMouseEvent* pEvent) {
    if (!m_pMenuBar || !m_pCentralWidget) {
        return;
    }
    // Buttons held means a drag, and a drag is what a finger does. Only a free
    // pointer counts.
    if (pEvent->buttons() != Qt::NoButton) {
        return;
    }
    const QPoint local = m_pCentralWidget->mapFromGlobal(pEvent->globalPosition().toPoint());

    if (!m_pMenuBar->isVisible()) {
        constexpr int kHotZoneHeight = 4;
        if (local.y() >= 0 && local.y() < kHotZoneHeight &&
                local.x() >= 0 && local.x() < m_pCentralWidget->width()) {
            m_pMenuBar->setGeometry(0, 0, m_pCentralWidget->width(),
                    m_pMenuBar->sizeHint().height());
            m_pMenuBar->show();
            m_pMenuBar->raise();
        }
        return;
    }

    // Hide again once the pointer leaves it -- but never while a menu is open,
    // or picking an item from it would dismiss the bar out from under the
    // pointer on the way down.
    if (m_pMenuBar->activeAction()) {
        return;
    }
    if (local.y() > m_pMenuBar->height()) {
        m_pMenuBar->hide();
    }
}

} // namespace deck
} // namespace mixxx
