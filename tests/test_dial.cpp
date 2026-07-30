#include "doctest.h"

#include "input/dial.hpp"

#include <cmath>

using h0::Dial;

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Drag along an arc at a fixed radius, in `samples` steps, summing the reported
/// movement. Mirrors how a real finger arrives: many small samples, not one big
/// jump.
int dragArc(Dial& d, float fromDeg, float toDeg, float radius, int samples,
            const Dial::Config& cfg = Dial::Config{}) {
    int total = 0;
    for (int i = 0; i <= samples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(samples);
        const float a = (fromDeg + (toDeg - fromDeg) * u) * kPi / 180.0f;
        const int16_t x = static_cast<int16_t>(cfg.cx + radius * std::cos(a));
        const int16_t y = static_cast<int16_t>(cfg.cy + radius * std::sin(a));
        total += d.update(true, x, y);
    }
    return total;
}

} // namespace

TEST_CASE("a full clockwise turn is one revolution of steps") {
    Dial d;
    const int steps = dragArc(d, 0.0f, 360.0f, 80.0f, 360);
    CHECK(steps == doctest::Approx(60).epsilon(0.05));
}

TEST_CASE("anticlockwise is negative") {
    Dial d;
    const int steps = dragArc(d, 0.0f, -360.0f, 80.0f, 360);
    CHECK(steps == doctest::Approx(-60).epsilon(0.05));
}

TEST_CASE("crossing the branch cut does not reverse the dial") {
    // atan2 jumps from +pi to -pi at the left of the dial. Unwrapped, dragging
    // smoothly across that point reads as a near-full turn backwards -- which on
    // a timer would subtract an hour mid-gesture.
    Dial d;
    const int steps = dragArc(d, 170.0f, 190.0f, 80.0f, 40);
    CHECK(steps >= 2);
    CHECK(steps <= 4); // 20 degrees is 3.33 steps
}

TEST_CASE("many small samples equal one large sweep") {
    // The residual must carry between calls, or a slow drag loses movement to
    // truncation on every sample and travels visibly less than a fast one.
    Dial slow, fast;
    const int slowSteps = dragArc(slow, 0.0f, 180.0f, 80.0f, 2000);
    const int fastSteps = dragArc(fast, 0.0f, 180.0f, 80.0f, 20);
    CHECK(slowSteps == fastSteps);
    CHECK(slowSteps == doctest::Approx(30).epsilon(0.05));
}

TEST_CASE("the first sample of a drag moves nothing") {
    // Otherwise every touch jumps by whatever the angle happens to be.
    Dial d;
    CHECK(d.update(true, 200, 140) == 0);
    CHECK(d.tracking());
}

TEST_CASE("releasing stops tracking, and a new touch does not jump") {
    Dial d;
    dragArc(d, 0.0f, 90.0f, 80.0f, 90);
    CHECK(d.update(false, 0, 0) == 0);
    CHECK_FALSE(d.tracking());

    // Touching down somewhere far away must be a fresh reference, not a delta
    // measured against the old angle.
    CHECK(d.update(true, 120, 60) == 0);
}

TEST_CASE("the centre dead zone is inert") {
    // At two pixels from centre a single pixel of travel is 30 degrees, so a
    // resting thumb would spin the dial.
    Dial d;
    int total = 0;
    for (int i = 0; i < 200; ++i) {
        const int16_t x = static_cast<int16_t>(120 + (i % 5) - 2);
        const int16_t y = static_cast<int16_t>(140 + (i % 3) - 1);
        total += d.update(true, x, y);
    }
    CHECK(total == 0);
    CHECK_FALSE(d.tracking());
}

TEST_CASE("dragging through the centre restarts rather than spinning") {
    Dial d;
    dragArc(d, 0.0f, 45.0f, 80.0f, 45);
    REQUIRE(d.tracking());

    d.update(true, 120, 140); // through the middle
    CHECK_FALSE(d.tracking());

    // Coming out the other side is a new drag, and its first sample is silent.
    CHECK(d.update(true, 40, 140) == 0);
}

TEST_CASE("radius selects coarse and fine") {
    Dial::Config cfg;
    Dial d(cfg);

    dragArc(d, 0.0f, 30.0f, 90.0f, 30, cfg); // outside coarseRadius 72
    CHECK(d.coarse());

    d.update(false, 0, 0);
    dragArc(d, 0.0f, 30.0f, 45.0f, 30, cfg); // between deadZone and coarseRadius
    CHECK_FALSE(d.coarse());
}

TEST_CASE("steps scale with the configured resolution") {
    Dial::Config cfg;
    cfg.stepsPerTurn = 12;
    Dial d(cfg);
    CHECK(dragArc(d, 0.0f, 360.0f, 80.0f, 360, cfg) == doctest::Approx(12).epsilon(0.1));
}

TEST_CASE("several turns accumulate without drift") {
    // A long duration needs several revolutions, so error must not compound.
    Dial d;
    const int steps = dragArc(d, 0.0f, 360.0f * 5.0f, 80.0f, 1800);
    CHECK(steps == doctest::Approx(300).epsilon(0.02));
}

TEST_CASE("a jitter-only touch produces no net movement") {
    // A finger resting on the rim wobbles by a pixel or two. Over a long hold
    // that must not integrate into a drift in either direction.
    Dial d;
    d.update(true, 200, 140);
    int total = 0;
    for (int i = 0; i < 500; ++i) {
        const int16_t x = static_cast<int16_t>(200 + (i % 3) - 1);
        const int16_t y = static_cast<int16_t>(140 + ((i / 3) % 3) - 1);
        total += d.update(true, x, y);
    }
    CHECK(total >= -1);
    CHECK(total <= 1);
}
