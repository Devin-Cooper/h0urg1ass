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
#include "hardware/watchdog.h"
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
#include "faces/setting_face.hpp"
#include "faces/timer_face.hpp"
#include "input/orientation.hpp"
#include "render/raster_ops.hpp"
#include "sand/sand_render.hpp"
#include "sand/sand_sim.hpp"
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
/// Quarter brightness. Ample indoors, and it takes the biggest single bite out
/// of the power budget -- the backlight dominates active draw, so this is worth
/// more than any amount of tuning elsewhere. It also flatters a white-on-black
/// display, where a bright backlight mostly lights up the parts of the screen
/// that are meant to be dark.
///
/// Perceived brightness is not linear in duty, so this looks considerably
/// brighter than a quarter. That is the point: the number that matters is the
/// current, and the eye barely notices what the battery does.
constexpr uint8_t kBacklightActive = 64;

/// NOTE: at an active level of 64 this is 56% of it, not the 18% it was against
/// the old 200 -- so the idle dim is now a slight fade rather than an obvious
/// state change, and saves correspondingly little.
constexpr uint8_t kBacklightIdle = 36;
constexpr uint64_t kIdleAfterUs = 20ull * 1'000'000ull;

/// Hold the soft power latch. GPIO15 -> R3 1k -> T1 base; T1's collector pulls
/// Q3's gate down, passing the battery through. Until this runs, the board is
/// alive only because a finger is on the button. Invisible over USB, where VBUS
/// reaches VSYS through D4 regardless.
void latchPower() {
    // Order matters twice over, and gpio_init() gets both wrong.
    //
    // SIO must be driving the pad HIGH before anything clears pad isolation.
    // gpio_init() sets the pin to input and value 0 first, then calls
    // gpio_set_function(), whose last act is to clear the pad's ISO bit. On a
    // wake from a POWMAN power state that isolation latch is the only thing
    // holding this pad high -- there is no finger on the button by then -- so
    // clearing it first drops the latch and the rail collapses. Invisible over
    // USB, where VBUS reaches VSYS through D4 whatever this pin does.
    sio_hw->gpio_set = 1u << board::power::SYS_EN;
    sio_hw->gpio_oe_set = 1u << board::power::SYS_EN;
    gpio_set_function(board::power::SYS_EN, GPIO_FUNC_SIO); // this clears ISO
    // ...so set it again afterwards. An isolated pad keeps its output enable and
    // level through the SDK's reset sweep, through FUNCSEL changes, and through
    // the low-leakage pin helpers, because the ISO bits are explicitly not
    // reset by the PADS block reset. A watchdog reset no longer switches the
    // board off on battery.
    //
    // It does not survive the always-on domain reset -- power-on, brownout, the
    // RUN pin, or a debug rescue. Those five still power the board down, which
    // is correct: they are the cases where nobody knows what state anything is
    // in.
    hw_set_bits(&pads_bank0_hw->io[board::power::SYS_EN], PADS_BANK0_GPIO0_ISO_BITS);
}

