#pragma once

#include "control/controlproxy.h"
#include "track/keyutils.h"
#include "widget/wlabel.h"

class WKey : public WLabel  {
    Q_OBJECT
  public:
    explicit WKey(const QString& group, QWidget* pParent = nullptr);

    void onConnectedControlChanged(double dParameter, double dValue) override;
    void setup(const QDomNode& node, const SkinContext& context) override;

  private slots:
    void setValue(double dValue);
    void keyNotationChanged(double dValue);
    void setCents();

  private:
    double m_dOldValue;
    bool m_displayCents;
    bool m_displayKey;
    /// Which spelling to draw, from the skin's `<Notation>`.
    ///
    /// `Custom` — the default, and what every stock skin gets — means "whatever
    /// the user picked in the preferences", which is what the global notation
    /// map holds. Naming one here overrides that for this widget alone, for a
    /// skin whose layout only works in one notation: `12A` is three characters
    /// wide and `G# minor` is eight.
    KeyUtils::KeyNotation m_notation;
    ControlProxy m_keyNotation;
    ControlProxy m_engineKeyDistance;
};
