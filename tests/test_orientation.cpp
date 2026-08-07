#include "doctest.h"

#include "input/orientation.hpp"

#include <cmath>
#include <vector>

using h0::GravityFilter;
using h0::MotionEvent;
using h0::Orientation;
using h0::OrientationTracker;
using h0::Vec3;

namespace {

constexpr uint64_t MS = 1'000ull;
constexpr uint64_t SAMPLE_US = 10'000ull; // 100 Hz

// Postures, as gravity directions in the device frame.
const Vec3 UPRIGHT{0.0f, 1.0f, 0.0f};
const Vec3 INVERTED{0.0f, -1.0f, 0.0f};
const Vec3 FLAT{0.0f, 0.0f, -1.0f};
const Vec3 FACEDOWN{0.0f, 0.0f, 1.0f};
const Vec3 ON_EDGE{1.0f, 0.0f, 0.0f};

/// Feed one posture for a while, collecting every event it produces.
std::vector<MotionEvent> hold(OrientationTracker& t, const Vec3& g, uint64_t ms, uint64_t& clock) {
    std::vector<MotionEvent> out;
    const uint64_t end = clock + ms * MS;
    while (clock < end) {
        const MotionEvent e = t.update(g, clock);
        if (e != MotionEvent::None) out.push_back(e);
        clock += SAMPLE_US;
    }
    return out;
}

/// Rotate smoothly between two postures, as a hand actually would.
std::vector<MotionEvent> sweep(OrientationTracker& t, const Vec3& from, const Vec3& to,
                               uint64_t ms, uint64_t& clock) {
    std::vector<MotionEvent> out;
    const uint64_t steps = (ms * MS) / SAMPLE_US;
    for (uint64_t i = 0; i <= steps; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(steps);
        Vec3 g{from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u,
               from.z + (to.z - from.z) * u};
        const float n = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
        if (n > 0.001f) { g.x /= n; g.y /= n; g.z /= n; }
        const MotionEvent e = t.update(g, clock);
        if (e != MotionEvent::None) out.push_back(e);
        clock += SAMPLE_US;
    }
    return out;
}

int count(const std::vector<MotionEvent>& v, MotionEvent want) {
    int n = 0;
    for (MotionEvent e : v) if (e == want) ++n;
    return n;
}

} // namespace

TEST_CASE("postures are classified correctly once settled") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);
    CHECK(t.current() == Orientation::UprightA);
    hold(t, INVERTED, 1000, clock);
    CHECK(t.current() == Orientation::UprightB);
    hold(t, FLAT, 1000, clock);
    CHECK(t.current() == Orientation::FlatBack);
    hold(t, FACEDOWN, 1000, clock);
    CHECK(t.current() == Orientation::FaceDown);
    hold(t, ON_EDGE, 1500, clock);
    CHECK(t.current() == Orientation::OnSide);
}

TEST_CASE("nothing is reported before the dwell elapses") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    // 200 ms of flat is under the 350 ms dwell: no pause.
    const auto e = hold(t, FLAT, 200, clock);
    CHECK(e.empty());
    CHECK(t.current() == Orientation::UprightA);
    CHECK(t.pending() == Orientation::FlatBack);
}

TEST_CASE("laying it down pauses and standing it up resumes") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    auto down = sweep(t, UPRIGHT, FLAT, 400, clock);
    auto settled = hold(t, FLAT, 1000, clock);
    CHECK(count(down, MotionEvent::Settled) + count(settled, MotionEvent::Settled) == 1);

    auto up = sweep(t, FLAT, UPRIGHT, 400, clock);
    auto standing = hold(t, UPRIGHT, 1000, clock);
    CHECK(count(up, MotionEvent::Raised) + count(standing, MotionEvent::Raised) == 1);
}

