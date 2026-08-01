#pragma once

#include <cstdint>

namespace h0 {

/// Five buckets, per power-and-time.md section 7.5. Never a percentage: the
/// curve is flat exactly where it matters, and a number implies a precision
/// this hardware does not have.
enum class Bucket : uint8_t { Critical = 0, Low = 1, Half = 2, Good = 3, Full = 4 };

Bucket bucketFor(uint16_t milliVolts);
const char* bucketName(Bucket b);

/// Single-point gain correction. The dominant error is the resistor ratio
/// (+/-5% on both R11 and R12, giving -6.3%/+7.0%), which is a pure gain term,
/// so one multiplier fixes it.
uint16_t applyCal(uint16_t rawMilliVolts, uint16_t permille);

/// IIR at alpha = 1/16 across one-per-second readings: tau is about 16 s, so
/// the display moves like the cell does rather than like the load.
class BatteryFilter {
public:
    void push(uint16_t milliVolts);
    uint16_t milliVolts() const { return value_; }
    bool valid() const { return valid_; }

private:
    uint32_t accum_ = 0; ///< value << 4, so the fraction is not thrown away
    uint16_t value_ = 0;
    bool valid_ = false;
};

struct BatteryReading {
    /// Filtered but UNCORRECTED. The CAL row needs this so it can re-apply a
    /// probe gain per wheel entry and show what each one would read.
    uint16_t rawMilliVolts = 0;
    uint16_t milliVolts = 0; ///< raw with the stored gain applied
    /// Set from AutoCal::charging(), not from a voltage threshold.
    ///
    /// The threshold this replaces was 4220 mV, which sits ABOVE the ETA6096's
    /// own 4210 mV CV setpoint (datasheet: 4.17 min / 4.21 typ / 4.25 max), so
    /// on a typical part it could never fire once the gauge was calibrated. It
    /// only ever appeared to work because the uncalibrated gain read high.
    /// Lowering it does not help either: a full resting cell and a cell held at
    /// CV differ by tens of millivolts. A rise is the only unambiguous signal.
    bool charging = false;
    bool calibrated = false; ///< batCalPermille != 1000
    bool valid = false;
};

} // namespace h0
