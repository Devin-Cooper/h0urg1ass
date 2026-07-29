// h0urg1ass -- M0 bring-up.
//
// Proves, in one boot: the battery power latch holds, USB CDC enumerates so the
// board can be reflashed without touching BOOT, clk_peri is parented correctly,
// all three i2c1 devices answer, the backlight dims, the buzzer sounds, and the
// battery divider reads.
//
// Two rules are load-bearing here and are commented at their call sites:
//   1. GPIO15 goes high before anything else, or the board dies on battery.
//   2. clk_peri must be re-pointed at PLL_SYS after set_sys_clock_khz.

#include <cstdio>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/structs/sysinfo.h"
#include "pico/stdlib.h"

#include "board/pins.hpp"

namespace {

// The vendor demos run this part at 200 MHz, above the 150 MHz datasheet
// maximum. 150 MHz is the specified ceiling and is plenty: it yields 37.5 MHz
// on SPI1, which pushes a full 240x280 frame in about 29 ms.
//
// Note this cannot be called SYS_CLK_KHZ -- pico-sdk defines that as a macro in
// hardware/platform_defs.h, and the collision produces a baffling
// "expected unqualified-id before numeric constant".
constexpr uint32_t kSysClockKhz = 150'000;

/// Hold the soft power latch.
///
/// On battery the RT9193 rail is gated by a network driven from GPIO15. Until
/// firmware asserts it, the board is alive only because a finger is holding the
/// power button. This must be the first thing main() does -- every millisecond
/// before it is a millisecond the board can die in.
///
/// Note this is invisible over USB, where VBUS keeps the rail up regardless.
/// A build that never sets this works perfectly on the bench and fails 100% of
/// the time in the field.
void latchPower() {
    gpio_init(board::power::SYS_EN);
    gpio_set_dir(board::power::SYS_EN, GPIO_OUT);
    gpio_put(board::power::SYS_EN, 1);
}

/// Re-parent clk_peri to PLL_SYS.
///
/// set_sys_clock_khz() points clk_peri at PLL_USB (48 MHz), which caps SPI at
/// 24 MHz -- slower than stock. Skipping this makes the display slower as the
/// CPU gets faster, which is a maddening way to misdiagnose a performance
/// problem. The vendor's own demo does this too.
void repointPeripheralClock() {
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    clock_get_hz(clk_sys),
                    clock_get_hz(clk_sys));
}

