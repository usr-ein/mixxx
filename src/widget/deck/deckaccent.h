#pragma once

#include <QColor>

#include "control/controlobject.h"

namespace mixxx {
namespace deck {

/// This deck's accent: the colour of what is selected and of where you are.
/// That is `[TriMixxx],accent`, which the TriMixxx skin declares as an RGB
/// integer -- 0x88FF00, Trimixxx1's lime, by default -- and a deck with a colour
/// of its own sets through mixxx_config/units/<deck>.json, which recolours the
/// skin's stylesheet and icons to match on the way out.
///
/// For the widgets that paint themselves, and so cannot take it from the
/// stylesheet. Read once, at construction, like panelHasBezel(): the skin
/// creates its controls before it builds a single widget. No control at all,
/// i.e. any other skin, means the lime.
inline QColor deckAccent() {
    constexpr QRgb kDefault = 0x88FF00;
    const ControlObject* pAccent = ControlObject::getControl(
            ConfigKey(QStringLiteral("[TriMixxx]"), QStringLiteral("accent")),
            ControlFlag::NoWarnIfMissing);
    const double value = pAccent ? pAccent->get() : kDefault;
    // Anything that is not a 24-bit RGB value falls back, rather than painting
    // a colour nobody chose.
    if (!(value >= 0.0 && value <= 0xFFFFFF)) {
        return QColor::fromRgb(kDefault);
    }
    return QColor::fromRgb(static_cast<QRgb>(value));
}

} // namespace deck
} // namespace mixxx
