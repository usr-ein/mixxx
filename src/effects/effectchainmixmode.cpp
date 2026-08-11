#include "effects/effectchainmixmode.h"

namespace {
const QString kDrySlashWetString = QStringLiteral("DRY/WET");
const QString kDryPlusWetString = QStringLiteral("DRY+WET");
const QString kWetOnlyString = QStringLiteral("WET");
} // anonymous namespace

QString EffectChainMixMode::toString(EffectChainMixMode::Type type) {
    if (type == EffectChainMixMode::DryPlusWet) {
        return kDryPlusWetString;
    }
    if (type == EffectChainMixMode::WetOnly) {
        return kWetOnlyString;
    }
    return kDrySlashWetString;
}

EffectChainMixMode::Type EffectChainMixMode::fromString(const QString& string) {
    if (string == kDryPlusWetString) {
        return Type::DryPlusWet;
    }
    if (string == kWetOnlyString) {
        return Type::WetOnly;
    }
    return Type::DrySlashWet;
}
