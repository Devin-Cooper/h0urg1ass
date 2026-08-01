#include "board/battery.hpp"

#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "board/pins.hpp"
#include "power/battery_floor.hpp"

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

BatteryUpdate Battery::sample(uint64_t now, const h0::Settings& s) {
    BatteryUpdate up;

    const bool fresh = (!primed_ || now - lastSampleUs_ >= kSamplePeriodUs);
    if (fresh) {
        // Filter the RAW reading and apply the gain at the end. Filtering the
        // corrected value would make every CAL wheel step drag a 16-second tail
        // behind it, so the number would never settle while you were adjusting
        // the very thing that moves it.
        filter_.push(readRawMilliVolts());
        lastSampleUs_ = now;
        primed_ = true;
    }

    up.reading.valid = filter_.valid();
    up.reading.rawMilliVolts = filter_.milliVolts();
    up.reading.milliVolts = h0::applyCal(up.reading.rawMilliVolts, s.batCalPermille);
    // "Something measured has landed", not "the gain differs from 1000".
    //
    // The bare `batCalPermille != 1000` had no way out for a board whose true
    // gain IS unity: AutoCal anchors at 1000, shouldStore's deadband refuses
    // the redundant write, the field never moves, and the BATTERY row says
    // UNCAL for the life of the device.
    //
    //   batCalPermille != 1000  auto anchored, or the wheel was moved.
    //   batCalAuto == 0         the wheel was hand-set, so the gain is a
    //                           decision rather than a default -- 1000
    //                           included, which is the case above.
    //   batFloorRawMv != 0      a liveness backstop, not evidence about the
    //                           gain: a device that has been all the way down
    //                           its own discharge curve has had a whole cycle
    //                           for the charger to anchor, and a word that can
    //                           never change after that is not a warning.
    up.reading.calibrated = (s.batCalPermille != 1000) || (s.batCalAuto == 0) ||
                            (s.batFloorRawMv != 0);

    // Both learners are fed once per SAMPLE, never once per frame: AutoCal's
    // plateau window counts samples and assumes 1 Hz, so feeding it per frame
    // would qualify a plateau roughly thirty times too early.
    if (fresh && up.reading.valid) {
        const uint16_t gain = autoCal_.push(up.reading.rawMilliVolts);
        if (gain != 0 && h0::AutoCal::shouldStore(gain, s.batCalPermille, s.batCalAuto)) {
            up.newCalPermille = gain;
        }
        up.newFloorRawMv = h0::BatteryFloor::update(
            up.reading.rawMilliVolts, up.reading.milliVolts, s.batFloorRawMv);
    }
    up.reading.charging = autoCal_.charging();

    return up;
}

} // namespace board
