#include "doctest.h"

#include "power/power_policy.hpp"

using h0::PowerAction;
using h0::PowerInput;
using h0::PowerPolicy;
using h0::Settings;

namespace {

constexpr uint64_t MS = 1'000ull;
constexpr uint64_t SEC = 1'000'000ull;

/// A device on battery, awake, nothing running, healthy calibrated cell.
PowerInput idleInput(uint64_t now) {
    PowerInput in;
    in.now = now;
    in.buttonDown = false;
    in.idleUs = 0;
    in.timerRunning = false;
    in.blanked = false;
    in.onUsb = false;
    in.battery.valid = true;
    in.battery.calibrated = true;
    in.battery.milliVolts = 3900;
    return in;
}

/// Hold the button from `t0` for `holdMs`, feeding one sample every 33 ms, and
/// return the last decision seen while still held.
h0::PowerDecision holdFor(PowerPolicy& p, Settings& s, uint64_t t0, uint64_t holdMs,
                          bool timerRunning = false) {
    h0::PowerDecision d{};
    for (uint64_t t = t0; t <= t0 + holdMs * MS; t += 33 * MS) {
        PowerInput in = idleInput(t);
        in.buttonDown = true;
        in.timerRunning = timerRunning;
        d = p.update(in, s);
    }
    return d;
}

} // namespace

TEST_CASE("a tap wakes a blanked device and does nothing otherwise") {
    PowerPolicy p;
    Settings s = h0::kDefaults;

    PowerInput in = idleInput(1 * SEC);
    in.blanked = true;
    in.buttonDown = true;
    CHECK(p.update(in, s).action == PowerAction::Wake);

    PowerPolicy q;
    PowerInput awake = idleInput(1 * SEC);
    awake.buttonDown = true;
    CHECK(q.update(awake, s).action == PowerAction::None);
}

TEST_CASE("releasing before the threshold cancels") {
    // The only cancel the device has. GPIO15 is never dropped, so the release
    // is a no-op and nothing needs undoing.
    PowerPolicy p;
    Settings s = h0::kDefaults;
    holdFor(p, s, 1 * SEC, 1900);

    PowerInput up = idleInput(1 * SEC + 1950 * MS);
    up.buttonDown = false;
    CHECK(p.update(up, s).action == PowerAction::None);
}

TEST_CASE("holding past the threshold asks for the release, and the release powers off") {
    PowerPolicy p;
    Settings s = h0::kDefaults;
    CHECK(holdFor(p, s, 1 * SEC, 2100).action == PowerAction::PromptRelease);

    PowerInput up = idleInput(1 * SEC + 2200 * MS);
    up.buttonDown = false;
    CHECK(p.update(up, s).action == PowerAction::PowerOff);
}

TEST_CASE("a running timer needs a second stage, and letting go declines") {
    // Section 9: entering OFF with a timer running loses the alarm silently,
    // because the RTC loses VDD too. Holding longer is the only confirm a
    // one-button device has.
    PowerPolicy p;
    Settings s = h0::kDefaults;
    CHECK(holdFor(p, s, 1 * SEC, 2100, true).action == PowerAction::PromptTimerRunning);

    PowerInput up = idleInput(1 * SEC + 3900 * MS);
    up.buttonDown = false;
    up.timerRunning = true;
    CHECK(p.update(up, s).action == PowerAction::None);

    PowerPolicy q;
    CHECK(holdFor(q, s, 1 * SEC, 4100, true).action == PowerAction::PromptRelease);
    PowerInput up2 = idleInput(1 * SEC + 4200 * MS);
    up2.buttonDown = false;
    up2.timerRunning = true;
    CHECK(q.update(up2, s).action == PowerAction::PowerOff);
}

