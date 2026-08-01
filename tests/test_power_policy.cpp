#include "doctest.h"

#include "power/power_policy.hpp"

using h0::PowerAction;
using h0::PowerInput;
using h0::PowerPolicy;
using h0::Settings;

namespace {

constexpr uint64_t MS = 1'000ull;
constexpr uint64_t SEC = 1'000'000ull;

/// A device on battery, awake, nothing running, healthy cell.
///
/// No armed floor: that is the shipping state, and it is what keeps the
/// low-battery route out of every test that is not about it.
PowerInput idleInput(uint64_t now) {
    PowerInput in;
    in.now = now;
    in.buttonDown = false;
    in.idleUs = 0;
    in.timerRunning = false;
    in.blanked = false;
    in.onUsb = false;
    in.battery.valid = true;
    in.battery.milliVolts = 3900;
    return in;
}

/// Hold the button from `t0` for `holdMs`, feeding one sample every 33 ms, and
/// return the last decision seen while still held.
h0::PowerDecision holdFor(PowerPolicy& p, Settings& s, uint64_t t0, uint64_t holdMs,
                          bool timerRunning = false, bool onUsb = false) {
    // Establish that the policy has already seen the button released once,
    // as every real one has by the time a deliberate hold begins -- see
    // PowerPolicy::seenRelease_. Without this, holdFor's own first sample
    // (buttonDown already true) would be mistaken for a boot-stale press and
    // ignored outright, which is a different thing than what these tests
    // exist to exercise.
    p.update(idleInput(t0), s);

    h0::PowerDecision d{};
    for (uint64_t t = t0; t <= t0 + holdMs * MS; t += 33 * MS) {
        PowerInput in = idleInput(t);
        in.buttonDown = true;
        in.timerRunning = timerRunning;
        in.onUsb = onUsb;
        d = p.update(in, s);
    }
    return d;
}

} // namespace