void initBuzzer() {
    gpio_set_function(board::BUZZER, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(board::BUZZER);
    pwm_set_wrap(slice, 2000);
    pwm_set_clkdiv(slice, 200.0f);
    pwm_set_gpio_level(board::BUZZER, 0); // silent until asked
    pwm_set_enabled(slice, true);
}

void beep(uint32_t ms) {
    pwm_set_gpio_level(board::BUZZER, 1000); // 50% duty
    sleep_ms(ms);
    pwm_set_gpio_level(board::BUZZER, 0);
}

void initBacklight() {
    gpio_set_function(board::lcd::BACKLIGHT, GPIO_FUNC_PWM);
    const uint slice = pwm_gpio_to_slice_num(board::lcd::BACKLIGHT);
    pwm_set_wrap(slice, 100); // 0-100 maps directly to percent
    pwm_set_gpio_level(board::lcd::BACKLIGHT, 0);
    pwm_set_enabled(slice, true);
}

void setBacklight(uint8_t percent) {
    if (percent > 100) percent = 100;
    pwm_set_gpio_level(board::lcd::BACKLIGHT, percent);
}

void initI2c() {
    i2c_init(i2c1, board::i2c::BAUD_HZ);
    gpio_set_function(board::i2c::SDA, GPIO_FUNC_I2C);
    gpio_set_function(board::i2c::SCL, GPIO_FUNC_I2C);
    // Internal pull-ups; every vendor driver enables these.
    gpio_pull_up(board::i2c::SDA);
    gpio_pull_up(board::i2c::SCL);
}

const char* i2cDeviceName(uint8_t addr) {
    switch (addr) {
        case board::i2c::ADDR_TOUCH:   return "CST816 touch";
        case board::i2c::ADDR_RTC:     return "PCF85063A RTC";
        case board::i2c::ADDR_IMU:     return "QMI8658C IMU (SA0 low)";
        case board::i2c::ADDR_IMU_ALT: return "QMI8658C IMU (SA0 high)";
        default:                       return "unknown";
    }
}

/// Bring the touch controller out of reset.
///
/// TP_RST is active low and push-pull. Without this the CST816 never appears on
/// the bus at all -- a scan simply does not see it, which reads as a dead part
/// rather than one held in reset.
void resetTouch() {
    gpio_init(board::touch::RST);
    gpio_set_dir(board::touch::RST, GPIO_OUT);
    gpio_put(board::touch::RST, 0);
    sleep_ms(20);
    gpio_put(board::touch::RST, 1);
    sleep_ms(80); // the part needs time before it will ACK

    // INT is active-low with an internal pull-up. Note this is a pull-UP, so
    // errata E9 does not apply here -- it bites pull-DOWN inputs on A2.
    gpio_init(board::touch::INT);
    gpio_set_dir(board::touch::INT, GPIO_IN);
    gpio_pull_up(board::touch::INT);
}

/// Read one 8-bit register. Returns false if the device does not respond.
bool readReg(uint8_t addr, uint8_t reg, uint8_t& out) {
    if (i2c_write_blocking_until(i2c1, addr, &reg, 1, true,
                                 make_timeout_time_ms(10)) < 0) {
        return false;
    }
    return i2c_read_blocking_until(i2c1, addr, &out, 1, false,
                                   make_timeout_time_ms(10)) >= 0;
}

/// Confirm each part by its identity register, not merely by an address ACK.
void identifyDevices() {
    uint8_t v = 0;

    // CST816 family: ChipID at 0xA7 reads 0xB5 on every variant.
    if (readReg(board::i2c::ADDR_TOUCH, 0xA7, v)) {
        printf("  touch ChipID(0xA7) = 0x%02X %s\n", v,
               v == 0xB5 ? "(expected 0xB5)" : "(UNEXPECTED)");
    } else {
        printf("  touch did not respond to a register read\n");
    }

    // QMI8658C: WHO_AM_I at 0x00 reads 0x05. Try both addresses -- the
    // schematic reading and the hardware have disagreed here.
    const uint8_t imu_addrs[] = {board::i2c::ADDR_IMU, board::i2c::ADDR_IMU_ALT};
    for (uint8_t a : imu_addrs) {
        if (readReg(a, 0x00, v)) {
            printf("  IMU @0x%02X WHO_AM_I = 0x%02X %s\n", a, v,
                   v == 0x05 ? "(expected 0x05)" : "(UNEXPECTED)");
            uint8_t rev = 0;
            if (readReg(a, 0x01, rev)) printf("  IMU revision = 0x%02X\n", rev);
        }
    }
}

/// Scan i2c1 and report. All three expected devices answering is the strongest
/// single signal that the board is healthy.
int scanI2c() {
    printf("i2c1 scan @ %u Hz (SDA=%u SCL=%u)\n",
           board::i2c::BAUD_HZ, board::i2c::SDA, board::i2c::SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        uint8_t rx;
        const int r = i2c_read_blocking_until(
            i2c1, addr, &rx, 1, false, make_timeout_time_ms(10));
        if (r >= 0) {
            printf("  0x%02X  %s\n", addr, i2cDeviceName(addr));
            ++found;
        }
    }
    printf("  %d device(s)\n", found);
    return found;
}

/// Settling time for the battery-sense node, measured on this board.
///
/// The divider is 200k over 100k, so the ADC sees a ~67k source impedance
/// against the capacitance on that node. The result is a genuinely slow RC,
/// not mere ADC noise. Measured curve against a steady-state 4.001 V:
///
///     ~1 ms   2.751 V      ~24 ms   3.960 V
///     ~3 ms   3.122 V      ~45 ms   3.997 V
///     ~6 ms   3.459 V      ~96 ms   4.001 V
///     ~12 ms  3.792 V
///
/// An unsettled read is wrong by up to 40% -- it reports a flat battery on a
/// full one. 20 ms is within 0.1%; 50 ms is fully settled with margin.
///
/// Consequence for the design: reading the battery is not cheap. Sample it on
/// a slow schedule and cache the result; never read it inside a frame loop.
constexpr uint32_t kBatterySettleMs = 50;

void initBattery() {
    adc_init();
    adc_gpio_init(board::power::BAT_ADC);
    adc_select_input(board::power::BAT_ADC_CHANNEL);
    sleep_ms(kBatterySettleMs);
}

float readBatteryVolts() {
    adc_select_input(board::power::BAT_ADC_CHANNEL);
    for (int i = 0; i < 8; ++i) (void)adc_read(); // sample-and-hold
    uint32_t acc = 0;
    for (int i = 0; i < 32; ++i) acc += adc_read();
    const float counts = static_cast<float>(acc) / 32.0f;
    const float v_adc = counts * 3.3f / 4095.0f;
    return v_adc * board::power::BAT_DIVIDER_RATIO;
}

void reportChip() {
    const uint32_t chip_id = sysinfo_hw->chip_id;
    const uint32_t revision = (chip_id >> 28) & 0xF;
    printf("RP2350 chip_id=0x%08lx revision=A%lu\n",
           static_cast<unsigned long>(chip_id),
           static_cast<unsigned long>(revision));
    if (revision == 2) {
        // RP2350-E9: leakage on a Bank 0 pad whose input buffer is enabled while
        // the output buffer is disabled can hold the pad near 2.2 V and overpower
        // an internal pull-down. As this board is wired nothing is affected --
        // but it is a property of the configuration, not the silicon.
        printf("  A2: errata E9 applies -- never configure a pull-down input\n");
    }
    printf("clk_sys  = %lu Hz\n", static_cast<unsigned long>(clock_get_hz(clk_sys)));
    printf("clk_peri = %lu Hz", static_cast<unsigned long>(clock_get_hz(clk_peri)));
    if (clock_get_hz(clk_peri) != clock_get_hz(clk_sys)) {
        printf("  <-- WRONG, should equal clk_sys");
    }
    printf("\n  max SPI = %lu Hz\n",
           static_cast<unsigned long>(clock_get_hz(clk_peri) / 2));
}

} // namespace

