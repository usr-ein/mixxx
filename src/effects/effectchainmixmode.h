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

    /// How many modes the `mix_mode` push button cycles through.
    ///
    /// Deliberately fewer than `kNumModes`. That control is a TOGGLE sized by
    /// whatever it is given, and it exists on *every* chain type -- QuickEffect
    /// and Equalizer chains included, where WetOnly is meaningless and would
    /// silently kill the dry. Leaving it at two keeps every existing skin's
    /// mix-mode button behaving exactly as it did.
    ///
    /// WetOnly is reached by setting `mix_mode` directly, which is what a chain
    /// preset does when it carries `WET`.
    static constexpr int kNumToggleModes = 2;

    static QString toString(Type type);
    static Type fromString(const QString& string);
};