TEST_CASE("a tap wakes a blanked device and does nothing otherwise") {
    PowerPolicy p;
    Settings s = h0::kDefaults;
    p.update(idleInput(0), s); // seen released once, as by the time of a real press

    PowerInput in = idleInput(1 * SEC);
    in.blanked = true;
    in.buttonDown = true;
    CHECK(p.update(in, s).action == PowerAction::Wake);

    PowerPolicy q;
    q.update(idleInput(0), s);
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
    p.update(idleInput(0), s); // seen released once, as by the time of a real press
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

TEST_CASE("an invalid battery reading never powers the device off") {
    PowerPolicy p;
    Settings s = h0::kDefaults;
    PowerInput in = idleInput(10 * SEC);
    in.battery.valid = false;
    in.battery.milliVolts = 0;
    CHECK(p.update(in, s).action == PowerAction::None);
}

TEST_CASE("on USB the automatic routes stay silent, and the button path says so") {
    // D4 keeps VSYS alive regardless of GPIO15, so neither automatic route
    // can ever actually act while on USB. A periodic "cannot power off" would
    // just be main.cpp treating it as interaction and relighting the screen
    // every offAfterS forever, for a message the user never asked to see.
    Settings s = h0::kDefaults;

    PowerPolicy p;
    PowerInput in = idleInput(10 * SEC);
    in.onUsb = true;
    in.idleUs = 301 * SEC;
    CHECK(p.update(in, s).action == PowerAction::None);

    PowerPolicy q;
    PowerInput flat = idleInput(10 * SEC);
    flat.onUsb = true;
    // An ARMED floor, or this assertion proves nothing: with the shipping
    // default of none, cutoffMv() returns 0, the route is disabled outright,
    // and None would come back whatever the onUsb branch did. 3380 raw at
    // unity gain arms at 3630, so 3400 is genuinely under it.
    flat.armedFloorRawMv = 3380;
    flat.battery.milliVolts = 3400;
    CHECK(q.update(flat, s).action == PowerAction::None);

    // The button path is the opposite: UsbCannotPowerOff must show for the
    // WHOLE hold past the threshold, not just the release sample, or the
    // panel spends the hold claiming "release to power off" -- untrue on
    // USB -- and only admits the truth for one ~25 ms frame.
    PowerPolicy r;
    Settings s2 = h0::kDefaults;
    CHECK(holdFor(r, s2, 1 * SEC, 2100, false, /*onUsb=*/true).action ==
          PowerAction::UsbCannotPowerOff);

    PowerInput up = idleInput(1 * SEC + 2200 * MS);
    up.onUsb = true;
    up.buttonDown = false;
    CHECK(r.update(up, s2).action == PowerAction::UsbCannotPowerOff);
}

TEST_CASE("a timer that just expired does not lose the idle race to its own run") {
    // isRunning() is state_ == Running, so it goes false the instant expiry
    // starts the alarm -- but idleUs has been accumulating for the entire,
    // quietly-counting run, because nothing about a running timer resets it.
    // Reproduces main.cpp's exact frame pair: a timer running with idleUs
    // already past offAfterS, then the one frame where timerRunning flips
    // false and alarmSounding flips true. Without gating on alarmSounding
    // too, that second frame reads as "not running, idle past the timeout"
    // and powers the device off before the alarm makes a sound -- silencing
    // any timer of offAfterS (default 300 s) or longer.
    PowerPolicy p;
    Settings s = h0::kDefaults; // offAfterS = 300

    PowerInput running = idleInput(500 * SEC);
    running.timerRunning = true;
    running.idleUs = 301 * SEC; // already past offAfterS, timer still running
    CHECK(p.update(running, s).action == PowerAction::None);

    PowerInput expiring = idleInput(500 * SEC + 33 * MS);
    expiring.timerRunning = false;  // isRunning() just went false...
    expiring.alarmSounding = true;  // ...because the alarm just started
    expiring.idleUs = 301 * SEC + 33 * MS;
    CHECK(p.update(expiring, s).action == PowerAction::None);
}

TEST_CASE("a press already down when the policy first runs cannot arm a power-off") {
    // PowerButton::begin() runs seconds after the press that turned the
    // device on -- main()'s idle window, LCD init and the sand probe all
    // land first, with the panel dark the whole time. Without seenRelease_,
    // that stale press starts accumulating hold time from the first frame
    // the policy ever sees it, and arms exactly like a deliberate hold.
    PowerPolicy p;
    Settings s = h0::kDefaults;

    // Held continuously from t=0, as if the press began before boot. Sweep
    // well past both thresholds (kConfirmUs is the longer of the two) and
    // confirm the button path never fires at all.
    for (uint64_t t = 0; t <= PowerPolicy::kConfirmUs + 1 * SEC; t += 33 * MS) {
        PowerInput in = idleInput(t);
        in.buttonDown = true;
        const h0::PowerDecision d = p.update(in, s);
        CHECK(d.action == PowerAction::None);
    }

    // Releasing the stale press must not power the device off either --
    // armed_ was never set, because the hold was ignored throughout.
    PowerInput up = idleInput(PowerPolicy::kConfirmUs + 1200 * MS);
    up.buttonDown = false;
    CHECK(p.update(up, s).action == PowerAction::None);

    // The button must work normally afterwards: this is a one-time gate on
    // the boot-stale press, not a policy that has stopped responding.
    CHECK(holdFor(p, s, PowerPolicy::kConfirmUs + 2 * SEC, 2100).action ==
          PowerAction::PromptRelease);
    PowerInput up2 = idleInput(PowerPolicy::kConfirmUs + 2 * SEC + 2200 * MS);
    up2.buttonDown = false;
    CHECK(p.update(up2, s).action == PowerAction::PowerOff);
}

TEST_CASE("a press already down when the policy first runs also suppresses Wake") {
    // The blanked-screen Wake path is part of the button path too, and the
    // fix's contract is "ignored entirely" -- not "arming suppressed but
    // Wake still fires".
    PowerPolicy p;
    Settings s = h0::kDefaults;

    PowerInput in = idleInput(1 * SEC);
    in.buttonDown = true;
    in.blanked = true;
    CHECK(p.update(in, s).action == PowerAction::None);
}

TEST_CASE("a flat battery does not power off until a floor has been learned") {
    // The gate the whole floor mechanism exists to control. Before the first
    // run to empty there is no measured floor, so there is nothing to act on
    // and the route stays disabled -- which is exactly today's behaviour.
    h0::PowerPolicy p;
    h0::Settings s;

    h0::PowerInput in;
    in.now = 10'000'000ull;
    in.armedFloorRawMv = 0;
    in.battery.valid = true;
    in.battery.milliVolts = 3000; // far below any plausible cutoff

    CHECK(p.update(in, s).action == h0::PowerAction::None);
}

TEST_CASE("a floor learned during this descent does not arm the cutoff that would end it") {
    // THE LEARNING RUN, and this is the only test that pins it. cutoffMv() is
    // applyCal(floor) + 250 mV, which is above the reading that produced the
    // floor FOR EVERY POSSIBLE FLOOR -- so if the live Settings value armed
    // the route, the first sample below 3700 mV would write a floor and power
    // the device off in the same frame, at ~3.70 V. The descent that the floor
    // learner exists to observe would never reach brownout, and the next boot
    // would arm at the clamped 3750 mV ceiling and never get low enough to
    // learn again.
    h0::PowerPolicy p;
    h0::Settings s;
    s.batFloorRawMv = 3650;   // just learned, this session, still descending
    s.batCalPermille = 1000;

    h0::PowerInput in;
    in.now = 10'000'000ull;
    in.armedFloorRawMv = 0;   // nothing survived a power cycle yet
    in.battery.valid = true;
    in.battery.milliVolts = 3650; // the very reading that produced the floor

    CHECK(p.update(in, s).action == h0::PowerAction::None);

    // And it stays silent all the way down to the brownout it is meant to
    // reach, not merely for the frame the floor landed in.
    for (uint16_t mv = 3650; mv >= 3400; mv = static_cast<uint16_t>(mv - 10)) {
        s.batFloorRawMv = mv;
        in.battery.milliVolts = mv;
        CHECK(p.update(in, s).action == h0::PowerAction::None);
    }
}

TEST_CASE("a floor that survived a power cycle arms the cutoff") {
    // The other half: the boot snapshot, which is what main.cpp passes. Same
    // floor value as the test above, and the opposite outcome -- the ONLY
    // difference being that this one came back out of flash.
    h0::PowerPolicy p;
    h0::Settings s;
    s.batCalPermille = 1000; // cutoff = 3380 + 250 = 3630

    h0::PowerInput in;
    in.now = 10'000'000ull;
    in.armedFloorRawMv = 3380;
    in.battery.valid = true;

    in.battery.milliVolts = 3700;
    CHECK(p.update(in, s).action == h0::PowerAction::None);

    in.battery.milliVolts = 3600;
    CHECK(p.update(in, s).action == h0::PowerAction::PowerOff);
}

TEST_CASE("the armed cutoff still follows the gain the reading was corrected with") {
    // The floor is frozen at boot; the GAIN is not. A gain learned this
    // session moves an already-armed cutoff as soon as it reaches the reading,
    // which is safe -- a better gain can only move a cutoff that exists, never
    // conjure one.
    h0::PowerPolicy p;
    h0::Settings s;

    h0::PowerInput in;
    in.now = 10'000'000ull;
    in.armedFloorRawMv = 3200;
    in.battery.valid = true;
    in.battery.milliVolts = 3500;

    in.armedGainPermille = 1000; // cutoff = 3200 + 250 = 3450
    CHECK(p.update(in, s).action == h0::PowerAction::None);

    h0::PowerPolicy q;
    in.armedGainPermille = 1052; // 3200 raw -> 3366 corrected, cutoff 3616
    CHECK(q.update(in, s).action == h0::PowerAction::PowerOff);
}

TEST_CASE("a previewed gain cannot move the cutoff out from under the reading") {
    // The cutoff and the reading it is compared against must come from the
    // SAME gain. main.cpp corrects battery.milliVolts with the COMMITTED gain
    // but hands update() the settings menu's live preview, so taking the gain
    // from `Settings` here would let a CAL drag lift the cutoff while the
    // reading stayed put -- and the CAL row accelerates, so a single flick
    // sweeps the whole 850..1150 ladder. This device has a floor from a
    // previous cycle and a pack at 3700 mV, ~20% left, nothing running: it
    // must survive the drag.
    h0::PowerPolicy p;
    h0::Settings s;
    s.batCalPermille = h0::kCalMax; // the preview, flicked to the top

    h0::PowerInput in;
    in.now = 10'000'000ull;
    in.armedFloorRawMv = 3380;
    in.armedGainPermille = 1000;  // what the reading was actually corrected with
    in.battery.valid = true;
    in.battery.milliVolts = 3700; // corrected at 1000; the true cutoff is 3630

    CHECK(p.update(in, s).action == h0::PowerAction::None);

    // The pair moving TOGETHER is a different matter, and must still fire:
    // at 1150 the same 3380 raw floor corrects to 3887, so the cutoff clamps
    // to kCutoffMaxMv 3750 and 3700 really is below it.
    h0::PowerPolicy q;
    in.armedGainPermille = h0::kCalMax;
    CHECK(q.update(in, s).action == h0::PowerAction::PowerOff);
}

TEST_CASE("the armed cutoff is silent on USB, like the other automatic routes") {
    h0::PowerPolicy p;
    h0::Settings s;

    h0::PowerInput in;
    in.now = 10'000'000ull;
    in.armedFloorRawMv = 3380;
    in.onUsb = true;
    in.battery.valid = true;
    in.battery.milliVolts = 3000;

    CHECK(p.update(in, s).action == h0::PowerAction::None);
}
