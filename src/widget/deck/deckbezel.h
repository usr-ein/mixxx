#pragma once

#include "control/controlobject.h"

namespace mixxx {
namespace deck {

/// Whether the deck's panel sits behind a lip, so the widgets on it have to keep
/// their outer rows clear. That is `[TriMixxx],bezel`, which the TriMixxx skin
/// declares -- 1 by default, for a panel recessed on every edge -- and a deck
/// with a flush panel turns off through mixxx_config/units/<deck>.json.
///
/// Read once, at construction: the skin creates its controls before it builds a
/// single widget, and a panel does not gain or lose a lip while running. No
/// control at all, i.e. any other skin, means the bezel, which is what these
/// widgets were laid out around.
inline bool panelHasBezel() {
    const ControlObject* pBezel = ControlObject::getControl(
            ConfigKey(QStringLiteral("[TriMixxx]"), QStringLiteral("bezel")),
            ControlFlag::NoWarnIfMissing);
    return pBezel == nullptr || pBezel->get() != 0.0;
}

} // namespace deck
} // namespace mixxx
