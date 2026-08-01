#include "power/battery_floor.hpp"

#include "power/battery_model.hpp" // applyCal

namespace h0 {

uint16_t BatteryFloor::update(uint16_t rawMv, uint16_t correctedMv, uint16_t storedRaw) {
    if (rawMv == 0) return 0;
    if (correctedMv >= kTrackBelowMv) return 0;

    // Against the STORED floor, never a per-session minimum. That is the whole
    // write budget: a per-session comparison would re-learn the same floor on
    // every discharge and write every time.
    if (storedRaw != 0 && static_cast<uint32_t>(rawMv) + kStepMv > storedRaw) return 0;

    return rawMv;
}

uint16_t BatteryFloor::cutoffMv(uint16_t storedRaw, uint16_t permille) {
    if (storedRaw == 0) return 0;

    uint32_t mv = static_cast<uint32_t>(applyCal(storedRaw, permille)) + kMarginMv;
    if (mv < kCutoffMinMv) mv = kCutoffMinMv;
    if (mv > kCutoffMaxMv) mv = kCutoffMaxMv;
    return static_cast<uint16_t>(mv);
}

} // namespace h0
