// h0urg1ass -- interactive build.
//
// The timer, driven by how you hold it:
//
//   hold it upright   set the time; the wheels are live whenever it is idle
//   turn it over      start it -- and, over a live run, reset it to full
//   rest it on end    pause, after a second
//   lay it flat       pause, and dial a new time
//   stand it up       off flat: start if idle, resume if paused; off its
//                     side or a tilt: resume only -- never starts
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
#include "app/settings_ui.hpp"
#include "board/battery.hpp"
#include "board/buzzer.hpp"
#include "board/cst816.hpp"
#include "board/flash_store.hpp"
#include "board/pins.hpp"
#include "board/power_button.hpp"
#include "board/qmi8658.hpp"
#include "board/st7789_1in69.hpp"
#include "input/drag_column.hpp"
#include "faces/boot_face.hpp"
#include "faces/power_face.hpp"
#include "faces/setting_face.hpp"
#include "faces/settings_face.hpp"
#include "faces/timer_face.hpp"
#include "input/orientation.hpp"
#include "power/backlight_policy.hpp"
#include "power/power_policy.hpp"
#include "render/raster_ops.hpp"
#include "sand/sand_render.hpp"
#include "sand/sand_sim.hpp"
#include "settings/settings_store.hpp"
#include "settings/theme.hpp"
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

