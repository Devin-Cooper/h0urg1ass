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

#include <cmath>
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
#include "board/cst816.hpp"
#include "board/pins.hpp"
#include "board/qmi8658.hpp"
#include "board/st7789_1in69.hpp"
#include "input/drag_column.hpp"
#include "faces/digits_face.hpp"
#include "faces/hourglass_face.hpp"
#include "faces/setting_face.hpp"
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

/// The duration the device starts with.
constexpr uint64_t kDefaultDurationUs = 2ull * 60ull * 1'000'000ull;

/// Backlight levels and the idle timeout.
///
/// The backlight is roughly 70% of active draw -- about 40 mA at full against
/// ~16 mA for everything else -- so how long it stays bright is the single
/// biggest lever on battery life, worth more than every other optimisation
/// combined. Dimming rather than blanking keeps the timer glanceable.
constexpr uint8_t kBacklightActive = 200;
constexpr uint8_t kBacklightIdle = 36;
constexpr uint64_t kIdleAfterUs = 20ull * 1'000'000ull;

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

/// Unwedge a stuck i2c bus.
///
/// i2c is open-drain and multi-drop, so a slave that was interrupted mid-byte
/// can sit holding SDA low forever. Every device on the bus then fails, which
/// is the tell: a fault in one part shows up as one part misbehaving, whereas a
/// wedged bus takes the IMU and the touch controller down together.
///
/// The recovery is the standard one -- take the pins back as GPIO and clock SCL
/// until the slave releases SDA, then issue a STOP and re-init the peripheral.
void recoverI2c() {
    gpio_set_function(board::i2c::SCL, GPIO_FUNC_SIO);
    gpio_set_function(board::i2c::SDA, GPIO_FUNC_SIO);
    gpio_set_dir(board::i2c::SCL, GPIO_OUT);
    gpio_set_dir(board::i2c::SDA, GPIO_IN);
    gpio_pull_up(board::i2c::SDA);

    // Nine clocks is enough for a slave to finish any byte it was in the middle
    // of and let go.
    for (int i = 0; i < 9 && !gpio_get(board::i2c::SDA); ++i) {
        gpio_put(board::i2c::SCL, 0);
        sleep_us(5);
        gpio_put(board::i2c::SCL, 1);
        sleep_us(5);
    }

    // Manual STOP: SDA low->high while SCL is high.
    gpio_set_dir(board::i2c::SDA, GPIO_OUT);
    gpio_put(board::i2c::SDA, 0);
    sleep_us(5);
    gpio_put(board::i2c::SCL, 1);
    sleep_us(5);
    gpio_set_dir(board::i2c::SDA, GPIO_IN);
    sleep_us(5);

    initI2c();
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

/// Latched by the touch controller's interrupt.
///
/// TP_INT is a ~1 ms PULSE, not a level held while a finger is down. Polling it
/// as a GPIO at frame rate misses almost every one -- measured, four samples in
/// twenty seconds of dragging -- so an edge interrupt latches it instead. The
/// few samples that did land were huge jumps, which then tripped the drag
/// column's fast-flick gain: the control was both starved and twitchy from the
/// same cause.
volatile bool g_touchIrq = false;

void touchIrqHandler(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    g_touchIrq = true;
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
    printf("\n");

    static board::Cst816 touch;
    const bool touchOk = touch.begin();
    if (touchOk) {
        gpio_set_irq_enabled_with_callback(board::touch::INT, GPIO_IRQ_EDGE_FALL, true,
                                           &touchIrqHandler);
    }
    printf("touch %s", touchOk ? "ok" : "NOT FOUND");
    if (touchOk) printf(" id 0x%02X", touch.chipId());
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
    lcd.setBacklight(kBacklightActive);

    static h0::DigitsFace digits;
    static h0::HourglassFace hourglass;
    static h0::SplitFlapFace splitflap;

    static h0::App app;
    static h0::OrientationTracker orient;
    static h0::GravityFilter filter;
    static h0::DragColumn colMin, colSec;
    static uint8_t activeCol = 0; // 0 none, 1 minutes, 2 seconds

    app.onMotion(h0::MotionEvent::Settled, time_us_64());
    app.setDuration(kDefaultDurationUs, time_us_64());

    printf("\nready -- stand it up to start, lay it flat to pause,\n");
    printf("turn it over to reset, face down to silence.\n");
    printf("while flat, drag the MIN / SEC columns to set the time.\n");
    printf("swipe left or right to change face\n\n");

    uint32_t frames = 0;
    uint64_t faceShownUntil = 0;
    uint64_t lastInteractionUs = 0;
    bool faceChangedThisTouch = false;
    uint8_t backlight = kBacklightActive;
    uint32_t imuFails = 0, touchFails = 0, touchReads = 0;
    uint64_t lastTouchUs = 0;
    bool touchActive = false;

    while (true) {
        const uint64_t now = time_us_64();

        h0::Vec3 raw{};
        const bool imuRead = imuOk && imu.readRaw(raw);
        if (!imuRead) {
            // The IMU is polled every frame, so it is the canary: if it stops
            // answering, the bus is wedged and everything else is about to fail
            // too. Recover once rather than limping.
            ++imuFails;
            recoverI2c();
        }
        if (imuRead) {
            const h0::Vec3 g = board::axisMap(raw);
            const h0::MotionEvent ev = orient.update(filter.push(g), now);
            if (ev != h0::MotionEvent::None) {
                const h0::Feedback fbk = app.onMotion(ev, now);
                lastInteractionUs = now;
                if (fbk != h0::Feedback::None) {
                    buzzer.play(fbk);
                    printf("[%s] -> %s  %lus left\n", orientationName(orient.current()),
                           feedbackName(fbk),
                           static_cast<unsigned long>(app.timer().remainingSeconds(now)));
                }
            }
        }

        // The dial is live only while the device is flat -- the setting posture.
        // Elsewhere a pocket could rewrite a running timer, and there is no undo.
        // Read on the latched interrupt, so no bus transaction happens while
        // nothing is being touched -- which is what stopped the bus wedging.
        board::TouchPoint tp{};
        bool touchRead = false;

        // The interrupt starts a drag; once a finger is down we poll every frame
        // for the duration. Relying on catching every pulse would drop samples
        // mid-drag, and a picker starved of samples is both unresponsive and --
        // because the gaps look like fast movement -- jerky.
        //
        // Polling only while a finger is actually down keeps the bus quiet the
        // rest of the time, which is what stopped it wedging.
        if (touchOk && (g_touchIrq || touchActive)) {
            g_touchIrq = false;
            ++touchReads;
            touchRead = touch.read(tp);
            if (!touchRead) ++touchFails;
            if (touchRead && tp.pressed) {
                touchActive = true;
                lastTouchUs = now;
                lastInteractionUs = now;
            } else if (touchRead && !tp.pressed) {
                touchActive = false;
                lastTouchUs = 0;
            }
        }

        // Belt and braces: if the controller goes quiet without ever reporting a
        // release, infer one. Otherwise the columns keep a stale drag reference
        // and the next touch jumps.
        if (touchActive && lastTouchUs != 0 && now - lastTouchUs > 200'000ull) {
            touchActive = false;
            lastTouchUs = 0;
            tp.pressed = false;
            touchRead = true;
        }

        // The controller stops pulsing when the finger lifts, so a release is
        // never reported -- it has to be inferred from silence. Without this the
        // columns keep their drag reference forever and the next touch is
        // measured against a stale position.
        if (!touchRead && lastTouchUs != 0 && now - lastTouchUs > 80'000ull) {
            lastTouchUs = 0;
            tp.pressed = false;
            touchRead = true; // deliver the release
        }
        if (touchRead) {
            // A horizontal swipe cycles the face. The controller detects it
            // itself, and it cannot be confused with a column drag because the
            // columns only ever consume vertical movement.
            // At most ONE face change per finger-down. The controller keeps
            // reporting the same gesture code for as long as the finger is
            // still there, so acting on the code directly fires it on every
            // frame of the swipe and races through all three faces. A time
            // debounce would only hide that; latching per touch is exact.
            const bool swipe = (tp.gesture == board::TouchGesture::SlideLeft ||
                                tp.gesture == board::TouchGesture::SlideRight);
            if (!tp.pressed) faceChangedThisTouch = false;

            if (swipe && !faceChangedThisTouch) {
                faceChangedThisTouch = true;
                app.cycleFace();
                buzzer.play(h0::Feedback::Resumed);
                faceShownUntil = now + 1'200'000ull;
                colMin.reset();
                colSec.reset();
                activeCol = 0;
            } else if (app.settingPosture()) {
                // Lock the column on touch-down. A finger drifts sideways during
                // a vertical drag, and switching wheels mid-gesture would move
                // whichever one it wandered over.
                if (tp.pressed && activeCol == 0) {
                    activeCol = (tp.x < 120) ? 1 : 2;
                } else if (!tp.pressed) {
                    activeCol = 0;
                }

                const int dMin = colMin.update(tp.pressed && activeCol == 1, tp.y);
                const int dSec = colSec.update(tp.pressed && activeCol == 2, tp.y);

                if (dMin != 0 || dSec != 0) {
                    int64_t secs = static_cast<int64_t>(app.timer().duration() / 1'000'000ull);
                    // Minutes carry into hours; seconds do NOT carry into
                    // minutes. A seconds wheel that dragged the minutes along
                    // would make the two columns fight each other.
                    secs += static_cast<int64_t>(dMin) * 60;
                    const int64_t curSec = ((secs % 60) + 60) % 60;
                    int64_t newSec = curSec + dSec;
                    newSec = ((newSec % 60) + 60) % 60;
                    secs += newSec - curSec;

                    if (secs < 0) secs = 0;
                    if (secs > 9 * 3600) secs = 9 * 3600; // nine hours is plenty
                    app.setDuration(static_cast<uint64_t>(secs) * 1'000'000ull, now);
                }
            } else {
                colMin.reset();
                colSec.reset();
                activeCol = 0;
            }
        }

        const h0::Feedback tickFb = app.tick(now);
        if (tickFb != h0::Feedback::None) {
            buzzer.play(tickFb);
            printf("-> %s\n", feedbackName(tickFb));
        }
        buzzer.update(now);

        if (app.settingPosture()) {
            // The dial replaces the face while flat: a rotary control with no
            // visible ring is undiscoverable, and the timer is not counting
            // anyway.
            const uint32_t total = app.timer().remainingSeconds(now);
            h0::PickerState ps;
            ps.hours = total / 3600;
            ps.minutes = (total % 3600) / 60;
            ps.seconds = total % 60;
            ps.minutesOffset = colMin.offsetPx();
            ps.secondsOffset = colSec.offsetPx();
            ps.activeColumn = activeCol;
            h0::SettingFace::renderAt(fb, ps);
        } else {
            h0::IFace* face = &digits;
            switch (app.face()) {
                case h0::FaceId::Hourglass: face = &hourglass; break;
                case h0::FaceId::SplitFlap: face = &splitflap; break;
                case h0::FaceId::Digits:    face = &digits; break;
            }
            face->render(fb, app.timer(), now);
            if (now < faceShownUntil) {
                // Name it briefly after a change. Three faces with no label
                // would leave the swipe feeling like it did something random.
                const char* n = face->name();
                const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, n);
                onebit::fillRect(fb, static_cast<int16_t>(120 - w / 2 - 4), 244,
                                 static_cast<int16_t>(w + 8), 13, WHITE);
                onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                                       static_cast<int16_t>(120 - w / 2), 246, n, BLACK);
            }
        }

        // A ringing alarm always goes to full brightness: it is the one moment
        // the device is trying to get attention from across a room.
        const bool idle = (now - lastInteractionUs) > kIdleAfterUs;
        const uint8_t want = (app.alarmSounding() || !idle) ? kBacklightActive
                                                            : kBacklightIdle;
        if (want != backlight) {
            backlight = want;
            lcd.setBacklight(backlight);
        }

        lcd.pushDirty(fb, tracker.update(fb));
        lcd.waitIdle();

        if (++frames % 200 == 0) {
            printf("  %s raw %+.2f %+.2f %+.2f  %lus  imuFail=%lu touchFail=%lu/%lu\n",
                   orientationName(orient.current()),
                   static_cast<double>(raw.x), static_cast<double>(raw.y),
                   static_cast<double>(raw.z),
                   static_cast<unsigned long>(app.timer().remainingSeconds(now)),
                   static_cast<unsigned long>(imuFails),
                   static_cast<unsigned long>(touchFails),
                   static_cast<unsigned long>(touchReads));
        }

        sleep_ms(25); // ~40 Hz, comfortably above the 125 Hz IMU rate we need
    }
}
