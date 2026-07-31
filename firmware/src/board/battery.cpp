#include "board/battery.hpp"

#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "board/pins.hpp"

namespace board {

namespace {

/// The divider's RC against C10/C12 is real: an unsettled first read is low by
/// up to 40%.
constexpr uint32_t kSettleMs = 50;
constexpr uint64_t kSamplePeriodUs = 1'000'000ull;

} // namespace

void Battery::begin() {
    adc_init();
    adc_gpio_init(board::power::BAT_ADC); // also disables the pad's input buffer
    adc_select_input(board::power::BAT_ADC_CHANNEL);
    sleep_ms(kSettleMs);
}

uint16_t Battery::readRawMilliVolts() {
    adc_select_input(board::power::BAT_ADC_CHANNEL);
    for (int i = 0; i < 8; ++i) (void)adc_read(); // discard after the channel select
    uint32_t acc = 0;
    for (int i = 0; i < 32; ++i) acc += adc_read();
    // 32 samples is ~64 us, spanning ~8 backlight PWM periods at 122 kHz, which
    // already kills the rail ripple without any synchronisation.
    const uint32_t mean = acc / 32u;
    // raw * 3300 * 3 / 4096, in millivolts at the battery.
    return static_cast<uint16_t>((mean * 3300u * 3u) / 4096u);
}

h0::BatteryReading Battery::sample(uint64_t now, uint16_t calPermille) {
    if (!primed_ || now - lastSampleUs_ >= kSamplePeriodUs) {
        // Filter the RAW reading and apply the gain at the end. Filtering the
        // corrected value would make every CAL wheel step drag a 16-second tail
        // behind it, so the number would never settle while you were adjusting
        // the very thing that moves it.
        filter_.push(readRawMilliVolts());
        lastSampleUs_ = now;
        primed_ = true;
    }

    h0::BatteryReading r;
    r.valid = filter_.valid();
    r.rawMilliVolts = filter_.milliVolts();
    r.milliVolts = h0::applyCal(r.rawMilliVolts, calPermille);
    r.charging = h0::isCharging(r.milliVolts);
    r.calibrated = (calPermille != 1000);
    return r;
}

} // namespace board