/// set_sys_clock_khz() re-parents clk_peri to PLL_USB at 48 MHz, capping SPI at
/// 24 MHz -- slower than stock, while the CPU gets faster.
/// Stop clocking things this board does not have, and stop clocking most of the
/// rest while the core is asleep.
///
/// The main loop already spends about three quarters of every frame parked in
/// WFE inside sleep_ms(), and the chip enters its SLEEP state whenever both
/// cores are in WFE and the DMA is idle -- core 1 is never launched, so it has
/// been sleeping in the bootrom since boot. But SLEEP_EN0/1 come out of reset as
/// all-ones, so that state has been gating precisely nothing.
///
/// WAKE_EN is the set clocked while running; SLEEP_EN the set clocked while
/// asleep. Only genuinely absent peripherals are dropped from WAKE_EN.
///
/// What must stay in SLEEP_EN, and why -- getting any of these wrong hangs the
/// device in WFE with no way back except the BOOT button:
///   TIMER0 + TICKS + REF_TICKS  the alarm that ends sleep_ms(). Gate these and
///                               the timer stops counting, the deadline never
///                               arrives, and the loop waits forever.
///   IO + PADS                   GPIO edge detect. The touch interrupt dies
///                               silently without them.
///   USB + USBCTRL               so the console still services, and so the
///                               1200-baud BOOTSEL reflash keeps working.
///   PWM                         NEVER gate this. A peripheral resumes in the
///                               state it was left in, so the backlight PWM
///                               counter freezes wherever it stopped -- at
///                               64/255 that is a one-in-four chance of holding
///                               full brightness for the whole idle window. The
///                               buzzer shares the block.
void pruneClockGating() {
    // Absent or unused: this board has no UART, and uses i2c1 and spi1 only.
    const uint32_t drop0 = CLOCKS_SLEEP_EN0_CLK_SYS_SHA256_BITS |
                           CLOCKS_SLEEP_EN0_CLK_SYS_I2C0_BITS;
    const uint32_t drop1 = CLOCKS_SLEEP_EN1_CLK_SYS_UART1_BITS |
                           CLOCKS_SLEEP_EN1_CLK_PERI_UART1_BITS |
                           CLOCKS_SLEEP_EN1_CLK_SYS_UART0_BITS |
                           CLOCKS_SLEEP_EN1_CLK_PERI_UART0_BITS |
                           CLOCKS_SLEEP_EN1_CLK_SYS_TRNG_BITS |
                           CLOCKS_SLEEP_EN1_CLK_SYS_TIMER1_BITS |
                           CLOCKS_SLEEP_EN1_CLK_SYS_SPI0_BITS;

    clocks_hw->wake_en0 &= ~drop0;
    clocks_hw->wake_en1 &= ~drop1;

    // Asleep, additionally drop the flash interface. It is by far the largest
    // single entry in the table, and nothing fetches an instruction while the
    // core is in WFE -- the hardware restores these clocks on the way out of
    // the sleep state, before any fetch can happen.
    clocks_hw->sleep_en0 = clocks_hw->wake_en0;
    clocks_hw->sleep_en1 = clocks_hw->wake_en1 & ~CLOCKS_SLEEP_EN1_CLK_SYS_XIP_BITS;
}

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

/// Quantise the gravity vector to the eight directions the sand understands.
///
/// The sand's y axis runs down the screen, matching the panel, so a gravity
/// vector pointing down the screen is Gravity::S.
h0::Gravity gravityDir(const h0::Vec3& g) {
    const float ax = g.x < 0 ? -g.x : g.x;
    const float ay = g.y < 0 ? -g.y : g.y;
    // A diagonal only when both axes are a real fraction of the total, so a
    // near-vertical hold does not flicker between S and SE.
    const bool diag = (ax > 0.38f && ay > 0.38f);
    if (diag) {
        if (g.y > 0) return g.x > 0 ? h0::Gravity::SE : h0::Gravity::SW;
        return g.x > 0 ? h0::Gravity::NE : h0::Gravity::NW;
    }
    if (ay >= ax) return g.y > 0 ? h0::Gravity::S : h0::Gravity::N;
    return g.x > 0 ? h0::Gravity::E : h0::Gravity::W;
}

