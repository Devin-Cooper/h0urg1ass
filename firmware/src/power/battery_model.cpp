#include "power/battery_model.hpp"

namespace h0 {

Bucket bucketFor(uint16_t mv) {
    // Thresholds from power-and-time.md section 7.5's table. Deliberately
    // coarse: 3.74-3.87 V spans 20%-60% SoC, so anything finer is invention.
    if (mv >= 4060) return Bucket::Full;
    if (mv >= 3870) return Bucket::Good;
    if (mv >= 3770) return Bucket::Half; // 7.5 puts 3.77 V at Half, 3.74 V at Low
    if (mv >= 3550) return Bucket::Low;
    return Bucket::Critical;
}

const char* bucketName(Bucket b) {
    switch (b) {
        case Bucket::Full:     return "FULL";
        case Bucket::Good:     return "GOOD";
        case Bucket::Half:     return "HALF";
        case Bucket::Low:      return "LOW";
        case Bucket::Critical: return "CRIT";
    }
    return "?";
}

uint16_t applyCal(uint16_t rawMilliVolts, uint16_t permille) {
    return static_cast<uint16_t>((static_cast<uint32_t>(rawMilliVolts) * permille) / 1000u);
}

void BatteryFilter::push(uint16_t mv) {
    if (!valid_) {
        // Seed from the first real reading. Starting at zero makes the row read
        // 0.00 V and climb for ~45 s after every boot, which looks exactly like
        // a flat battery and is the most alarming possible lie.
        accum_ = static_cast<uint32_t>(mv) << 4;
        value_ = mv;
        valid_ = true;
        return;
    }
    accum_ = accum_ - (accum_ >> 4) + mv;
    value_ = static_cast<uint16_t>(accum_ >> 4);
}

} // namespace h0
