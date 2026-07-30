#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "timer/timer_model.hpp"

using h0::TimerModel;
using State = h0::TimerModel::State;

namespace {
constexpr uint64_t SEC = 1'000'000ull;
constexpr uint64_t MIN = 60ull * SEC;
} // namespace

TEST_CASE("a fresh model is idle and empty") {
    TimerModel t;
    CHECK(t.state() == State::Idle);
    CHECK(t.duration() == 0);
    CHECK(t.remaining(0) == 0);
    CHECK(t.elapsed(0) == 0);
}

TEST_CASE("setDuration parks the timer at full, idle") {
    TimerModel t;
    t.setDuration(5 * MIN);
    CHECK(t.state() == State::Idle);
    CHECK(t.remaining(0) == 5 * MIN);
    CHECK(t.fraction(0) == doctest::Approx(1.0f));

    // Idle does not count, no matter how much clock passes.
    CHECK(t.remaining(90 * MIN) == 5 * MIN);
}

TEST_CASE("a running timer counts down against the clock") {
    TimerModel t;
    t.setDuration(10 * SEC);
    t.start(1000 * SEC);

    CHECK(t.state() == State::Running);
    CHECK(t.remaining(1000 * SEC) == 10 * SEC);
    CHECK(t.remaining(1003 * SEC) == 7 * SEC);
    CHECK(t.elapsed(1003 * SEC) == 3 * SEC);
    CHECK(t.fraction(1005 * SEC) == doctest::Approx(0.5f));
}

TEST_CASE("elapsed is anchored, so poll frequency cannot cause drift") {
    // The whole point of the design. A model that accumulated per-frame deltas
    // would drift here; an anchored one cannot, because it never adds.
    TimerModel sparse, dense;
    sparse.setDuration(MIN);
    dense.setDuration(MIN);
    sparse.start(0);
    dense.start(0);

    // Poll one model 6000 times and the other twice, over the same interval.
    for (uint64_t i = 0; i <= 30 * SEC; i += 5000) (void)dense.remaining(i);
    (void)sparse.remaining(15 * SEC);

    CHECK(sparse.remaining(30 * SEC) == dense.remaining(30 * SEC));
    CHECK(sparse.remaining(30 * SEC) == 30 * SEC);
}

TEST_CASE("a long stall costs no accuracy") {
    // A 200 ms display push, a dropped frame, or a sleep must not lose time.
    TimerModel t;
    t.setDuration(MIN);
    t.start(0);
    CHECK(t.remaining(59 * SEC) == 1 * SEC); // never polled in between
}

TEST_CASE("remaining saturates at zero and never underflows") {
    TimerModel t;
    t.setDuration(SEC);
    t.start(0);
    // Unsigned arithmetic: an unguarded subtraction here yields ~584,000 years.
    CHECK(t.remaining(10 * SEC) == 0);
    CHECK(t.remaining(1'000'000 * SEC) == 0);
    CHECK(t.fraction(10 * SEC) == doctest::Approx(0.0f));
}

TEST_CASE("tick moves a run to Expired exactly once it is due") {
    TimerModel t;
    t.setDuration(5 * SEC);
    t.start(0);

    t.tick(4 * SEC);
    CHECK(t.state() == State::Running);

    t.tick(5 * SEC);
    CHECK(t.state() == State::Expired);
    CHECK(t.isExpired());
    CHECK(t.remaining(5 * SEC) == 0);
}

TEST_CASE("the model is correct even if tick is never called") {
    // tick() only exposes the edge; remaining() is derived, so a caller that
    // forgets to tick still gets the right number.
    TimerModel t;
    t.setDuration(5 * SEC);
    t.start(0);
    CHECK(t.remaining(9 * SEC) == 0);
}

TEST_CASE("pause freezes elapsed for arbitrarily long") {
    TimerModel t;
    t.setDuration(MIN);
    t.start(0);
    t.pause(20 * SEC);

    CHECK(t.state() == State::Paused);
    CHECK(t.remaining(20 * SEC) == 40 * SEC);
    // An hour paused changes nothing.
    CHECK(t.remaining(20 * SEC + 60 * MIN) == 40 * SEC);
}

TEST_CASE("resume does not bill the paused interval") {
    TimerModel t;
    t.setDuration(MIN);
    t.start(0);
    t.pause(20 * SEC);
    t.resume(20 * SEC + 60 * MIN); // paused an hour

    CHECK(t.state() == State::Running);
    CHECK(t.remaining(20 * SEC + 60 * MIN) == 40 * SEC);
    CHECK(t.remaining(20 * SEC + 60 * MIN + 10 * SEC) == 30 * SEC);
}

TEST_CASE("many pause/resume cycles do not accumulate error") {
    // The flat/upright gesture makes this the most-exercised path in the whole
    // product, so an error that compounds per cycle would be a real defect.
    TimerModel t;
    t.setDuration(100 * SEC);
    t.start(0);

    uint64_t now = 0;
    for (int i = 0; i < 50; ++i) {
        now += SEC;      // one second of running
        t.pause(now);
        now += 3 * SEC;  // three seconds paused, must not count
        t.resume(now);
    }
    CHECK(t.elapsed(now) == 50 * SEC);
    CHECK(t.remaining(now) == 50 * SEC);
}

