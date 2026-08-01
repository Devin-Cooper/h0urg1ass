#include "doctest.h"

#include "power/battery_floor.hpp"

TEST_CASE("nothing above the tracking threshold is ever recorded") {
    // Keeping the whole normal operating range out of scope is what keeps
    // flash writes off it entirely.
    CHECK(h0::BatteryFloor::update(3800, 3800, 0) == 0);
    CHECK(h0::BatteryFloor::update(3701, 3701, 0) == 0);
    CHECK(h0::BatteryFloor::update(3699, 3699, 0) == 3699);
}

TEST_CASE("the first low below the threshold is recorded immediately") {
    // storedRaw == 0 means never learned, and holding out for a 50 mV
    // improvement on nothing would never record anything at all.
    CHECK(h0::BatteryFloor::update(3600, 3600, 0) == 3600);
}

TEST_CASE("a new low must beat the stored floor by the step to be worth a write") {
    // THE WRITE BUDGET, and it is the reason this approach is affordable.
    CHECK(h0::BatteryFloor::update(3599, 3599, 3600) == 0);    // 1 mV: not worth it
    CHECK(h0::BatteryFloor::update(3551, 3551, 3600) == 0);    // 49 mV: still not
    CHECK(h0::BatteryFloor::update(3550, 3550, 3600) == 3550); // 50 mV: yes
}

TEST_CASE("a higher reading never raises the stored floor") {
    // The floor is a lifetime minimum. If it could rise, every charge cycle
    // would rewrite it and the cutoff would drift upward forever.
    CHECK(h0::BatteryFloor::update(3650, 3650, 3600) == 0);
}

TEST_CASE("a whole descent costs fewer than ten writes") {
    // Counted, not asserted in the abstract. SettingsStore's wear note assumes
    // one page program per settings SESSION; a burst of ten sits inside that,
    // but a burst of forty would turn the deferred 400 ms sector erase into a
    // routine event. A test that only checked the final floor would pass just
    // as well with a write on every sample.
    uint16_t stored = 0;
    int writes = 0;
    for (uint16_t mv = 4100; mv >= 3380; mv = static_cast<uint16_t>(mv - 1)) {
        const uint16_t next = h0::BatteryFloor::update(mv, mv, stored);
        if (next != 0) {
            stored = next;
            ++writes;
        }
    }
    // Measured: writes at 3699, 3649, 3599, 3549, 3499, 3449, 3399 = 7, and the
    // final 3380 is only 19 mV below 3399 so it does not qualify. At kStepMv 25
    // -- which is what the spec first said -- the same descent costs 13, which
    // is where "under ten" came from and why the constant moved.
    CAPTURE(writes);
    CAPTURE(stored);
    CHECK(writes == 7);
    CHECK(stored == 3399);
}

TEST_CASE("no floor means no cutoff, which is what leaves it disarmed") {
    CHECK(h0::BatteryFloor::cutoffMv(0, 1000) == 0);
}

TEST_CASE("the cutoff is the floor plus the margin, at unity gain") {
    CHECK(h0::BatteryFloor::cutoffMv(3380, 1000) == 3630);
}

TEST_CASE("the cutoff moves with the gain, because the floor is stored raw") {
    // Storing raw is what lets a floor learned before calibration stay correct
    // after it. At 1052 permille, 3380 raw is 3555 corrected, so 3805 -- which
    // then clamps to the ceiling.
    CHECK(h0::BatteryFloor::cutoffMv(3380, 1052) == h0::BatteryFloor::kCutoffMaxMv);
    CHECK(h0::BatteryFloor::cutoffMv(3200, 1052) == 3616);
}

TEST_CASE("the cutoff clamps at both ends") {
    // Asymmetric consequences: too high only wastes capacity, too low
    // over-discharges the cell. Both ends still have to hold.
    CHECK(h0::BatteryFloor::cutoffMv(2000, 1000) == h0::BatteryFloor::kCutoffMinMv);
    CHECK(h0::BatteryFloor::cutoffMv(4200, 1000) == h0::BatteryFloor::kCutoffMaxMv);
}