/// The opposite direction. `Gravity` is declared S, SW, W, NW, N, NE, E, SE, so
/// opposites sit exactly four apart and this is arithmetic rather than a table.
h0::Gravity negate(h0::Gravity g) {
    return static_cast<h0::Gravity>((static_cast<int>(g) + 4) & 7);
}
static_assert(static_cast<int>(h0::Gravity::N) - static_cast<int>(h0::Gravity::S) == 4,
              "negate() assumes opposites are four apart in the enum");

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
    // White figures on a black field. The framebuffer convention is unchanged --
    // BLACK still means ink everywhere in the code -- this only decides which way
    // round the glass shows it. It also suits the panel physically: an unlit
    // pixel is the darkest thing this display can do, so a mostly-black frame is
    // both the better-looking one and the cheaper one.
    lcd.setInverted(true);
    lcd.clear(WHITE);
    lcd.setBacklight(kBacklightActive);

    static h0::TimerFace face;

    static h0::App app;
    static h0::OrientationTracker orient;
    static h0::GravityFilter filter;
    static h0::DragColumn colMin, colSec;
    static uint8_t activeCol = 0; // 0 none, 1 minutes, 2 seconds

    // Seed a duration without pretending the device is flat. Forcing the
    // setting posture at boot left the picker live in the hand whenever the
    // device powered on upright, with the timer face never drawn at all --
    // there is no transition at power-on for an event-driven flag to observe.
    app.setFlat(true);
    app.setDuration(kDefaultDurationUs, time_us_64());
    app.setFlat(false);

    // Measure one sand tick on the real part before committing to the face.
    // Every timing figure for the simulation so far is host, -O2, Apple
    // Silicon; the M33 at 125 MHz is plausibly 30-60x slower, and the frame
    // budget depends entirely on which.
    {
        static h0::SandSim probe;
        probe.setWalls(h0::makeVessel(2));
        probe.seed(1);
        const int floorRow = h0::sandgeom::FLOOR_ROW;
        for (int cy = floorRow - 40; cy < floorRow; ++cy)
            for (int cx = 1; cx < h0::SandGrid::W - 1; ++cx)
                if (!probe.walls().get(cx, cy)) probe.sand().set(cx, cy, true);
        const int grains = probe.sand().count();

        // Several rounds, because the state changes as the sand drains and a
        // single average hides that. The first round starts from a packed slab
        // and the last from a settled pile -- if those differ a lot, the cost is
        // occupancy-dependent and a single number is misleading.
        constexpr int kProbeTicks = 100;
        printf("sand: %d grains,", grains);
        for (int round = 0; round < 4; ++round) {
            const absolute_time_t s0 = get_absolute_time();
            for (int i = 0; i < kProbeTicks; ++i) probe.step(h0::Gravity::S);
            const int64_t us = absolute_time_diff_us(s0, get_absolute_time());
            printf("  %.2f", static_cast<double>(us) / kProbeTicks / 1000.0);
        }
        printf(" ms/tick (rounds 1-4)\n");
    }

    // Clock pruning goes here, after every peripheral is up and configured, so
    // nothing is initialised through a gated clock.
    pruneClockGating();

    // A safety net for exactly that change. If a clock this loop needs turns out
    // to be gated, the core parks in WFE and never returns -- and a device that
    // never returns cannot be reflashed over its own USB, because the 1200-baud
    // BOOTSEL handshake needs live firmware to answer it. The watchdog turns
    // that dead end into a reboot every eight seconds, and each reboot opens the
    // three-second window at the top of main() that flashing relies on.
    //
    // Safe on battery only because the pad isolation set in latchPower() holds
    // GPIO15 through a watchdog reset. Before that change this would have
    // switched the board off instead of restarting it.
    watchdog_enable(8000, true);

    printf("\nready -- stand it up to start, lay it flat to pause,\n");
    printf("turn it over to reset, face down to silence.\n");
    printf("while flat, drag the MIN / SEC columns to set the time.\n");
    printf("turn it over and the display follows\n\n");

    uint32_t frames = 0;
    uint64_t lastInteractionUs = 0;

    // True while the device is held the other way up. Latched from the last
    // definite upright posture: flat and face-down have no in-plane direction
    // to read, so they keep whatever was last known rather than flapping.
    bool inverted = false;
    uint8_t backlight = kBacklightActive;
    uint32_t imuFails = 0, touchFails = 0, touchReads = 0;
    uint64_t lastTouchUs = 0;
    bool touchActive = false;
    uint32_t consecImuFails = 0;

    while (true) {
        const uint64_t now = time_us_64();

        h0::Vec3 raw{};
        const bool imuRead = imuOk && imu.readRaw(raw);
        if (!imuRead) {
            ++imuFails;
            // The IMU is the canary -- polled every frame, so if it stops
            // answering the bus is wedged and everything else is about to fail.
            //
            // But only recover when the part was actually FOUND at boot, and
            // only after several consecutive failures. Without both guards, a
            // board with no IMU tears the bus down and re-inits it forty times
            // a second, forever, while the touch controller is the only live
            // device on it.
            if (imuOk && ++consecImuFails >= 3) {
                consecImuFails = 0;
                recoverI2c();
            }
        } else {
            consecImuFails = 0;
        }
        if (imuRead) {
            const h0::Vec3 g = board::axisMap(raw);
            const h0::MotionEvent ev = orient.update(filter.push(g), now);

            // Posture is derived from the MEASURED orientation, not accumulated
            // from events. An event-derived flag goes stale whenever one is
            // missed, and they are missed routinely: `flat -> edge -> upright`
            // never produces the FlatBack->vertical transition that Raised
            // needs, so the picker would stay live in the hand.
            app.setFlat(orient.current() == h0::Orientation::FlatBack);

            // The touch controller costs 1.6 mA held awake and 6 uA in its own
            // standby, and touch is only used by the picker. Hold it awake only
            // while the picker is live, so the wake delay lands where nobody is
            // dragging and the current lands where somebody is.
            if (touchOk) touch.setHeldAwake(app.settingPosture());

            if (orient.current() == h0::Orientation::UprightA) inverted = false;
            else if (orient.current() == h0::Orientation::UprightB) inverted = true;

            // The sand follows real gravity. Lying flat is the one posture with
            // no in-plane direction, so it keeps whatever it had rather than
            // spinning on sensor noise.
            if (orient.current() != h0::Orientation::FlatBack &&
                orient.current() != h0::Orientation::FaceDown) {
                // Content space is a 180-degree rotation away from panel space
                // when inverted, and that negates both axes. Handing the sim
                // content-space gravity means it only ever sees the direction it
                // was built for -- the simulation never learns the device can be
                // turned over.
                h0::Gravity down = gravityDir(filter.value());
                if (inverted) down = negate(down);
                face.setGravity(down);
            }

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

            // The frame is rotated for the inverted posture, so the panel the
            // finger is touching no longer matches the coordinates the
            // controller reports. Without this the picker's column lock picks
            // the wrong wheel and every drag runs backwards -- and since
            // `inverted` is latched from the last upright posture, it applies
            // the moment anyone lays the device down to set a timer after
            // turning it over.
            if (touchRead && inverted && tp.pressed) {
                tp.x = static_cast<int16_t>(board::lcd::WIDTH - 1 - tp.x);
                tp.y = static_cast<int16_t>(board::lcd::HEIGHT - 1 - tp.y);
            }
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

        // A poll normally reports the release itself (FingerNum goes to 0).
        // This covers the case where reads are FAILING instead: without it
        // `touchActive` stays set, the loop keeps polling a dead bus every
        // frame burning i2c timeouts, and the columns hold a stale drag
        // reference -- the exact opposite of the bus-quiet property that
        // stopped the wedging.
        if (touchActive && lastTouchUs != 0 && now - lastTouchUs > 200'000ull) {
            touchActive = false;
            lastTouchUs = 0;
            tp.pressed = false;
            touchRead = true; // deliver the inferred release
        }
        if (touchRead) {
            // The horizontal swipe used to cycle faces. With one face there is
            // nothing to cycle, so the gesture is now free -- deliberately left
            // unbound rather than given a second job, because a gesture with no
            // acknowledgement path is worse than no gesture.
            if (!tp.pressed) {
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
                    // Both wheels WRAP, and neither carries into the other. A
                    // seconds wheel that dragged the minutes along would make
                    // the two columns fight each other; a minutes wheel that
                    // clamped would stick silently at the end of its travel.
                    //
                    // 100 minutes is the ceiling because the readout has five
                    // cells and cannot grow. Making the wheel wrap there rather
                    // than clamping somewhere higher keeps the dial and the
                    // display honest about the same limit.
                    const int64_t cur =
                        static_cast<int64_t>(app.timer().duration() / 1'000'000ull);
                    const int64_t mins = (((cur / 60 + dMin) % 100) + 100) % 100;
                    const int64_t secs = (((cur % 60 + dSec) % 60) + 60) % 60;
                    app.setDuration(static_cast<uint64_t>(mins * 60 + secs) * 1'000'000ull,
                                    now);
                }
            } else {
                colMin.reset();
                colSec.reset();
                activeCol = 0;
            }
        }

        // Advance the sand on its OWN clock, not once per render: the drain has
        // to run at a fixed rate whatever the frame rate happens to be. Only
        // while the face is actually showing -- a simulation nobody can see is
        // pure battery cost.
        if (!app.settingPosture()) {
            face.tick(app.timer(), now);
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
            // The DURATION, not the remaining time. The picker edits duration,
            // so showing remaining meant that laying down a part-run timer
            // displayed one number and edited another: run 5:00 down to 3:00,
            // nudge minutes once, and it jumps to 6:00.
            const uint32_t total =
                static_cast<uint32_t>(app.timer().duration() / 1'000'000ull);
            h0::PickerState ps;
            ps.minutes = total / 60;
            ps.seconds = total % 60;
            ps.minutesOffset = colMin.offsetPx();
            ps.secondsOffset = colSec.offsetPx();
            ps.activeColumn = activeCol;
            h0::SettingFace::renderAt(fb, ps);
        } else {
            face.render(fb, app.timer(), now);
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

        // Rotate the FINISHED frame rather than teaching the simulation, the
        // picker, the font and the touch map that the device can be turned over.
        // Everything upstream keeps working in a content space where up is up.
        if (inverted) h0::rotate180(fb);

        watchdog_update();

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

        // ~30 Hz in practice: 25 ms of sleep plus render, SPI push and any i2c
        // timeouts. That is BELOW the IMU's 125 Hz output rate, so most samples
        // are dropped -- fine for a gravity vector that is low-passed anyway,
        // but it means the filter's time constant is ~330 ms rather than the
        // ~100 ms its own comment assumes at 100 Hz.
        sleep_ms(25);
    }
}