TEST_CASE("transitions are idempotent") {
    TimerModel t;
    t.setDuration(MIN);

    t.start(0);
    t.start(10 * SEC); // must not re-anchor and lose 10 s
    CHECK(t.remaining(10 * SEC) == 50 * SEC);

    t.pause(10 * SEC);
    t.pause(20 * SEC); // must not re-bank
    CHECK(t.remaining(20 * SEC) == 50 * SEC);

    t.resume(20 * SEC);
    t.resume(30 * SEC); // must not re-anchor
    CHECK(t.remaining(30 * SEC) == 40 * SEC);
}

TEST_CASE("pause on a non-running timer does nothing") {
    TimerModel t;
    t.setDuration(MIN);
    t.pause(10 * SEC); // Idle
    CHECK(t.state() == State::Idle);
    CHECK(t.remaining(10 * SEC) == MIN);
}

TEST_CASE("reset returns to full and runs, from any state") {
    TimerModel t;
    t.setDuration(MIN);

    SUBCASE("from running") {
        t.start(0);
        t.reset(30 * SEC);
    }
    SUBCASE("from paused") {
        t.start(0);
        t.pause(30 * SEC);
        t.reset(30 * SEC);
    }
    SUBCASE("from expired") {
        // Flipping a spent hourglass is exactly how you start the next one.
        t.start(0);
        t.tick(2 * MIN);
        REQUIRE(t.state() == State::Expired);
        t.reset(30 * SEC);
    }
    SUBCASE("from idle") {
        t.reset(30 * SEC);
    }

    CHECK(t.state() == State::Running);
    CHECK(t.remaining(30 * SEC) == MIN);
    CHECK(t.remaining(40 * SEC) == 50 * SEC);
}

TEST_CASE("stop returns to full and idle") {
    TimerModel t;
    t.setDuration(MIN);
    t.start(0);
    t.stop();
    CHECK(t.state() == State::Idle);
    CHECK(t.remaining(90 * MIN) == MIN);
}

TEST_CASE("acknowledge clears an expiry, and only an expiry") {
    TimerModel t;
    t.setDuration(SEC);
    t.start(0);

    t.acknowledge(); // still running -- must be ignored
    CHECK(t.state() == State::Running);

    t.tick(2 * SEC);
    t.acknowledge();
    CHECK(t.state() == State::Idle);
    CHECK(t.remaining(2 * SEC) == SEC);
}

TEST_CASE("setDuration mid-run returns to idle at the new full") {
    TimerModel t;
    t.setDuration(MIN);
    t.start(0);
    t.setDuration(5 * MIN); // dialling a new time while it runs
    CHECK(t.state() == State::Idle);
    CHECK(t.remaining(30 * SEC) == 5 * MIN);
}

TEST_CASE("a zero duration expires rather than running forever") {
    TimerModel t;
    t.setDuration(0);
    t.start(0);
    CHECK(t.state() == State::Expired);
    CHECK(t.fraction(0) == doctest::Approx(0.0f)); // no divide by zero

    t.reset(SEC);
    CHECK(t.state() == State::Expired);
}

TEST_CASE("resuming an already-elapsed timer expires it") {
    // Paused with 1 s left, then the duration is somehow already consumed.
    TimerModel t;
    t.setDuration(10 * SEC);
    t.start(0);
    t.pause(10 * SEC); // exactly consumed
    CHECK(t.remaining(10 * SEC) == 0);
    t.resume(20 * SEC);
    CHECK(t.state() == State::Expired);
}

TEST_CASE("a non-monotonic clock cannot rewind the timer") {
    // Defensive: a stale `now` must not produce a negative delta, which as
    // unsigned would read as an enormous elapsed time.
    TimerModel t;
    t.setDuration(MIN);
    t.start(1000 * SEC);
    CHECK(t.elapsed(999 * SEC) == 0);
    CHECK(t.remaining(999 * SEC) == MIN);
}

TEST_CASE("remainingSeconds rounds up so the last second is displayed") {
    TimerModel t;
    t.setDuration(10 * SEC);
    t.start(0);

    CHECK(t.remainingSeconds(0) == 10);
    CHECK(t.remainingSeconds(500'000) == 10);        // 9.5 s left -> "10"
    CHECK(t.remainingSeconds(1 * SEC) == 9);
    CHECK(t.remainingSeconds(9 * SEC + 1) == 1);     // 0.999999 s left -> "1"
    CHECK(t.remainingSeconds(10 * SEC) == 0);        // exactly zero -> "0"
}

TEST_CASE("fraction spans 1.0 to 0.0 monotonically") {
    TimerModel t;
    t.setDuration(100 * SEC);
    t.start(0);

    CHECK(t.fraction(0) == doctest::Approx(1.0f));
    CHECK(t.fraction(25 * SEC) == doctest::Approx(0.75f));
    CHECK(t.fraction(50 * SEC) == doctest::Approx(0.5f));
    CHECK(t.fraction(100 * SEC) == doctest::Approx(0.0f));

    float prev = 1.1f;
    for (uint64_t s = 0; s <= 100; ++s) {
        const float f = t.fraction(s * SEC);
        CHECK(f <= prev);
        CHECK(f >= 0.0f);
        CHECK(f <= 1.0f);
        prev = f;
    }
}

TEST_CASE("an hour-long timer is exact to the microsecond") {
    // No float anywhere in the time path, so this is exact rather than close.
    TimerModel t;
    t.setDuration(60 * MIN);
    t.start(0);
    CHECK(t.remaining(60 * MIN - 1) == 1);
    CHECK(t.remaining(60 * MIN) == 0);
}
