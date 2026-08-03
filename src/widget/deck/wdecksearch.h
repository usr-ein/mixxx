#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QGridLayout;

namespace mixxx {
namespace deck {

/// The search screen's keyboard (browser-prd.md 10).
///
/// **Alphabetical, not QWERTY.** The person using this is finding a track, not
/// typing prose, and alphabetical is faster to *scan* — which is what you do on
/// a keyboard you touch twice a set. All caps, because the query line is too.
///
/// Touch only, and that is an accepted trade rather than an oversight: the
/// encoder walks the *results*, and walking twenty-six keys to spell DEAD would
/// be four times worse than not having search at all. It is the one place on
/// this deck that needs the panel.
class WDeckKeyboard : public QWidget {
    Q_OBJECT

  public:
    explicit WDeckKeyboard(QWidget* pParent = nullptr);

  signals:
    void keyPressed(const QString& character);
    void backspacePressed();
    void clearPressed();
    void donePressed();

  private:
    void buildKeys();
    void setPage(bool numbers);

    QGridLayout* m_pGrid;
    QList<QPushButton*> m_buttons;
    bool m_numbers = false;
};

} // namespace deck
} // namespace mixxx
