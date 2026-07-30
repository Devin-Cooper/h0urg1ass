// h0urg1ass -- interactive build.
//
// The timer, driven by how you hold it:
//
//   stand it up       start, or resume
//   lay it flat       pause; this is also the setting posture
//   turn it over      reset to full and run
//   face down         silence a ringing alarm
//
// A debug strip along the bottom reports the raw IMU vector and the classified
// posture. It is there because the IMU's axes are not the panel's -- the part is
// soldered in whatever orientation suited the layout -- and the mapping can only
// be established by holding the board in known postures and reading it off.
// Once `axisMap` is right, the strip goes.
//
// Two rules are load-bearing and are commented at their call sites:
//   1. GPIO15 goes high before anything else, or the board dies on battery.
//   2. clk_peri must be re-pointed at PLL_SYS after set_sys_clock_khz.

#include <cstdio>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/structs/sysinfo.h"
#include "pico/stdlib.h"

#include <1bit/core/framebuffer.hpp>
#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/dirty_rect.hpp>
#include <1bit/render/primitives.hpp>

#include "app/app.hpp"
#include "board/buzzer.hpp"
#include "board/pins.hpp"
#include "board/qmi8658.hpp"
#include "board/st7789_1in69.hpp"
#include "faces/digits_face.hpp"
#include "faces/hourglass_face.hpp"
#include "faces/splitflap_face.hpp"
#include "input/orientation.hpp"
#include "timer/timer_model.hpp"

using onebit::BLACK;
using onebit::WHITE;

namespace {

// 125 MHz, deliberately below the 150 MHz maximum: the PL022's integer divider
// reaches only 75 MHz (over the panel's 62.5 MHz spec) or 37.5 MHz from a
// 150 MHz clk_peri, so the faster CPU clock costs 12.5 ms of every frame.
// 125 divides to exactly 62.5. Cannot be named SYS_CLK_KHZ -- pico-sdk defines
// that as a macro and the collision is baffling.
constexpr uint32_t kSysClockKhz = 125'000;

/// Battery-sense settling, measured: the 200k/100k divider against C10/C12 on
/// the tap gives a real RC, and an unsettled read is low by up to 40%.
constexpr uint32_t kBatterySettleMs = 50;

/// The duration the device starts with, until touch lands.
constexpr uint64_t kDefaultDurationUs = 2ull * 60ull * 1'000'000ull;

/// Hold the soft power latch. GPIO15 -> R3 1k -> T1 base; T1's collector pulls
/// Q3's gate down, passing the battery through. Until this runs, the board is
/// alive only because a finger is on the button. Invisible over USB, where VBUS
/// reaches VSYS through D4 regardless.
void latchPower() {
    gpio_init(board::power::SYS_EN);
    gpio_set_dir(board::power::SYS_EN, GPIO_OUT);
    gpio_put(board::power::SYS_EN, 1);
}

/// set_sys_clock_khz() re-parents clk_peri to PLL_USB at 48 MHz, capping SPI at
/// 24 MHz -- slower than stock, while the CPU gets faster.
void repointPeripheralClock() {
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));
}

void initI2c() {
    i2c_init(i2c1, board::i2c::BAUD_HZ);
    gpio_set_function(board::i2c::SDA, GPIO_FUNC_I2C);
    gpio_set_function(board::i2c::SCL, GPIO_FUNC_I2C);
    gpio_pull_up(board::i2c::SDA);
    gpio_pull_up(board::i2c::SCL);
}

/// The CST816 does not ACK its own address until RST is pulsed -- held in reset
/// it looks like a dead part on a bus scan rather than a held one.
void resetTouch() {
    gpio_init(board::touch::RST);
    gpio_set_dir(board::touch::RST, GPIO_OUT);
    gpio_put(board::touch::RST, 0);
    sleep_ms(20);
    gpio_put(board::touch::RST, 1);
    sleep_ms(80);
    gpio_init(board::touch::INT);
    gpio_set_dir(board::touch::INT, GPIO_IN);
    gpio_pull_up(board::touch::INT); // pull-UP, so errata E9 does not apply
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
    return (static_cast<float>(acc) / 32.0f) * 3.3f / 4095.0f *
           board::power::BAT_DIVIDER_RATIO;
}

const char* orientationName(h0::Orientation o) {
    switch (o) {
        case h0::Orientation::UprightA: return "UPRIGHT";
        case h0::Orientation::UprightB: return "INVERTED";
        case h0::Orientation::FlatBack: return "FLAT";
        case h0::Orientation::FaceDown: return "FACEDOWN";
        case h0::Orientation::Edge:     return "EDGE";
        case h0::Orientation::Unknown:  return "?";
    }
    return "?";
}

const char* feedbackName(h0::Feedback f) {
    switch (f) {
        case h0::Feedback::Started:  return "START";
        case h0::Feedback::Paused:   return "PAUSE";
        case h0::Feedback::Resumed:  return "RESUME";
        case h0::Feedback::Reset:    return "RESET";
        case h0::Feedback::Rejected: return "REJECT";
        case h0::Feedback::AlarmOn:  return "ALARM";
        case h0::Feedback::AlarmOff: return "HUSH";
        case h0::Feedback::None:     return "";
    }
    return "";
}