TEST_CASE("the hold progress is monotonic and fills exactly at the threshold") {
    PowerPolicy p;
    Settings s = h0::kDefaults;
    int last = -1;
    bool sawFull = false;
    // 33 ms steps never land exactly on a 2000 ms boundary (2000 / 33 is not
    // an integer): the sample just before the threshold sits at 1980 ms held,
    // and the next one -- the one that actually crosses kHoldUs -- lands at
    // 2013 ms. The bound has to admit that sample or the crossing this test
    // exists to observe can never happen.
    for (uint64_t t = 1 * SEC; t <= 1 * SEC + 2013 * MS; t += 33 * MS) {
        PowerInput in = idleInput(t);
        in.buttonDown = true;
        const h0::PowerDecision d = p.update(in, s);
        if (d.action == PowerAction::PromptHold) {
            CHECK(d.progress >= last);
            last = d.progress;
        }
        if (d.action == PowerAction::PromptRelease) sawFull = true;
    }
    CHECK(last >= 0);
    CHECK(sawFull);
}

TEST_CASE("idle powers off, but never while a timer is running") {
    PowerPolicy p;
    Settings s = h0::kDefaults; // offAfterS = 300

    PowerInput in = idleInput(10 * SEC);
    in.idleUs = 301 * SEC;
    CHECK(p.update(in, s).action == PowerAction::PowerOff);

    PowerPolicy q;
    PowerInput busy = idleInput(10 * SEC);
    busy.idleUs = 3600 * SEC;
    busy.timerRunning = true;
    CHECK(q.update(busy, s).action == PowerAction::None);
}

TEST_CASE("OFF AT set to NEVER never fires") {
    PowerPolicy p;
    Settings s = h0::kDefaults;
    s.offAfterS = 0;
    PowerInput in = idleInput(10 * SEC);
    in.idleUs = 86400 * SEC;
    CHECK(p.update(in, s).action == PowerAction::None);
}

TEST_CASE("the low-battery cutoff fires only on a calibrated device") {
    // Uncalibrated the gain error is +/-9%, or +/-0.35 V, so a reading of
    // 3.45 V could be a true 3.16 V or a true 3.79 V. The battery row already
    // refuses to show a number in that state; the cutoff refuses to act on one.
    Settings s = h0::kDefaults;

    PowerPolicy p;
    PowerInput flat = idleInput(10 * SEC);
    flat.battery.milliVolts = 3400;
    CHECK(p.update(flat, s).action == PowerAction::PowerOff);

    for (uint16_t mv = 3000; mv <= 3450; mv += 50) {
        PowerPolicy q;
        PowerInput uncal = idleInput(10 * SEC);
        uncal.battery.milliVolts = mv;
        uncal.battery.calibrated = false;
        CHECK(q.update(uncal, s).action == PowerAction::None);
    }
}

TEST_CASE("an invalid battery reading never powers the device off") {
    PowerPolicy p;
    Settings s = h0::kDefaults;
    PowerInput in = idleInput(10 * SEC);
    in.battery.valid = false;
    in.battery.milliVolts = 0;
    CHECK(p.update(in, s).action == PowerAction::None);
}

TEST_CASE("on USB every power-off route says so instead") {
    // D4 keeps VSYS alive regardless of GPIO15, so dropping the latch does
    // nothing. A power-off that visibly fails reads as a fault.
    Settings s = h0::kDefaults;

    PowerPolicy p;
    PowerInput in = idleInput(10 * SEC);
    in.onUsb = true;
    in.idleUs = 301 * SEC;
    CHECK(p.update(in, s).action == PowerAction::UsbCannotPowerOff);

    PowerPolicy q;
    PowerInput flat = idleInput(10 * SEC);
    flat.onUsb = true;
    flat.battery.milliVolts = 3400;
    CHECK(q.update(flat, s).action == PowerAction::UsbCannotPowerOff);

    PowerPolicy r;
    Settings s2 = h0::kDefaults;
    PowerInput up = idleInput(1 * SEC + 2200 * MS);
    up.onUsb = true;
    holdFor(r, s2, 1 * SEC, 2100);
    up.buttonDown = false;
    CHECK(r.update(up, s2).action == PowerAction::UsbCannotPowerOff);
}