int main() {
    // Rule 1. Before stdio, before clocks, before anything.
    latchPower();

    set_sys_clock_khz(kSysClockKhz, true);
    repointPeripheralClock(); // Rule 2, and it must follow the line above.

    stdio_init_all();

    // Idle before touching hardware. If the app hangs past this point USB is
    // already up, so the 1200-baud reboot still works and reflashing never
    // needs the BOOT button. Losing this costs a physical button press on every
    // bad build.
    sleep_ms(3000);

    printf("\n=== h0urg1ass M0 bring-up ===\n");
    reportChip();

    initBuzzer();
    initBacklight();
    initI2c();
    resetTouch(); // must precede the scan, or the CST816 never appears
    initBattery();

    const int devices = scanI2c();
    identifyDevices();
    printf("battery = %.2f V (settled %lu ms)\n",
           static_cast<double>(readBatteryVolts()),
           static_cast<unsigned long>(kBatterySettleMs));

    // Two short beeps for a clean scan, one long for a problem. Audible
    // pass/fail matters because there is nothing on the glass yet.
    if (devices >= 3) {
        beep(60); sleep_ms(80); beep(60);
    } else {
        beep(400);
    }

    printf("\nbacklight ramp\n");
    for (int p = 0; p <= 100; p += 5) { setBacklight(p); sleep_ms(20); }
    for (int p = 100; p >= 20; p -= 5) { setBacklight(p); sleep_ms(20); }

    printf("alive -- heartbeat every 5 s\n");
    uint32_t tick = 0;
    while (true) {
        sleep_ms(5000);
        printf("[%lu] battery %.2f V\n",
               static_cast<unsigned long>(++tick),
               static_cast<double>(readBatteryVolts()));
    }
}