TEST_CASE("turning it over is a flip") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    // A real turn passes through the side; it does not teleport.
    auto a = sweep(t, UPRIGHT, ON_EDGE, 250, clock);
    auto b = sweep(t, ON_EDGE, INVERTED, 250, clock);
    auto c = hold(t, INVERTED, 1000, clock);

    const int flips = count(a, MotionEvent::Flip) + count(b, MotionEvent::Flip) +
                      count(c, MotionEvent::Flip);
    CHECK(flips == 1);
    CHECK(t.current() == Orientation::UprightB);
}

TEST_CASE("turning it back over flips again") {
    // Each 180 degree turn restarts it, exactly like the real object -- there is
    // no wrong way up.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);
    sweep(t, UPRIGHT, INVERTED, 500, clock);
    auto first = hold(t, INVERTED, 1000, clock);
    sweep(t, INVERTED, UPRIGHT, 500, clock);
    auto second = hold(t, UPRIGHT, 1000, clock);

    CHECK(count(first, MotionEvent::Flip) == 1);
    CHECK(count(second, MotionEvent::Flip) == 1);
}

TEST_CASE("setting it down and picking it up rotated is NOT a flip") {
    // The expensive failure. Reset is the only destructive action in the
    // product, and putting something down and picking it up the other way round
    // is a thing people do without meaning anything by it.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    sweep(t, UPRIGHT, FLAT, 400, clock);
    auto down = hold(t, FLAT, 3000, clock); // genuinely set down
    CHECK(count(down, MotionEvent::Settled) == 1);
    CHECK_FALSE(t.flipArmed()); // settling disarmed it

    sweep(t, FLAT, INVERTED, 400, clock);
    auto up = hold(t, INVERTED, 1000, clock);

    CHECK(count(up, MotionEvent::Flip) == 0); // <-- the property under test
    // It DOES resume, though: standing it up is standing it up, whichever way
    // round. Only the destructive reset is withheld.
    CHECK(count(up, MotionEvent::Raised) == 1);
    CHECK(t.current() == Orientation::UprightB);
}

TEST_CASE("face down silences and does not flip") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    sweep(t, UPRIGHT, FACEDOWN, 400, clock);
    auto e = hold(t, FACEDOWN, 1000, clock);
    CHECK(count(e, MotionEvent::Silence) == 1);
    CHECK(count(e, MotionEvent::Flip) == 0);
    CHECK_FALSE(t.flipArmed());
}

TEST_CASE("walking with it in hand produces no events") {
    // The failure that would make the product unusable: without dwell and
    // hysteresis, footfall and hand tremor cross thresholds continuously and the
    // timer pauses and resumes several times a second.
    OrientationTracker t;
    GravityFilter f;
    uint64_t clock = 0;

    // Settle upright first, through the filter.
    for (int i = 0; i < 200; ++i) {
        t.update(f.push(UPRIGHT), clock);
        clock += SAMPLE_US;
    }
    REQUIRE(t.current() == Orientation::UprightA);

    // 20 s of walking: a 2 Hz stride plus tremor, tilting the device up to ~25
    // degrees off vertical -- vigorous, but never actually laid down.
    int events = 0;
    for (int i = 0; i < 2000; ++i) {
        const float ph = static_cast<float>(i) * 0.0628f;
        Vec3 raw{0.30f * std::sin(ph * 2.0f) + 0.06f * std::sin(ph * 11.0f),
                 0.94f + 0.05f * std::sin(ph * 3.0f),
                 0.28f * std::sin(ph) + 0.06f * std::cos(ph * 13.0f)};
        const float n = std::sqrt(raw.x * raw.x + raw.y * raw.y + raw.z * raw.z);
        raw.x /= n; raw.y /= n; raw.z /= n;
        if (t.update(f.push(raw), clock) != MotionEvent::None) ++events;
        clock += SAMPLE_US;
    }
    CHECK(events == 0);
    CHECK(t.current() == Orientation::UprightA);
}

