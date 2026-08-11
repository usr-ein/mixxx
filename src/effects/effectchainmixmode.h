#pragma once
#include <QString>

class EffectChainMixMode {
  public:
    enum Type {
        DrySlashWet = 0,
        DryPlusWet = 1,
        /// Output the wet signal alone; the dry never reaches the output.
        /// For a hardware send/return bus, where an external mixer already
        /// carries the dry and any dry returned here would be heard twice.
        WetOnly = 2
    };
    static constexpr int kNumModes = 3;

    static QString toString(Type type);
    static Type fromString(const QString& string);
};