/// Bottom strip: raw IMU vector, mapped posture, last event.
///
/// The raw numbers are the point. Holding the board in each known posture and
/// reading which component goes to +-1 is the only way to establish the axis
/// mapping, because it depends on how the part is rotated on the PCB.
void drawDebug(onebit::IFramebuffer& fb, const h0::Vec3& raw, h0::Orientation o,
               const char* lastEvent) {
    constexpr int16_t y0 = 236;
    onebit::fillRect(fb, 0, y0, 240, 44, WHITE);
    onebit::drawLine(fb, 20, y0, 220, y0, BLACK);

    char line[40];
    std::snprintf(line, sizeof(line), "%-8s %s", orientationName(o), lastEvent);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9, 26, y0 + 6, line, BLACK);

    std::snprintf(line, sizeof(line), "%+.2f %+.2f %+.2f", static_cast<double>(raw.x),
                  static_cast<double>(raw.y), static_cast<double>(raw.z));
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9, 26, y0 + 18, line, BLACK);
}

} // namespace

int main() {
    latchPower(); // Rule 1. Before stdio, before clocks, before anything.

    set_sys_clock_khz(kSysClockKhz, true);
    repointPeripheralClock(); // Rule 2, and it must follow the line above.

    stdio_init_all();

    // Idle before touching hardware. If the app hangs past here USB is already
    // up, so picotool can still force BOOTSEL and reflashing needs no button.
    sleep_ms(3000);

    printf("\n=== h0urg1ass -- interactive ===\n");
    printf("clk_sys=%lu clk_peri=%lu\n", static_cast<unsigned long>(clock_get_hz(clk_sys)),
           static_cast<unsigned long>(clock_get_hz(clk_peri)));

    initI2c();
    resetTouch();
    initBattery();

    static board::Buzzer buzzer;
    buzzer.begin();

    static board::Qmi8658 imu;
    const bool imuOk = imu.begin();
    printf("IMU %s", imuOk ? "ok" : "NOT FOUND");
    if (imuOk) printf(" @0x%02X rev 0x%02X", imu.address(), imu.revision());
    printf("\nbattery %.2f V\n", static_cast<double>(readBatteryVolts()));

    static board::St7789_1in69 lcd;
    if (!lcd.init()) {
        printf("FATAL: display init failed\n");
        while (true) tight_loop_contents();
    }
    printf("SPI %.2f MHz\n", lcd.actualBaud() / 1e6);

    static onebit::Framebuffer<board::lcd::WIDTH, board::lcd::HEIGHT> fb;
    static onebit::DirtyRectTracker tracker(board::lcd::WIDTH, board::lcd::HEIGHT);
    lcd.clear(WHITE);
    lcd.setBacklight(200);

    static h0::DigitsFace digits;
    static h0::HourglassFace hourglass;
    static h0::SplitFlapFace splitflap;

    static h0::App app;
    static h0::OrientationTracker orient;
    static h0::GravityFilter filter;

    // Seed a duration. The dial lands with touch; until then the device starts
    // with something usable so the gestures can be exercised.
    app.onMotion(h0::MotionEvent::Settled, time_us_64());
    app.setDuration(kDefaultDurationUs, time_us_64());

    printf("\nready -- stand it up to start, lay it flat to pause,\n");
    printf("turn it over to reset, face down to silence\n\n");

    const char* lastEvent = "";
    uint32_t frames = 0;

    while (true) {
        const uint64_t now = time_us_64();

        h0::Vec3 raw{};
        if (imuOk && imu.readRaw(raw)) {
            const h0::Vec3 g = board::axisMap(raw);
            const h0::MotionEvent ev = orient.update(filter.push(g), now);
            if (ev != h0::MotionEvent::None) {
                const h0::Feedback fbk = app.onMotion(ev, now);
                if (fbk != h0::Feedback::None) {
                    buzzer.play(fbk);
                    lastEvent = feedbackName(fbk);
                    printf("[%s] -> %s  %lus left\n", orientationName(orient.current()),
                           lastEvent,
                           static_cast<unsigned long>(app.timer().remainingSeconds(now)));
                }
            }
        }

        const h0::Feedback tickFb = app.tick(now);
        if (tickFb != h0::Feedback::None) {
            buzzer.play(tickFb);
            lastEvent = feedbackName(tickFb);
            printf("-> %s\n", lastEvent);
        }
        buzzer.update(now);

        h0::IFace* face = &digits;
        switch (app.face()) {
            case h0::FaceId::Hourglass: face = &hourglass; break;
            case h0::FaceId::SplitFlap: face = &splitflap; break;
            case h0::FaceId::Digits:    face = &digits; break;
        }
        face->render(fb, app.timer(), now);
        drawDebug(fb, raw, orient.current(), lastEvent);

        lcd.pushDirty(fb, tracker.update(fb));
        lcd.waitIdle();

        if (++frames % 200 == 0) {
            printf("  %s  raw %+.2f %+.2f %+.2f  %lus\n", orientationName(orient.current()),
                   static_cast<double>(raw.x), static_cast<double>(raw.y),
                   static_cast<double>(raw.z),
                   static_cast<unsigned long>(app.timer().remainingSeconds(now)));
        }

        sleep_ms(25); // ~40 Hz, comfortably above the 125 Hz IMU rate we need
    }
}