TEST_CASE("resting on its side is a pause, but only after a full second") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    // 600 ms is past the shared 350 ms dwell and short of kSideDwellUs. If the
    // per-posture dwell is dropped this fires here, which is the mutation this
    // case exists to catch.
    const auto early = hold(t, ON_EDGE, 600, clock);
    CHECK(early.empty());
    CHECK(t.current() == Orientation::UprightA);
    CHECK(t.pending() == Orientation::OnSide);

    const auto late = hold(t, ON_EDGE, 1500, clock);
    CHECK(count(late, MotionEvent::Tipped) == 1);
    CHECK(t.current() == Orientation::OnSide);
}

TEST_CASE("a tilted-back device is Edge, not OnSide, and stays silent") {
    // The reason OnSide had to be split out of Edge at all. |z| = 0.80 is under
    // kFlatEnter so it is not flat, and |y| = 0.60 is under kVerticalEnter so it
    // is not upright -- but |x| = 0, so it is nowhere near its side. This is a
    // device held tilted back in the hand, and it must not pause anything.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    const Vec3 TILTED_BACK{0.0f, 0.600f, -0.800f};
    const auto e = hold(t, TILTED_BACK, 5000, clock);
    CHECK(count(e, MotionEvent::Tipped) == 0);
    CHECK(t.current() == Orientation::Edge);
}

TEST_CASE("a brisk flip does not pause on the way over") {
    // The whole point of kSideDwellUs. A one-second 180 degree turn spends about
    // half of it within 45 degrees of horizontal.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);

    auto a = sweep(t, UPRIGHT, ON_EDGE, 500, clock);
    auto b = sweep(t, ON_EDGE, INVERTED, 500, clock);
    auto c = hold(t, INVERTED, 1000, clock);

    CHECK(count(a, MotionEvent::Tipped) + count(b, MotionEvent::Tipped) +
          count(c, MotionEvent::Tipped) == 0);
    CHECK(count(a, MotionEvent::Flip) + count(b, MotionEvent::Flip) +
          count(c, MotionEvent::Flip) == 1);
}

TEST_CASE("standing it up from its side resumes") {
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);
    hold(t, ON_EDGE, 1500, clock);
    REQUIRE(t.current() == Orientation::OnSide);

    // Righted, not Raised: only lying flat may start an idle timer, and
    // OnSide is not that.
    auto e = hold(t, UPRIGHT, 1000, clock);
    CHECK(count(e, MotionEvent::Righted) == 1);
    CHECK(count(e, MotionEvent::Raised) == 0);
    CHECK(count(e, MotionEvent::Flip) == 0);
}

TEST_CASE("a rested side does NOT disarm the flip") {
    // Deliberate, and the user was asked directly: 180 degrees always means
    // restart, whether or not it paused on the way. The cost is that a paused
    // device resting on its side, picked up in the OTHER upright, resets and
    // loses its elapsed time. Recorded here so nobody later reads it as a bug
    // and "fixes" it.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);
    hold(t, ON_EDGE, 1500, clock);
    REQUIRE(t.current() == Orientation::OnSide);
    CHECK(t.flipArmed());

    auto e = hold(t, INVERTED, 1000, clock);
    CHECK(count(e, MotionEvent::Flip) == 1);
}

TEST_CASE("picking it up from flat through the side still resumes") {
    // main.cpp and app.hpp both record that flat -> edge -> upright never
    // produces an event at all if Edge is left out of both conditions, so the
    // picker stays live in the hand. Edge is in the Righted condition, which
    // closes that -- but as Righted, not Raised: Edge is a transitional
    // wobble, not lying flat, and must not be a launch posture even when it
    // is reached by way of flat.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);
    hold(t, FLAT, 1000, clock);
    REQUIRE(t.current() == Orientation::FlatBack);

    // NOT the {0, 0.600, -0.800} used in the case above. Leaving flat is
    // governed by kFlatExit = 0.70, not kFlatEnter, so |z| = 0.80 would stay
    // FlatBack and this case would silently test nothing. This vector clears
    // all three thresholds: |z| 0.62 < 0.70, |y| 0.60 < 0.70, |x| 0.50 < 0.70.
    const Vec3 HALF_UP{0.50f, 0.60f, -0.62f};
    hold(t, HALF_UP, 1000, clock);
    REQUIRE(t.current() == Orientation::Edge);

    auto e = hold(t, UPRIGHT, 1000, clock);
    CHECK(count(e, MotionEvent::Righted) == 1);
    CHECK(count(e, MotionEvent::Raised) == 0);
}