/// The duration the device starts with.
constexpr uint64_t kDefaultDurationUs = 2ull * 60ull * 1'000'000ull;

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
        case h0::Orientation::OnSide:   return "ONSIDE";
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
        case h0::Feedback::SettingsOpen:  return "SETTINGS";
        case h0::Feedback::SettingsSaved: return "SAVED";
        case h0::Feedback::Booted:   return "BOOT";
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

    // Settings first, and before the chirp -- MUTE has to be honoured, and
    // FlashStore is XIP with no i2c, so nothing that can wedge is touched yet.
    static board::FlashStore flashStore;
    static h0::SettingsStore settingsStore(flashStore);
    settingsStore.load();

    // The chirp is the ROBUST half of the boot signal. Buzzer::begin() is PWM
    // configuration -- it cannot hang and cannot wedge a bus -- so sounding it
    // here means the device announces itself even if everything below dies.
    static board::Buzzer buzzer;
    buzzer.begin();
    if (!settingsStore.settings().mute) buzzer.play(h0::Feedback::Booted);

    static board::St7789_1in69 lcd;
    if (!lcd.begin()) {
        printf("FATAL: display init failed\n");
        buzzer.stop(); // otherwise the boot chirp holds its tone forever
        while (true) tight_loop_contents();
    }
    printf("SPI %.2f MHz\n", lcd.actualBaud() / 1e6);

    static onebit::Framebuffer<board::lcd::WIDTH, board::lcd::HEIGHT> fb;
    static onebit::DirtyRectTracker tracker(board::lcd::WIDTH, board::lcd::HEIGHT);

    {
        const h0::Theme& t = h0::themeFor(
            static_cast<h0::ThemeId>(settingsStore.settings().themeId));
        lcd.setColors(t.ink, t.paper);
    }
    h0::BootFace::renderAt(fb);
    tracker.markAllDirty();
    lcd.pushDirty(fb, tracker.update(fb));
    lcd.waitIdle();
    lcd.setBacklight(settingsStore.settings().backlightActive);

    // Idle before touching any bus that can wedge. If the app hangs past here
    // USB is already up, so picotool can still force BOOTSEL and reflashing
    // needs no button -- and the watchdog reboots straight back into this
    // window. It stays three seconds: shortening it would trade that for
    // cosmetics. The splash above is what stops it looking like the device
    // never came on.
    //
    // Pumped rather than slept: the buzzer's note script advances in update(),
    // so a bare sleep_ms would hold the chirp on its first note throughout.
    {
        const uint64_t bootUntil = time_us_64() + 3'000'000ull;
        while (time_us_64() < bootUntil) {
            buzzer.update(time_us_64());
            sleep_ms(5);
        }
        buzzer.stop();
    }

    printf("\n=== h0urg1ass -- interactive ===\n");
    printf("clk_sys=%lu clk_peri=%lu\n", static_cast<unsigned long>(clock_get_hz(clk_sys)),
           static_cast<unsigned long>(clock_get_hz(clk_peri)));

    initI2c();
    resetTouch();

    static board::Battery battery;
    battery.begin();

    // GPIO14, input with pull-up. Cheap enough to bring up this early; the
    // policy that reads it is seeded down with the other app-state statics.
    static board::PowerButton powerButton;
    powerButton.begin();

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
    printf("\n");

    // The learned floor AS IT WAS AT BOOT. Read once, here, and NEVER
    // reassigned -- this is the only thing allowed to arm the low-battery
    // cutoff.
    //
    // BatteryFloor::cutoffMv is applyCal(floor) + 250 mV, which is by
    // construction above the reading that produced the floor. So a floor
    // learned during the CURRENT descent, if it were allowed to arm, would
    // power the device off in the same frame it first appeared -- ending the
    // very run to brownout that the floor learner exists to observe, and
    // leaving the device permanently stuck at the clamped 3750 mV ceiling
    // afterwards because it can never again get low enough to learn. Only a
    // floor that survived a power cycle may arm the cutoff. If you are tempted
    // to "fix" this by reading settingsStore.settings() or sessionSettings at
    // the call site below, that is the bug, not the fix.
    const uint16_t bootFloorRawMv = settingsStore.settings().batFloorRawMv;

    {
        const h0::BatteryReading b =
            battery.sample(time_us_64(), settingsStore.settings()).reading;
        printf("battery %u mV (%s)\n", static_cast<unsigned>(b.milliVolts),
               b.calibrated ? "calibrated" : "uncal");
    }

    static h0::TimerFace face;

    static h0::App app;
    static h0::OrientationTracker orient;
    static h0::GravityFilter filter;
    static h0::DragColumn colMin, colSec;
    static uint8_t activeCol = 0; // 0 none, 1 minutes, 2 seconds
    static h0::SettingsUi settingsUi;
    static h0::GestureGate gestureGate;
    static h0::BatteryReading batteryReading;
    // The two-stage hold, the idle rule and the calibration gate -- all of it
    // pure, and re-derived from `powerButton.isDown()` once per frame below.
    static h0::PowerPolicy powerPolicy;
    static h0::PowerDecision powerDecision;
    // True once a wheel has actually ADVANCED during the current touch --
    // distinct from mere contact, which both DragColumn::tracking() and
    // SettingsUi::activeColumn() latch on the first, reference-establishing
    // sample. Cleared on release, alongside the columns it shadows.
    static bool wheelMovedThisTouch = false;

    // The settings this session behaves as having once the menu is closed.
    // Deliberately NOT settingsStore.settings(): the commit path below can have
    // a flash write fail AFTER the edit has already been pushed to every live
    // subsystem via applySettings(), and this is what stops the session from
    // straddling two different records when that happens -- every consumer of
    // `eff` keeps agreeing with what is actually on the hardware, whether or
    // not that record also reached flash.
    static h0::Settings sessionSettings = settingsStore.settings();

    // The theme currently pushed to the LCD, so applySettings() below can tell
    // whether an edit actually changed it. Seeded outside ThemeId's valid range
    // (0..3), not from sessionSettings.themeId -- the LCD driver's own default
    // ink/paper need not match the loaded theme, so the first applySettings()
    // call, at boot below, must always run setColors rather than see a
    // spurious "unchanged" and skip it.
    static uint8_t appliedThemeId = static_cast<uint8_t>(h0::ThemeId::Count);

    // Push settings into the subsystems that own them. Called on load, on every
    // live edit, and on cancel -- so a preview and a restore go through exactly
    // the same path and cannot disagree.
    //
    // setColors + markAllDirty run only when the THEME actually changed. The
    // design budgets one 19.1 ms full-frame push per THEME step; this lambda
    // runs on EVERY live edit, and CAL's 151-entry ladder with 5x acceleration
    // changes on nearly every frame while dragging -- an unconditional push
    // here would make every CAL step a full-frame push too, and roughly halve
    // the frame rate for the whole drag. Alarm timeout and mute stay
    // unconditional: both are cheap regardless of how often they run.
    auto applySettings = [&](const h0::Settings& s) {
        if (s.themeId != appliedThemeId) {
            appliedThemeId = s.themeId;
            const h0::Theme& t = h0::themeFor(static_cast<h0::ThemeId>(s.themeId));
            lcd.setColors(t.ink, t.paper);
            // The tracker has no idea the colours moved, so without this the
            // panel keeps showing the old theme until something else happens
            // to dirty it.
            tracker.markAllDirty();
        }
        app.setAlarmTimeout(static_cast<uint64_t>(s.alarmS) * 1'000'000ull);
        face.setMuted(s.mute != 0);
    };

    // The picker's columns track independently of the settings menu, so
    // closing the menu -- by any path -- must not leave them holding a
    // reference from before it opened. Without this, the first picker sample
    // after a close measures against a stale y and slams the dialled
    // duration by however far the finger has moved since, amplified up to 5x
    // by the velocity gain -- with no undo. Called from every exit path.
    auto resetPickerColumns = [&] {
        colMin.reset();
        colSec.reset();
        activeCol = 0;
    };

    // MUTE silences the only acknowledgement channel a wholly invisible gesture
    // vocabulary has. It ships at the user's explicit direction; Task 14 adds
    // the glyph that makes the state visible rather than only audible-by-absence.
    //
    // Reads the LIVE settings, not the stored ones, so toggling SOUND previews
    // like every other row.
    auto say = [&](h0::Feedback f) {
        const bool muted = settingsUi.isOpen() ? settingsUi.live().mute : sessionSettings.mute;
        if (!muted) buzzer.play(f);
    };

    // Colour is decided in exactly one place: the theme's ink/paper pair, which
    // the expander bakes into its lookup table. The panel keeps its own INVON
    // requirement, untouched.
    //
    // These two were previously fighting. setInverted(true) sends INVOFF here --
    // (geometry().invert != uiInverted_) with both true -- so the panel ran with
    // its mandatory inversion OFF and every RGB565 value reached the glass
    // COMPLEMENTED. That is invisible while the only colours are 0x0000 and
    // 0xFFFF, and turns an amber ink into blue the moment a theme exists.
    //
    // applySettings() must run before clear() below, or this boot-time clear
    // paints with whatever colours the driver defaulted to rather than the
    // loaded theme -- the same class of bug the paragraph above describes.
    applySettings(sessionSettings);
    lcd.clear(WHITE);
    lcd.setBacklight(sessionSettings.backlightActive);

    // No posture games needed any more: the timer starts Idle, and Idle is a
    // setting posture, so the dial is simply live.
    app.setDuration(kDefaultDurationUs, time_us_64());

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

    printf("\nready -- dial a time, turn it over to start.\n");
    printf("stand it up off flat to start or resume; off its side, resume only.\n");
    printf("rest it on its end or lay it flat to pause.\n");
    printf("turn it over again to reset. face down to silence.\n");
    printf("drag the MIN / SEC columns whenever the picker is showing.\n\n");

    uint32_t frames = 0;
    uint64_t lastInteractionUs = 0;

    // True while the device is held the other way up. Latched from the last
    // definite upright posture: flat and face-down have no in-plane direction
    // to read, so they keep whatever was last known rather than flapping.
    bool inverted = false;
    uint8_t backlight = settingsStore.settings().backlightActive;
    bool rendering = true;
    uint32_t imuFails = 0, touchFails = 0, touchReads = 0;
    uint64_t lastTouchUs = 0;
    bool touchActive = false;
    uint32_t consecImuFails = 0;
    // Guards the power-off settings commit against firing on every frame a
    // hold spends past the threshold, and against firing twice for the same
    // hold (once at PromptRelease, again at PowerOff). Reset to false
    // whenever no power-off sequence is in flight, so the next one commits
    // again.
    bool settingsCommittedForShutdown = false;

    while (true) {
        const uint64_t now = time_us_64();

        // THE settings every consumer reads this frame.
        //
        // While the menu is open this is the in-progress edit, not the stored
        // one -- which is what makes live preview real. Reading
        // settingsStore.settings() here instead would mean dragging BRIGHT
        // changes nothing on the glass until the swipe commits, and "live
        // preview is the only honest way to choose a brightness" is the whole
        // reason the commit model looks the way it does.
        //
        // The closed-menu fallback is sessionSettings, not
        // settingsStore.settings() -- see its declaration above, and the commit
        // path below.
        const h0::Settings& eff =
            settingsUi.isOpen() ? settingsUi.live() : sessionSettings;

        // Every frame, unconditionally -- board/battery.hpp: "Call every
        // frame." It self-rate-limits to 1 Hz internally, so this costs
        // nothing extra; calling it only while settings is open and rendering
        // (as before) left the IIR unfed for hours between settings visits, so
        // the first sample after opening the menu moved the displayed voltage
        // by just 1/16 of however far it had drifted since the boot reading --
        // and that same stale value drove both the bucket and the CAL row that
        // calibration exists to be trusted against.
        //
        // `sessionSettings`, NOT `eff`. The learners inside sample() compare
        // against the stored gain and the stored floor, and `eff` is the
        // settings menu's preview -- a snapshot taken at open() that only a
        // drag ever mutates. Feeding them that froze BatteryFloor's reference
        // for the whole menu session, so with an unlearned floor (the shipping
        // state) every single sample re-qualified and committed: a write every
        // second, 4 kB of sector filled in 16 s, and then SettingsStore's
        // forced inline eraseSector -- a ~400 ms interrupt-masked stall --
        // over and over. That is precisely the hazard settings_store.hpp's
        // deferred-erase design exists to prevent.
        //
        // The CAL row's live preview is untouched: SettingsFace::formatValue
        // re-derives its own voltage from bat.rawMilliVolts and the PREVIEWED
        // s.batCalPermille, so dragging the wheel still moves the number.
        // What does change is the BATTERY row's bucket, which now follows the
        // committed gain rather than the preview -- the more honest reading
        // anyway, since the bucket is a claim about the pack and not about
        // what the wheel is currently resting on.
        const board::BatteryUpdate batteryUpdate = battery.sample(now, sessionSettings);
        batteryReading = batteryUpdate.reading;
        if (batteryUpdate.newCalPermille != 0 || batteryUpdate.newFloorRawMv != 0) {
            // Update sessionSettings itself and commit THAT -- not a separate
            // copy of settingsStore.settings() -- for the same reason every
            // other commit path in this file keeps the two in lockstep (see
            // sessionSettings's own declaration comment above). sample() reads
            // sessionSettings, so the next sample re-reads whatever
            // sessionSettings.batFloorRawMv holds; leaving it stale here would
            // mean that reference never advances, the
            // `rawMv + kStepMv <= storedRaw` deadband in BatteryFloor::update
            // would keep being satisfied, and this block would commit every
            // second instead of once -- exactly the write-spam
            // battery_floor.hpp's "the whole learning burst happens once"
            // comment and SettingsStore's one-write-per-session wear budget
            // assume can't happen. Committing settingsStore.settings()
            // directly, as before, left sessionSettings holding the pre-learn
            // values permanently, which both feeds that stale comparison AND
            // means the next unrelated menu-open-and-swipe (SettingsUi::open
            // snapshots from `eff`, i.e. sessionSettings) silently writes the
            // pre-learn gain and floor back over the learned ones.
            if (batteryUpdate.newCalPermille != 0) {
                sessionSettings.batCalPermille = batteryUpdate.newCalPermille;
            }
            if (batteryUpdate.newFloorRawMv != 0) {
                sessionSettings.batFloorRawMv = batteryUpdate.newFloorRawMv;
            }
            settingsStore.commit(sessionSettings);
        }

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

            // Any change of posture is interaction, event or not. Section 7.4
            // notes this happens without an event too: flat -> edge -> upright
            // never produces Raised, so a wake source that only watched events
            // would leave a device picked up and turned over dark.
            static h0::Orientation lastOrientation = h0::Orientation::Unknown;
            if (orient.current() != lastOrientation) {
                lastOrientation = orient.current();
                lastInteractionUs = now;
            }

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
                lastInteractionUs = now;
                bool menuConsumedEvent = false;
                if (settingsUi.isOpen()) {
                    // Leaving flat cancels the edit either way. Most events are
                    // then CONSUMED too: a raise that closes settings must not
                    // also resume the timer.
                    //
                    // Flip is the one exception, and it is not dead code:
                    // settingPosture() now opens this menu whenever the device
                    // is upright and Idle, not only flat, so a Flip can land
                    // here -- power on upright, swipe sideways to open
                    // settings, then turn it over. "Turn it over to start" is
                    // the central rule of this gesture model and has to mean
                    // the same thing everywhere, so the menu is still
                    // cancelled but the Flip is also handed to App below: the
                    // timer starts, exactly as if the menu had never opened.
                    const h0::Settings restored = settingsUi.cancel();
                    applySettings(restored);
                    resetPickerColumns();
                    if (ev != h0::MotionEvent::Flip) {
                        say(h0::Feedback::Rejected);
                        menuConsumedEvent = true;
                    }
                }
                if (!menuConsumedEvent) {
                    const h0::Feedback fbk = app.onMotion(ev, now);
                    if (fbk != h0::Feedback::None) {
                        say(fbk);
                        printf("[%s] -> %s  %lus left\n",
                               orientationName(orient.current()), feedbackName(fbk),
                               static_cast<unsigned long>(
                                   app.timer().remainingSeconds(now)));
                    }
                }
            }

            // Backstop for the paths that leave flat WITHOUT an event at all --
            // flat -> edge -> upright never produces Raised.
            if (settingsUi.isOpen() && !app.settingPosture()) {
                applySettings(settingsUi.cancel());
                resetPickerColumns();
                say(h0::Feedback::Rejected);
            }
        }

        // The dial is live in the setting posture -- flat OR upright-and-Idle,
        // not only flat. Elsewhere a pocket could rewrite a running timer,
        // and there is no undo.
        // Read on the latched interrupt, so no bus transaction happens while
        // nothing is being touched -- which is what stopped the bus wedging.
        board::TouchPoint tp{};
        bool touchRead = false;

        // `pressed` is now the controller's contact truth; a bad coordinate no
        // longer masquerades as a release. Anything that consumes a POSITION must
        // test both, or a single corrupt sample becomes a ~4000 px drag delta.
        //
        // Computed once a real read lands and held through the rest of the
        // frame -- the inferred-release path below only ever forces `pressed`
        // false, which can only take `usable` from false to false, never from
        // true to false, so a value set here stays correct for the picker logic
        // further down.
        bool usable = false;

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
            usable = tp.pressed && tp.positionValid;

            // The frame is rotated for the inverted posture, so the panel the
            // finger is touching no longer matches the coordinates the
            // controller reports. Without this the picker's column lock picks
            // the wrong wheel and every drag runs backwards -- and since
            // `inverted` is latched from the last upright posture, it applies
            // the moment anyone lays the device down to set a timer after
            // turning it over.
            if (touchRead && inverted && usable) {
                tp.x = static_cast<int16_t>(board::lcd::WIDTH - 1 - tp.x);
                tp.y = static_cast<int16_t>(board::lcd::HEIGHT - 1 - tp.y);
            }
            if (!touchRead) ++touchFails;
            // Any successful read is interaction -- including a bare touch IRQ
            // that resolves to no pressed point, which is otherwise silent.
            if (touchRead) lastInteractionUs = now;
            if (touchRead && tp.pressed) {
                touchActive = true;
                lastTouchUs = now;
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
            usable = false; // the inferred release carries no usable position
            touchRead = true; // deliver the inferred release
        }
        if (touchRead) {
            // `usable` was computed above where the read landed (or forced false
            // by the inferred-release path); reused here rather than re-derived.
            //
            // Entry and exit: a sideways swipe, in the setting posture (flat OR
            // upright-and-Idle). Both directions mean the same thing, which
            // sidesteps the coordinate mirroring above entirely -- a gesture
            // that means the same either way does not care which way up the
            // device is held.
            //
            // Evaluated BEFORE the reset below, or the gate's per-touch latch is
            // cleared before it can be read.
            const bool swipeEdge = tp.gestureIsNew &&
                                   (tp.gesture == board::TouchGesture::SlideLeft ||
                                    tp.gesture == board::TouchGesture::SlideRight);

            // `dragged` means a wheel has MOVED -- not merely that a finger is
            // in contact. DragColumn::tracking() and SettingsUi::activeColumn()
            // both latch on the first, reference-establishing sample of a
            // touch, which moves nothing by design; using either here would
            // latch the gate closed on touch-down, before the controller can
            // ever report the slide gesture that opens or commits the menu.
            // wheelMovedThisTouch instead only goes true where real movement
            // is observed, below, and is cleared on release alongside the
            // columns it shadows.
            const bool dragged = wheelMovedThisTouch;

            if (gestureGate.onTouch(tp.pressed, swipeEdge, dragged, now) &&
                app.settingPosture()) {
                lastInteractionUs = now;
                if (settingsUi.isOpen()) {
                    const h0::Settings out = settingsUi.commit();
                    const bool ok = settingsStore.commit(out);
                    // Adopt `out` for the rest of this session regardless of
                    // `ok`. settingsStore.settings() only updates on a
                    // successful write, so falling back to it (as `eff` used
                    // to) would silently revert brightness, the dim/blank
                    // timeouts and the battery calibration the instant the menu
                    // closes on a failed commit -- while theme, alarm timeout
                    // and mute stayed new, because applySettings() below pushes
                    // those to the hardware unconditionally. sessionSettings
                    // keeps every field, not only the three applySettings
                    // happens to touch, in agreement with what is on screen.
                    sessionSettings = out;
                    applySettings(out);
                    // The picker's own columns are untouched while the menu is
                    // open, so they still hold whatever y they last saw before
                    // it opened. Without this reset, the very next picker
                    // sample -- this same frame, if the finger is still down --
                    // measures against that stale reference and slams the
                    // dialled duration by however far the finger has moved
                    // since, amplified up to 5x by the velocity gain.
                    resetPickerColumns();
                    // A failed write plays Rejected, not Saved: the two differ
                    // only in whether `out` also reached flash, not in what
                    // this session looks like from here on -- see
                    // sessionSettings above. Only a reboot would resurface the
                    // stale flashed record.
                    say(ok ? h0::Feedback::SettingsSaved : h0::Feedback::Rejected);
                } else {
                    settingsUi.open(eff);
                    say(h0::Feedback::SettingsOpen);
                }
            }

            // The horizontal swipe used to cycle faces. With one face there is
            // nothing to cycle, so the gesture is now free -- deliberately left
            // unbound rather than given a second job, because a gesture with no
            // acknowledgement path is worse than no gesture.
            if (!tp.pressed) {
                colMin.reset();
                colSec.reset();
                activeCol = 0;
                wheelMovedThisTouch = false;
                settingsUi.onDrag(0, false, tp.y);
            } else if (settingsUi.isOpen()) {
                if (usable) {
                    // Same column lock as the picker, same boundary.
                    if (activeCol == 0) activeCol = (tp.x < 120) ? 1 : 2;
                    if (settingsUi.onDrag(activeCol, true, tp.y)) {
                        wheelMovedThisTouch = true;
                        applySettings(settingsUi.live());
                    }
                }
            } else if (app.settingPosture()) {
                // Lock the column on touch-down. A finger drifts sideways during
                // a vertical drag, and switching wheels mid-gesture would move
                // whichever one it wandered over.
                if (usable && activeCol == 0) {
                    activeCol = (tp.x < 120) ? 1 : 2;
                }

                const int dMin = colMin.update(usable && activeCol == 1, tp.y);
                const int dSec = colSec.update(usable && activeCol == 2, tp.y);

                if (dMin != 0 || dSec != 0) {
                    wheelMovedThisTouch = true;
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
            say(tickFb);
            printf("-> %s\n", feedbackName(tickFb));
        }
        // A ringing alarm is the device demanding attention, not idleness --
        // treating it as the latter is wrong on its own, independently of
        // PowerPolicy's own alarmSounding gate below. Also isRunning() goes
        // false the instant expiry starts the alarm, so idleUs has been
        // accumulating for the timer's entire run right up to this frame;
        // without this, that accumulated idleUs is still sitting there, past
        // offAfterS, on the very frame PowerPolicy is fed timerRunning=false.
        if (tickFb == h0::Feedback::AlarmOn) lastInteractionUs = now;
        buzzer.update(now);

        // The power button. Fed once per frame, mirroring backlightFor() just
        // below -- and deliberately run BEFORE idleUs is computed there, for
        // the same reason touch and motion update lastInteractionUs ahead of
        // that line above: a hold that begins on a blanked screen must force
        // the backlight ladder back to ACTIVE on THIS frame, not the next one,
        // or the prompt would be decided but never drawn. `blanked` reads
        // `rendering` as of the START of this frame -- the ladder has not run
        // yet this iteration, so that is exactly "is the screen dark right
        // now".
        {
            h0::PowerInput pin;
            pin.now = now;
            pin.buttonDown = powerButton.isDown();
            pin.idleUs = now - lastInteractionUs;
            pin.timerRunning = app.timer().isRunning();
            // isRunning() alone goes false the instant expiry starts the
            // alarm, so PowerPolicy needs this too -- same reasoning
            // backlightFor() was already given app.alarmSounding() for, just
            // below.
            pin.alarmSounding = app.alarmSounding();
            pin.blanked = !rendering;
            pin.onUsb = stdio_usb_connected();
            pin.battery = batteryReading;
            // The BOOT snapshot, deliberately -- see bootFloorRawMv's
            // declaration. A floor learned during this descent must not arm
            // the cutoff that would end it.
            pin.armedFloorRawMv = bootFloorRawMv;
            // The gain `batteryReading.milliVolts` was ACTUALLY CORRECTED
            // WITH, a few hundred lines above, which is sessionSettings' --
            // never `eff`'s. The cutoff and the reading it is compared against
            // have to be derived from the same gain or a previewed CAL change
            // moves one and not the other, and the CAL wheel accelerates. `eff`
            // is still what update() reads for offAfterS and the rest of the
            // live preview; only the cutoff derivation is pinned here.
            pin.armedGainPermille = sessionSettings.batCalPermille;
            powerDecision = powerPolicy.update(pin, eff);
        }

        switch (powerDecision.action) {
            case h0::PowerAction::Wake:
            case h0::PowerAction::PromptHold:
            case h0::PowerAction::PromptTimerRunning:
            case h0::PowerAction::UsbCannotPowerOff:
                // Force the backlight ladder back to ACTIVE. Without this, a
                // hold that starts after the screen has blanked would run the
                // whole two-second gesture -- bar filling, threshold reached --
                // against a dark panel, and UsbCannotPowerOff would never be
                // seen at all if it was reached from BLANK.
                lastInteractionUs = now;
                break;
            case h0::PowerAction::PromptRelease:
                lastInteractionUs = now;
                // Design section 5: the flash commit happens at the
                // threshold, not at the eventual release -- so settings
                // survive a brownout between the two. `sessionSettings`, not
                // `eff`: `eff` is the settings menu's live, uncommitted
                // preview while the menu is open, and its contract is that
                // only the swipe commits and leaving flat cancels. Guarded
                // so a hold that keeps returning PromptRelease every frame
                // (this is a level, not an edge) commits exactly once.
                if (!settingsCommittedForShutdown) {
                    applySettings(sessionSettings);
                    settingsStore.commit(sessionSettings);
                    settingsCommittedForShutdown = true;
                }
                break;
            case h0::PowerAction::PowerOff:
                // Flash first, blocking, THEN the sequence that drops the
                // latch. settingsStore is a main() local; board::PowerButton
                // has no business knowing it exists, so the commit lives here
                // rather than inside shutdown(). There is no power-fail
                // window: GPIO15 is still driven high through this entire
                // call, and ~10 uF of bulk on 3V3 holds the rail about 50 us
                // even after it drops -- three orders of magnitude short of a
                // 3 ms page program.
                //
                // Usually already committed above, at the first PromptRelease
                // frame -- this is a fallback for the automatic idle and
                // low-battery routes, which jump straight to PowerOff with no
                // PromptRelease stage of their own.
                if (!settingsCommittedForShutdown) {
                    applySettings(sessionSettings);
                    settingsStore.commit(sessionSettings);
                }
                powerButton.shutdown(lcd, touch, imu, buzzer);
                break;
            case h0::PowerAction::None:
                // No power-off sequence in flight, so the next one -- if
                // any -- must be free to commit again.
                settingsCommittedForShutdown = false;
                break;
        }

        const uint64_t idleUs = now - lastInteractionUs;
        // `eff`, not settingsStore.settings() -- see Task 12 Step 2. Dragging
        // BRIGHT or DIM TO must move the backlight as the finger moves, which is
        // the whole justification for the commit model.
        const h0::BacklightState want =
            h0::backlightFor(eff, idleUs, app.alarmSounding());

        if (want.level != backlight) {
            backlight = want.level;
            // pwm_set_gpio_level(25, 0) IS sufficient here: this state enters no
            // POWMAN state, so section 8.6 item 1's pad-isolation warning does
            // not apply. It becomes binding the day issue #8 lands, at which
            // point GPIO25 must be taken back as SIO and driven low.
            if (!lcd.setBacklight(backlight)) printf("backlight set failed\n");
        }

        if (want.render != rendering) {
            rendering = want.render;
            face.setTickHz(rendering ? 30 : 8);
            // The tracker's shadow has no idea what happened while it was not
            // being fed, so the first frame after a wake is a full one.
            if (rendering) tracker.markAllDirty();
        }

        // All four conditions, per the design's section 5.3: not flat, no timer,
        // no alarm, and the screen ALREADY DARK. The last is not politeness --
        // ~400 ms of masked interrupts drops the ~1 ms edge-latched touch pulse
        // outright, so this has to happen when nobody is touching the glass.
        if (settingsStore.needsErase() && !app.isFlat() &&
            !settingsUi.isOpen() && !app.timer().isRunning() &&
            !app.alarmSounding() && want.level == 0) {
            settingsStore.runDeferredErase();
        }

        if (rendering) {
            // The prompt is a dedicated full-screen face, never an overlay --
            // at one bit, text over the falling sand is unreadable -- so it
            // takes priority over every other face rather than drawing atop
            // one.
            if (powerDecision.action == h0::PowerAction::PromptHold ||
                powerDecision.action == h0::PowerAction::PromptRelease ||
                powerDecision.action == h0::PowerAction::PromptTimerRunning ||
                powerDecision.action == h0::PowerAction::UsbCannotPowerOff) {
                h0::PowerFace::renderAt(fb, powerDecision.action, powerDecision.progress);
            } else if (settingsUi.isOpen()) {
                h0::SettingsFace::renderAt(fb, settingsUi, batteryReading);
            } else if (app.settingPosture()) {
                // The dial replaces the face in the setting posture -- flat OR
                // upright-and-Idle: a rotary control with no visible ring is
                // undiscoverable, and the timer is not counting anyway.
                // The DURATION, not the remaining time. The picker edits
                // duration, so showing remaining meant that laying down a
                // part-run timer displayed one number and edited another: run
                // 5:00 down to 3:00, nudge minutes once, and it jumps to 6:00.
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
        }

        watchdog_update();

        // Render, tracker.update and pushDirty move as one unit: updating the
        // tracker's shadow without pushing (or vice versa) is what makes it
        // diverge from the panel. Guarded on the same `rendering` value the
        // render call above used, so the two either both run or both don't.
        if (rendering) {
            // Rotate the FINISHED frame rather than teaching the simulation,
            // the picker, the font and the touch map that the device can be
            // turned over. Everything upstream keeps working in a content
            // space where up is up.
            if (inverted) h0::rotate180(fb);
            lcd.pushDirty(fb, tracker.update(fb));
            lcd.waitIdle();
        }

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
