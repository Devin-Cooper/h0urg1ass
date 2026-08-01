#pragma once

#include <cstdint>

#include "power/auto_cal.hpp"
#include "power/battery_model.hpp"
#include "settings/settings.hpp"

namespace board {

/// What one sample produced: the reading, plus anything the caller should
/// persist. Non-zero means "store this"; the caller owns the flash, because
/// SettingsStore is a main() local and this class has no business knowing it
/// exists.
struct BatteryUpdate {
    h0::BatteryReading reading;
    uint16_t newCalPermille = 0;
    uint16_t newFloorRawMv = 0;
};

/// The ADC side of the battery gauge.
///
/// Sampling policy is power-and-time.md section 7.4: 32 conversions after 8
/// discards, once per second, into an IIR at alpha 1/16.
///
/// Section 7.4 also prescribes holding the display steady until the backlight
/// duty has been unchanged for 2 s. That gate is deliberately NOT applied: the
/// settings screen exists to be looked at while the BRIGHT wheel is dragged,
/// and freezing the row exactly then is the opposite of useful. The 1 Hz
/// cadence and the 16-second IIR already make the bucket unflickerable.
class Battery {
public:
    void begin();

    /// Call every frame. Re-reads at most once per second.
    BatteryUpdate sample(uint64_t now, const h0::Settings& s);

private:
    uint16_t readRawMilliVolts();

    h0::BatteryFilter filter_;
    h0::AutoCal autoCal_;
    uint64_t lastSampleUs_ = 0;
    bool primed_ = false;
};

} // namespace board
