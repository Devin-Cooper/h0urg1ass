// h0urg1ass -- M1 bring-up: first light on the glass.
//
// Builds on M0 (power latch, clocks, i2c inventory, battery) and adds the
// ST7789V2 driver. Sequence:
//
//   1. a full-screen 1-pixel checkerboard, held long enough to photograph.
//      This is the only signal-integrity check the panel offers -- it is
//      write-only, so there is no controller ID read and no GRAM read-back.
//      It also makes the rounded-corner radius directly measurable.
//   2. a layout page showing the safe area and the corner clipping.
//   3. push timings, full frame and partial region.
//
// Two rules are load-bearing and are commented at their call sites:
//   1. GPIO15 goes high before anything else, or the board dies on battery.
//   2. clk_peri must be re-pointed at PLL_SYS after set_sys_clock_khz.

#include <cstdio>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/structs/sysinfo.h"
#include "pico/stdlib.h"

#include <1bit/core/framebuffer.hpp>
#include <1bit/fonts/term_6x9.hpp>
#include <1bit/fonts/term_8x12.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>
#include <1bit/render/vector_font.hpp>

#include "board/pins.hpp"
#include "board/st7789_1in69.hpp"

using onebit::BLACK;
using onebit::WHITE;

namespace {

// 125 MHz, deliberately *below* the 150 MHz datasheet maximum -- because it
// makes the display 1.65x faster.
//
// The PL022 divides clk_peri by an integer prescale and postdiv, and clk_peri
// follows clk_sys. The panel's own TSCYCW ceiling is 62.5 MHz, so:
//
//   clk_sys   achievable SPI <= 62.5     full 240x280 frame   measured
//   150 MHz   37.5 MHz  (75 is over spec)      28.7 ms ideal   31.6 ms
//   125 MHz   62.5 MHz  (exactly on spec)      17.2 ms ideal   19.1 ms
//
// From 150 MHz the divider can only produce 75 MHz (out of spec) or 37.5 MHz,
// and there is nothing in between -- so the faster CPU clock costs 12.5 ms of
// every frame. 125 MHz divides to exactly the panel's rated maximum. This app
// is bus-bound, not compute-bound, so the 17% less CPU is free and the 1.65x
// faster panel is not.
//
// 250 MHz would also divide to 62.5 MHz, but that is a 1.67x overclock for no
// display gain. Revisit only if something turns out to be compute-bound.
//
// Note this cannot be called SYS_CLK_KHZ: pico-sdk defines that as a macro in
// hardware/platform_defs.h, and the collision produces a baffling
// "expected unqualified-id before numeric constant".
constexpr uint32_t kSysClockKhz = 125'000;

/// Settling time for the battery-sense node, measured on this board.
///
/// The divider is 200k over 100k, so the ADC sees ~67k against C10/C12 on the
/// tap. Measured against a settled 4.001 V: ~1 ms → 2.751 V, ~6 ms → 3.459 V,
/// ~24 ms → 3.960 V, ~45 ms → 3.997 V. An unsettled read is low by up to 40% --
/// it reports a flat battery on a full one. Reading the battery is therefore
/// not cheap; sample it on a slow schedule and cache it.
constexpr uint32_t kBatterySettleMs = 50;

/// Hold the soft power latch.
///
/// GPIO15 → R3 1k → base of T1 (SS8050); T1's collector pulls the gate of
/// Q3 (AO3401) down, passing the battery through to B+. Until firmware asserts
/// it, the board is alive only because a finger is on the power button. This
/// must be the first thing main() does.
///
/// Invisible over USB, where VBUS reaches VSYS through D4 regardless. A build
/// that never sets this works perfectly on the bench and fails every time in
/// the field.
void latchPower() {
    gpio_init(board::power::SYS_EN);
    gpio_set_dir(board::power::SYS_EN, GPIO_OUT);
    gpio_put(board::power::SYS_EN, 1);
}

/// Re-parent clk_peri to PLL_SYS.
///
/// set_sys_clock_khz() points clk_peri at PLL_USB (48 MHz), capping SPI at
/// 24 MHz -- slower than stock. Skipping this makes the display slower as the
/// CPU gets faster, which is a maddening way to misdiagnose a performance bug.
void repointPeripheralClock() {
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
}

void initBuzzer() {
    gpio_set_function(board::BUZZER, GPIO_FUNC_PWM);
    const unsigned slice = pwm_gpio_to_slice_num(board::BUZZER);
    pwm_set_wrap(slice, 2000);
    pwm_set_clkdiv(slice, 200.0f);
    pwm_set_gpio_level(board::BUZZER, 0);
    pwm_set_enabled(slice, true);
}

void beep(uint32_t ms) {
    pwm_set_gpio_level(board::BUZZER, 1000);
    sleep_ms(ms);
    pwm_set_gpio_level(board::BUZZER, 0);
}

void initI2c() {
    i2c_init(i2c1, board::i2c::BAUD_HZ);
    gpio_set_function(board::i2c::SDA, GPIO_FUNC_I2C);
    gpio_set_function(board::i2c::SCL, GPIO_FUNC_I2C);
    gpio_pull_up(board::i2c::SDA);
    gpio_pull_up(board::i2c::SCL);
}

/// Bring the touch controller out of reset.
///
/// Without this the CST816 never appears on the bus at all -- a scan simply
/// does not see it, which reads as a dead part rather than one held in reset.
void resetTouch() {
    gpio_init(board::touch::RST);
    gpio_set_dir(board::touch::RST, GPIO_OUT);
    gpio_put(board::touch::RST, 0);
    sleep_ms(20);
    gpio_put(board::touch::RST, 1);
    sleep_ms(80);

    // Pull-UP, so errata E9 does not apply -- that bites pull-DOWN inputs on A2.
    gpio_init(board::touch::INT);
    gpio_set_dir(board::touch::INT, GPIO_IN);
    gpio_pull_up(board::touch::INT);
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

int scanI2c() {
    printf("i2c1 scan @ %u Hz\n", board::i2c::BAUD_HZ);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        uint8_t rx;
        if (i2c_read_blocking_until(i2c1, addr, &rx, 1, false,
                                    make_timeout_time_ms(10)) >= 0) {
            printf("  0x%02X  %s\n", addr, i2cDeviceName(addr));
            ++found;
        }
    }
    return found;
}

void initBattery() {
    adc_init();
    adc_gpio_init(board::power::BAT_ADC);
    adc_select_input(board::power::BAT_ADC_CHANNEL);
    sleep_ms(kBatterySettleMs);
}

float readBatteryVolts() {
    adc_select_input(board::power::BAT_ADC_CHANNEL);
    for (int i = 0; i < 8; ++i) (void)adc_read();
    uint32_t acc = 0;
    for (int i = 0; i < 32; ++i) acc += adc_read();
    const float v_adc = (static_cast<float>(acc) / 32.0f) * 3.3f / 4095.0f;
    return v_adc * board::power::BAT_DIVIDER_RATIO;
}

void reportChip() {
    const uint32_t chip_id = sysinfo_hw->chip_id;
    const uint32_t revision = (chip_id >> 28) & 0xF;
    printf("RP2350 chip_id=0x%08lx revision=A%lu\n",
           static_cast<unsigned long>(chip_id), static_cast<unsigned long>(revision));
    if (revision == 2) {
        printf("  A2: errata E9 applies -- never configure a pull-down input\n");
    }
    printf("clk_sys=%lu Hz  clk_peri=%lu Hz%s\n",
           static_cast<unsigned long>(clock_get_hz(clk_sys)),
           static_cast<unsigned long>(clock_get_hz(clk_peri)),
           clock_get_hz(clk_peri) == clock_get_hz(clk_sys) ? "" : "  <-- WRONG");
}

// ---------------------------------------------------------------- drawing --

/// Full-screen 1-pixel checkerboard.
///
/// The worst case for SPI signal integrity and for a buffer race, and the only
/// integrity check this write-only panel allows. What to look for:
///   * an even grey field  -> clean
///   * sparse static that does NOT change when the SPI clock is halved
///                         -> a DMA strip-buffer race, not signal integrity
///   * banding or torn rows -> clock too high for the FPC
/// It also renders the rounded corners unmistakable, so the corner radius can
/// be measured straight off a photograph.
void drawCheckerboard(onebit::IFramebuffer& fb) {
    for (int16_t y = 0; y < fb.height(); ++y) {
        for (int16_t x = 0; x < fb.width(); ++x) {
            fb.setPixel(x, y, ((x ^ y) & 1) ? BLACK : WHITE);
        }
    }
}

/// Layout reference: safe area, corner clipping, and text at three sizes.
void drawLayoutPage(onebit::IFramebuffer& fb, float volts) {
    fb.clear(WHITE);

    const int16_t w = fb.width();
    const int16_t h = fb.height();

    // A 1 px full-perimeter frame. Its corners are the clipping evidence: the
    // straight runs should be visible and the corners should vanish.
    onebit::drawRect(fb, 0, 0, w, h, BLACK);

    // The safe rectangle. A 44 px quarter-circle needs 44 - 44/sqrt(2) ~= 12.9 px
    // of diagonal clearance, so a 16 px inset clears it with margin.
    constexpr int16_t kInset = 16;
    onebit::drawRect(fb, kInset, kInset, w - 2 * kInset, h - 2 * kInset, BLACK);

    // Big vector digits -- the countdown is the most important element in this
    // UI, so prove the largest text works before anything else is designed.
    onebit::renderString(fb, "12:00", 26, 40, 34, 52, 6, 4, BLACK);

    onebit::drawLine(fb, kInset, 108, w - kInset, 108, BLACK);

    onebit::drawBitmapText(fb, onebit::fonts::TERM_8X12, kInset + 4, 120,
                           "h0urg1ass M1", BLACK);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9, kInset + 4, 138,
                           "ST7789V2 240x280", BLACK);

