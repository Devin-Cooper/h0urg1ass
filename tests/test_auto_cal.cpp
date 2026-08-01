#include "doctest.h"

#include "power/auto_cal.hpp"

namespace {

/// Feed `n` samples of the same value, returning the last non-zero result.
uint16_t hold(h0::AutoCal& c, uint16_t mv, int n) {
    uint16_t got = 0;
    for (int i = 0; i < n; ++i) {
        const uint16_t r = c.push(mv);
        if (r != 0) got = r;
    }
    return got;
}

/// A charge cycle: settle low, ramp up, then hold at `plateau`.
uint16_t chargeTo(h0::AutoCal& c, uint16_t plateau) {
    hold(c, 3600, 5);
    for (uint16_t mv = 3600; mv < plateau; mv = static_cast<uint16_t>(mv + 20)) c.push(mv);
    return hold(c, plateau, h0::AutoCal::kFlatSamples + 2);
}

} // namespace

TEST_CASE("a charge plateau yields the gain that makes it read the CV setpoint") {
    // The whole mechanism in one line: the charger holds the terminal at
    // 4210 mV, so whatever the ADC reports there IS the gain error.
    // 1000 * 4210 / 4000 = 1052 (integer).
    h0::AutoCal c;
    CHECK(chargeTo(c, 4000) == 1052);
}

TEST_CASE("a discharge never calibrates") {
    // Only a charger can raise a cell's terminal voltage. Without that rise
    // there is no reason to believe any flat stretch is the CV plateau.
    h0::AutoCal c;
    uint16_t got = 0;
    for (uint16_t mv = 4100; mv > 3400; mv = static_cast<uint16_t>(mv - 1)) {
        const uint16_t r = c.push(mv);
        if (r != 0) got = r;
    }
    CHECK(got == 0);
    CHECK_FALSE(c.charging());
}

TEST_CASE("a plateau with no preceding rise never calibrates") {
    // THE PACK-DISCONNECTED CASE, and the only test that pins the rise
    // requirement. With the charger driving an open circuit the reading jumps
    // straight to CV and sits there; without this test the rise check could be
    // deleted and every other test here would still pass.
    h0::AutoCal c;
    CHECK(hold(c, 4200, h0::AutoCal::kFlatSamples + 50) == 0);
}

TEST_CASE("a plateau implying an out-of-range gain is refused") {
    // A board this far out has a divider or an LDO out of spec. Storing the
    // clamped value would call it calibrated when it is not.
    h0::AutoCal lo, hi;
    CHECK(chargeTo(lo, 3000) == 0); // implies 1403, over kCalMax
    CHECK(chargeTo(hi, 5000) == 0); // implies 842, under kCalMin
}

TEST_CASE("a plateau interrupted before the window never calibrates") {
    h0::AutoCal c;
    hold(c, 3600, 5);
    for (uint16_t mv = 3600; mv < 4000; mv = static_cast<uint16_t>(mv + 20)) c.push(mv);
    CHECK(hold(c, 4000, h0::AutoCal::kFlatSamples - 10) == 0);
    c.push(3950); // a step outside the flatness band restarts the window
    CHECK(hold(c, 4000, 20) == 0);
}

TEST_CASE("one charge session anchors once, however long it is held") {
    // Otherwise a device left plugged in re-emits the same gain every second
    // and the caller writes flash forever.
    h0::AutoCal c;
    CHECK(chargeTo(c, 4000) == 1052);
    CHECK(hold(c, 4000, h0::AutoCal::kFlatSamples + 100) == 0);
}

TEST_CASE("charging follows the rise and clears when the voltage falls away") {
    h0::AutoCal c;
    hold(c, 3600, 5);
    CHECK_FALSE(c.charging());
    for (uint16_t mv = 3600; mv <= 4000; mv = static_cast<uint16_t>(mv + 20)) c.push(mv);
    CHECK(c.charging());
    hold(c, 3900, 3); // 100 mV below the peak: unplugged, or charge terminated
    CHECK_FALSE(c.charging());
}

TEST_CASE("a second charge session can anchor again") {
    // The charger restarts 160 mV below EOC, so this is a real cycle, not a
    // hypothetical. Re-anchoring is what lets the gain track ageing.
    h0::AutoCal c;
    CHECK(chargeTo(c, 4000) == 1052);
    hold(c, 3800, 3); // falls away: session over
    CHECK(chargeTo(c, 4010) == 1049);
}

TEST_CASE("shouldStore applies a deadband so a stable board stops writing") {
    CHECK(h0::AutoCal::shouldStore(1052, 1000, true));
    CHECK_FALSE(h0::AutoCal::shouldStore(1052, 1052, true));
    CHECK_FALSE(h0::AutoCal::shouldStore(1052, 1050, true)); // within kCalDeadband
    CHECK(h0::AutoCal::shouldStore(1052, 1040, true));
}

TEST_CASE("shouldStore refuses when automatic calibration has been switched off") {
    // MANUAL WINS, and this is the only test that pins it. A meter beats the
    // charger's +/-0.95%, so a deliberate hand-calibration must not be silently
    // overwritten -- and if this lived in board/battery.cpp instead it would be
    // untestable, because no board code is compiled into the host suite.
    CHECK_FALSE(h0::AutoCal::shouldStore(1052, 1000, false));
}