TEST_CASE("Raised vs Righted is decided by source posture") {
    // The split that closes the asymmetry: only lying flat may start an idle
    // timer, so only FlatBack -> upright may say Raised. OnSide and Edge both
    // return to vertical too, but say Righted instead, which App never lets
    // start anything. FaceDown says neither -- unchanged from before the
    // split.

    SUBCASE("flat -> upright says Raised") {
        OrientationTracker t;
        uint64_t clock = 0;
        hold(t, UPRIGHT, 1000, clock);
        hold(t, FLAT, 1000, clock);
        REQUIRE(t.current() == Orientation::FlatBack);

        auto e = hold(t, UPRIGHT, 1000, clock);
        CHECK(count(e, MotionEvent::Raised) == 1);
        CHECK(count(e, MotionEvent::Righted) == 0);
    }

    SUBCASE("OnSide -> upright says Righted, not Raised") {
        OrientationTracker t;
        uint64_t clock = 0;
        hold(t, UPRIGHT, 1000, clock);
        hold(t, ON_EDGE, 1500, clock);
        REQUIRE(t.current() == Orientation::OnSide);

        auto e = hold(t, UPRIGHT, 1000, clock);
        CHECK(count(e, MotionEvent::Righted) == 1);
        CHECK(count(e, MotionEvent::Raised) == 0);
    }

    SUBCASE("Edge -> upright says Righted, not Raised") {
        OrientationTracker t;
        uint64_t clock = 0;
        hold(t, UPRIGHT, 1000, clock);
        // Under kFlatEnter (not flat) and under kVerticalEnter (not upright),
        // |x| ~ 0 (nowhere near OnSide): classifies Edge, same vector as "a
        // tilted-back device is Edge, not OnSide, and stays silent" above.
        const Vec3 TILTED_BACK{0.0f, 0.600f, -0.800f};
        hold(t, TILTED_BACK, 1000, clock);
        REQUIRE(t.current() == Orientation::Edge);

        auto e = hold(t, UPRIGHT, 1000, clock);
        CHECK(count(e, MotionEvent::Righted) == 1);
        CHECK(count(e, MotionEvent::Raised) == 0);
    }

    SUBCASE("FaceDown -> upright says neither") {
        OrientationTracker t;
        uint64_t clock = 0;
        hold(t, UPRIGHT, 1000, clock);
        hold(t, FACEDOWN, 1000, clock);
        REQUIRE(t.current() == Orientation::FaceDown);

        auto e = hold(t, UPRIGHT, 1000, clock);
        CHECK(count(e, MotionEvent::Raised) == 0);
        CHECK(count(e, MotionEvent::Righted) == 0);
    }
}