    char line[40];
    std::snprintf(line, sizeof(line), "battery %.2f V", static_cast<double>(volts));
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9, kInset + 4, 152, line, BLACK);

    // Solid and outline circles: fill correctness and midpoint symmetry.
    onebit::fillCircle(fb, 78, 205, 30, BLACK);
    onebit::drawCircle(fb, 162, 205, 30, BLACK);

    // A grey wedge by dither, to show tone without a grey level.
    for (int16_t y = 240; y < 262; ++y) {
        for (int16_t x = kInset; x < w - kInset; ++x) {
            if (((x + y) & 3) == 0) fb.setPixel(x, y, BLACK);
        }
    }
}

} // namespace

int main() {
    latchPower(); // Rule 1. Before stdio, before clocks, before anything.

    set_sys_clock_khz(kSysClockKhz, true);
    repointPeripheralClock(); // Rule 2, and it must follow the line above.

    stdio_init_all();

    // Idle before touching hardware. If the app hangs past this point USB is
    // already up, so picotool can still force a reboot into BOOTSEL and
    // reflashing never needs the BOOT button.
    sleep_ms(3000);

    printf("\n=== h0urg1ass M1 -- first light ===\n");
    reportChip();

    initBuzzer();
    initI2c();
    resetTouch();
    initBattery();

    const int devices = scanI2c();
    printf("%d i2c device(s); battery %.2f V\n", devices,
           static_cast<double>(readBatteryVolts()));

    // ------------------------------------------------------------ display --
    static board::St7789_1in69 lcd;
    if (!lcd.init()) {
        printf("FATAL: display init failed (strip alloc)\n");
        beep(600);
        while (true) tight_loop_contents();
    }
    printf("SPI requested 62.50 MHz, achieved %.2f MHz\n",
           lcd.actualBaud() / 1e6);
    printf("panel %dx%d, strip %u B x2\n", lcd.width(), lcd.height(),
           static_cast<unsigned>(lcd.stripBytes()));

    static onebit::Framebuffer<board::lcd::WIDTH, board::lcd::HEIGHT> fb;
    if (!fb.isValid()) {
        printf("FATAL: framebuffer alloc failed\n");
        beep(600);
        while (true) tight_loop_contents();
    }
    printf("framebuffer %u B\n", static_cast<unsigned>(fb.bufferSize()));

    lcd.clear(WHITE);
    lcd.setBacklight(200);

    // 1 -- the canary.
    printf("\ncheckerboard canary (8 s) -- look for an even grey field\n");
    drawCheckerboard(fb);
    absolute_time_t t0 = get_absolute_time();
    lcd.push(fb);
    lcd.waitIdle();
    const int64_t checkerUs = absolute_time_diff_us(t0, get_absolute_time());
    printf("  full frame %lld us\n", static_cast<long long>(checkerUs));
    sleep_ms(8000);

    // 2 -- the layout page.
    drawLayoutPage(fb, readBatteryVolts());
    t0 = get_absolute_time();
    lcd.push(fb);
    lcd.waitIdle();
    const int64_t fullUs = absolute_time_diff_us(t0, get_absolute_time());

    // 3 -- a partial region, the path that actually matters for a timer.
    const onebit::Rect region{16, 120, 208, 24};
    t0 = get_absolute_time();
    lcd.beginFrame();
    lcd.writeRegion(fb, region);
    lcd.endFrame();
    lcd.waitIdle();
    const int64_t partUs = absolute_time_diff_us(t0, get_absolute_time());

    const double bits = 240.0 * 280.0 * 16.0;
    printf("\ntiming\n");
    printf("  full frame   %6lld us   (%.1f fps, %.0f%% of wire limit)\n",
           static_cast<long long>(fullUs), 1e6 / static_cast<double>(fullUs),
           100.0 * (bits / lcd.actualBaud()) / (static_cast<double>(fullUs) / 1e6));
    printf("  208x24 strip %6lld us   (%.1fx cheaper)\n",
           static_cast<long long>(partUs),
           static_cast<double>(fullUs) / static_cast<double>(partUs));

    beep(60); sleep_ms(80); beep(60);
    printf("\nalive -- heartbeat every 10 s\n");

    uint32_t tick = 0;
    while (true) {
        sleep_ms(10000);
        printf("[%lu] battery %.2f V\n", static_cast<unsigned long>(++tick),
               static_cast<double>(readBatteryVolts()));
    }
}
