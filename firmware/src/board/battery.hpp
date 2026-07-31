#pragma once

#include <cstdint>

#include "power/battery_model.hpp"

namespace board {

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
    h0::BatteryReading sample(uint64_t now, uint16_t calPermille);

private:
    uint16_t readRawMilliVolts();

    h0::BatteryFilter filter_;
    uint64_t lastSampleUs_ = 0;
    bool primed_ = false;
};

} // namespace board