TEST_CASE("split thresholds keep a reading in the hysteresis band stable") {
    // The band between kFlatExit 0.70 and kFlatEnter 0.85 must be sticky in BOTH
    // directions: a reading inside it neither enters flat from upright, nor
    // leaves flat once there. A single threshold would chatter here.

    SUBCASE("does not enter flat from upright") {
        OrientationTracker t;
        uint64_t clock = 0;
        hold(t, UPRIGHT, 1000, clock);
        REQUIRE(t.current() == Orientation::UprightA);

        // |z| = 0.80, under kFlatEnter -- tilted well over but not set down.
        Vec3 tilted{0.0f, 0.600f, -0.800f};
        int events = 0;
        for (int i = 0; i < 2000; ++i) {
            tilted.z = (i % 2 == 0) ? -0.800f : -0.8005f;
            if (t.update(tilted, clock) != MotionEvent::None) ++events;
            clock += SAMPLE_US;
        }
        CHECK(events == 0);
        CHECK(t.current() != Orientation::FlatBack); // the property under test
    }

    SUBCASE("does not leave flat once settled") {
        OrientationTracker t;
        uint64_t clock = 0;
        hold(t, UPRIGHT, 1000, clock);
        hold(t, FLAT, 1000, clock);
        REQUIRE(t.current() == Orientation::FlatBack);

        // Nudged to |z| = 0.75 -- below kFlatEnter but above kFlatExit, so it
        // must stay flat. A single 0.85 threshold would drop out here and the
        // timer would resume itself on a knocked table.
        Vec3 nudged{0.0f, 0.661f, -0.750f};
        int events = 0;
        for (int i = 0; i < 2000; ++i) {
            nudged.z = (i % 2 == 0) ? -0.750f : -0.7505f;
            if (t.update(nudged, clock) != MotionEvent::None) ++events;
            clock += SAMPLE_US;
        }
        CHECK(events == 0);
        CHECK(t.current() == Orientation::FlatBack);
    }
}

TEST_CASE("power-on in any posture emits no event") {
    // The device must not appear to start itself. Whatever it wakes up holding,
    // the first classification is a state, not a command.
    for (const Vec3& start : {UPRIGHT, INVERTED, FLAT, FACEDOWN, ON_EDGE}) {
        OrientationTracker t;
        uint64_t clock = 0;
        const auto e = hold(t, start, 2000, clock);
        CHECK(count(e, MotionEvent::Flip) == 0);
        CHECK(count(e, MotionEvent::Raised) == 0);
        CHECK(count(e, MotionEvent::Righted) == 0);
        CHECK(count(e, MotionEvent::Tipped) == 0);
    }
}

TEST_CASE("the first vertical posture after power-on is not a flip") {
    OrientationTracker t;
    uint64_t clock = 0;
    const auto e = hold(t, INVERTED, 2000, clock);
    CHECK(count(e, MotionEvent::Flip) == 0);
    CHECK(t.current() == Orientation::UprightB);
}

TEST_CASE("rotating through the side keeps the flip armed") {
    // Edge is not a settled posture: rotating through the device's side is
    // exactly how a turn happens, so it must not disarm.
    OrientationTracker t;
    uint64_t clock = 0;
    hold(t, UPRIGHT, 1000, clock);
    hold(t, ON_EDGE, 1500, clock); // linger there, longer than kSideDwellUs
    CHECK(t.current() == Orientation::OnSide);
    CHECK(t.flipArmed());

    auto e = hold(t, INVERTED, 1000, clock);
    CHECK(count(e, MotionEvent::Flip) == 1);
}

TEST_CASE("the filter seeds from the first sample rather than ramping") {
    // Ramping up from the origin would pass through every posture on the way and
    // fire a spurious event at power-on.
    GravityFilter f;
    const Vec3& v = f.push(FACEDOWN);
    CHECK(v.z == doctest::Approx(1.0f));
    CHECK(f.primed());
}

TEST_CASE("the filter suppresses a single-sample spike") {
    // One bad reading -- a knock, an i2c glitch -- must not move the state.
    OrientationTracker t;
    GravityFilter f;
    uint64_t clock = 0;
    for (int i = 0; i < 300; ++i) { t.update(f.push(UPRIGHT), clock); clock += SAMPLE_US; }
    REQUIRE(t.current() == Orientation::UprightA);

    int events = 0;
    for (int i = 0; i < 3; ++i) {
        if (t.update(f.push(FACEDOWN), clock) != MotionEvent::None) ++events;
        clock += SAMPLE_US;
    }
    for (int i = 0; i < 300; ++i) { t.update(f.push(UPRIGHT), clock); clock += SAMPLE_US; }

    CHECK(events == 0);
    CHECK(t.current() == Orientation::UprightA);
}
