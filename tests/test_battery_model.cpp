#include "doctest.h"

#include "power/battery_model.hpp"

TEST_CASE("buckets follow the measured curve") {
    // power-and-time.md section 7.5. Five buckets, never a percentage: 3.74 V
    // to 3.87 V covers 20% to 60% SoC, and a calibrated reading is still
    // +/-20% SoC there.
    CHECK(h0::bucketFor(4180) == h0::Bucket::Full);
    CHECK(h0::bucketFor(3950) == h0::Bucket::Good);
    CHECK(h0::bucketFor(3800) == h0::Bucket::Half);
    CHECK(h0::bucketFor(3700) == h0::Bucket::Low);
    CHECK(h0::bucketFor(3500) == h0::Bucket::Critical);
}

TEST_CASE("the bucket boundaries are the ones the table actually states") {
    // The table maps 3.74 V to Low and 3.77 V to Half. Getting this off by one
    // row is invisible in a spot check and wrong for 30 mV of the curve.
    CHECK(h0::bucketFor(3769) == h0::Bucket::Low);
    CHECK(h0::bucketFor(3770) == h0::Bucket::Half);
    CHECK(h0::bucketFor(3869) == h0::Bucket::Half);
    CHECK(h0::bucketFor(3870) == h0::Bucket::Good);
    CHECK(h0::bucketFor(4059) == h0::Bucket::Good);
    CHECK(h0::bucketFor(4060) == h0::Bucket::Full);
}

TEST_CASE("buckets are monotonic in voltage") {
    // A gauge that ever reads higher as the cell drains is worse than no gauge.
    int last = -1;
    for (uint16_t mv = 3300; mv <= 4250; mv += 5) {
        const int b = static_cast<int>(h0::bucketFor(mv));
        CHECK(b >= last);
        last = b;
    }
}

TEST_CASE("calibration is a straight gain correction") {
    CHECK(h0::applyCal(3900, 1000) == 3900);
    CHECK(h0::applyCal(3900, 1100) == 4290);
    CHECK(h0::applyCal(4000, 850) == 3400);
}

TEST_CASE("the IIR seeds from the first reading rather than from zero") {
    // Seeded at zero the row reads 0.00 V and climbs for ~45 s after every
    // boot, which looks exactly like a flat battery.
    h0::BatteryFilter f;
    CHECK_FALSE(f.valid());
    f.push(3900);
    CHECK(f.valid());
    CHECK(f.milliVolts() == 3900);

    for (int i = 0; i < 100; ++i) f.push(3700);
    CHECK(f.milliVolts() > 3690);
    CHECK(f.milliVolts() < 3720);
}

TEST_CASE("charging is inferred above the cell's own ceiling") {
    // No fuel gauge, and ETA6096 STAT is unconnected -- but nothing except a
    // charger can hold the terminal above 4.22 V.
    CHECK(h0::isCharging(4250));
    CHECK_FALSE(h0::isCharging(4150));
}
