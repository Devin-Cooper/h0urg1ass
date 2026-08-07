#include "doctest.h"

#include "app/app.hpp"

using h0::App;
using h0::Feedback;
using h0::MotionEvent;
using h0::TimerModel;

namespace {

constexpr uint64_t SEC = 1'000'000ull;
constexpr uint64_t MIN = 60ull * SEC;

/// Dial in a duration the way a user would: lay it flat first.
void dial(App& a, uint64_t us, uint64_t now) {
    a.onMotion(MotionEvent::Settled, now);
    REQUIRE(a.setDuration(us, now));
}

/// Start a run the only way there now is: turn it over.
void startRun(App& a, uint64_t now) {
    REQUIRE(a.onMotion(MotionEvent::Flip, now) == Feedback::Started);
}

} // namespace

TEST_CASE("the core loop: dial it, turn it over, it runs") {
    App a;
    dial(a, 5 * MIN, 0);
    CHECK(a.settingPosture());
    CHECK(a.timer().state() == TimerModel::State::Idle);

    // Standing it up is no longer a start -- upright is where you SET it.
    CHECK(a.onMotion(MotionEvent::Raised, 1 * SEC) == Feedback::None);
    CHECK(a.timer().state() == TimerModel::State::Idle);
    CHECK(a.settingPosture());   // still Idle, so the picker is still live

    CHECK(a.onMotion(MotionEvent::Flip, 2 * SEC) == Feedback::Started);
    CHECK(a.timer().isRunning());
    CHECK_FALSE(a.settingPosture());
    CHECK(a.timer().remaining(2 * SEC) == 5 * MIN);
}

TEST_CASE("laying it flat pauses, standing it up resumes, and the pause is free") {
    App a;
    dial(a, 10 * MIN, 0);
    startRun(a, 0);

    CHECK(a.onMotion(MotionEvent::Settled, 2 * MIN) == Feedback::Paused);
    CHECK(a.timer().state() == TimerModel::State::Paused);
    CHECK(a.timer().remaining(2 * MIN) == 8 * MIN);

    // An hour face down on a desk must cost the timer nothing.
    CHECK(a.onMotion(MotionEvent::Raised, 62 * MIN) == Feedback::Resumed);
    CHECK(a.timer().remaining(62 * MIN) == 8 * MIN);
}

TEST_CASE("flipping resets to full and runs, from any state") {
    App a;
    dial(a, 4 * MIN, 0);
    startRun(a, 0);

    SUBCASE("while running") {
        CHECK(a.onMotion(MotionEvent::Flip, 3 * MIN) == Feedback::Reset);
    }
    SUBCASE("while paused") {
        a.onMotion(MotionEvent::Settled, 3 * MIN);
        CHECK(a.onMotion(MotionEvent::Flip, 3 * MIN) == Feedback::Reset);
    }
    SUBCASE("after it finished") {
        a.tick(5 * MIN);
        REQUIRE(a.timer().isExpired());
        CHECK(a.onMotion(MotionEvent::Flip, 3 * MIN) == Feedback::Reset);
    }

    CHECK(a.timer().isRunning());
    CHECK(a.timer().remaining(3 * MIN) == 4 * MIN);
    CHECK_FALSE(a.alarmSounding());
}

TEST_CASE("expiry raises the alarm exactly once") {
    App a;
    dial(a, 1 * MIN, 0);
    startRun(a, 0);

    CHECK(a.tick(30 * SEC) == Feedback::None);
    CHECK_FALSE(a.alarmSounding());

    CHECK(a.tick(60 * SEC) == Feedback::AlarmOn);
    CHECK(a.alarmSounding());

    // Every subsequent tick must be silent, not a re-trigger.
    for (int i = 1; i < 20; ++i) CHECK(a.tick(60 * SEC + i * SEC) == Feedback::None);
    CHECK(a.alarmSounding());
}

TEST_CASE("face down silences a ringing alarm") {
    App a;
    dial(a, 1 * MIN, 0);
    startRun(a, 0);
    a.tick(60 * SEC);
    REQUIRE(a.alarmSounding());

    CHECK(a.onMotion(MotionEvent::Silence, 61 * SEC) == Feedback::AlarmOff);
    CHECK_FALSE(a.alarmSounding());
    CHECK(a.timer().state() == TimerModel::State::Idle);
}

TEST_CASE("face down with nothing ringing does nothing") {
    // It is also how the device ends up when put away, so it must be inert.
    App a;
    dial(a, 5 * MIN, 0);
    startRun(a, 0);

    CHECK(a.onMotion(MotionEvent::Silence, 1 * MIN) == Feedback::None);
    CHECK(a.timer().isRunning());
}

TEST_CASE("the alarm gives up rather than beeping forever") {
    App a;
    dial(a, 1 * MIN, 0);
    startRun(a, 0);
    a.tick(60 * SEC);
    REQUIRE(a.alarmSounding());

    CHECK(a.tick(60 * SEC + 30 * SEC) == Feedback::None); // still within timeout
    CHECK(a.alarmSounding());

    CHECK(a.tick(60 * SEC + App::kAlarmTimeoutUs) == Feedback::AlarmOff);
    CHECK_FALSE(a.alarmSounding());
    // The expiry is not forgotten -- the face must still read DONE.
    CHECK(a.timer().isExpired());
}

TEST_CASE("laying a finished timer flat clears it for the next one") {
    App a;
    dial(a, 1 * MIN, 0);
    startRun(a, 0);
    a.tick(60 * SEC);
    REQUIRE(a.alarmSounding());

    CHECK(a.onMotion(MotionEvent::Settled, 61 * SEC) == Feedback::AlarmOff);
    CHECK_FALSE(a.alarmSounding());
    CHECK(a.timer().state() == TimerModel::State::Idle);
    CHECK(a.settingPosture());
}

TEST_CASE("standing up a finished timer does not restart it") {
    // Restart is the flip, and it should stay the only thing that is.
    App a;
    dial(a, 1 * MIN, 0);
    a.onMotion(MotionEvent::Raised, 0);
    a.tick(60 * SEC);
    a.onMotion(MotionEvent::Silence, 61 * SEC); // silence it, still expired-ish

    a.onMotion(MotionEvent::Settled, 62 * SEC);
    // Flat cleared it to Idle, but standing up no longer starts anything.
    CHECK(a.onMotion(MotionEvent::Raised, 63 * SEC) == Feedback::None);
    CHECK(a.timer().state() == TimerModel::State::Idle);
}

TEST_CASE("a flip with no duration dialled is refused") {
    App a;
    // Raised is not a command any more, so it says nothing at all.
    CHECK(a.onMotion(MotionEvent::Raised, 0) == Feedback::None);
    // Flip is the start, so it still gets an audible refusal.
    CHECK(a.onMotion(MotionEvent::Flip, 1 * SEC) == Feedback::Rejected);
    CHECK(a.timer().state() == TimerModel::State::Idle);
}

TEST_CASE("the dial is live while idle and while flat, and dead while running") {
    // Otherwise a pocket could rewrite a running timer, and there is no undo.
    App a;

    SUBCASE("live while idle, without ever being laid flat") {
        // The new behaviour: power on upright and set a time in the hand.
        CHECK(a.settingPosture());
        CHECK(a.setDuration(5 * MIN, 0));
        CHECK(a.timer().duration() == 5 * MIN);
    }

    SUBCASE("dead while running") {
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        REQUIRE(a.timer().isRunning());

        CHECK_FALSE(a.settingPosture());
        CHECK_FALSE(a.setDuration(30 * MIN, 1 * MIN));
        CHECK(a.timer().duration() == 5 * MIN);
        CHECK(a.timer().isRunning());
    }

    SUBCASE("live again once laid flat mid-run") {
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        a.onMotion(MotionEvent::Settled, 2 * MIN);
        CHECK(a.settingPosture());
        CHECK(a.setDuration(30 * MIN, 2 * MIN));
        CHECK(a.timer().duration() == 30 * MIN);
    }

    SUBCASE("dead while paused on its side -- the hourglass stays up") {
        // Paused is deliberately NOT a setting posture: at 90 degrees what you
        // need to see is how much is left, not a pair of wheels.
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        CHECK(a.onMotion(MotionEvent::Tipped, 1 * MIN) == Feedback::Paused);
        CHECK(a.timer().state() == TimerModel::State::Paused);
        CHECK_FALSE(a.settingPosture());
        CHECK_FALSE(a.setDuration(30 * MIN, 1 * MIN));
        CHECK(a.timer().duration() == 5 * MIN);
    }
}

TEST_CASE("resting it on its side pauses a running timer") {
    App a;
    dial(a, 10 * MIN, 0);
    startRun(a, 0);

    CHECK(a.onMotion(MotionEvent::Tipped, 3 * MIN) == Feedback::Paused);
    CHECK(a.timer().state() == TimerModel::State::Paused);
    CHECK(a.timer().remaining(3 * MIN) == 7 * MIN);

    // And the pause costs nothing, same as flat.
    CHECK(a.onMotion(MotionEvent::Raised, 60 * MIN) == Feedback::Resumed);
    CHECK(a.timer().remaining(60 * MIN) == 7 * MIN);
}

TEST_CASE("resting it on its side does nothing when nothing is running") {
    App a;

    SUBCASE("idle") {
        dial(a, 5 * MIN, 0);
        CHECK(a.onMotion(MotionEvent::Tipped, 1 * SEC) == Feedback::None);
        CHECK(a.timer().state() == TimerModel::State::Idle);
    }

    SUBCASE("already paused") {
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        a.onMotion(MotionEvent::Tipped, 1 * MIN);
        REQUIRE(a.timer().state() == TimerModel::State::Paused);
        CHECK(a.onMotion(MotionEvent::Tipped, 2 * MIN) == Feedback::None);
        CHECK(a.timer().remaining(2 * MIN) == 4 * MIN);  // not re-paused later
    }

    SUBCASE("expired and ringing -- flat and face down own silencing") {
        dial(a, 1 * MIN, 0);
        startRun(a, 0);
        REQUIRE(a.tick(60 * SEC) == Feedback::AlarmOn);
        CHECK(a.onMotion(MotionEvent::Tipped, 61 * SEC) == Feedback::None);
        CHECK(a.alarmSounding());
        CHECK(a.timer().state() == TimerModel::State::Expired);
    }
}

TEST_CASE("the flip says Started from idle and Reset over a live run") {
    // Feedback::Reset is documented as the most emphatic pattern in the set
    // because it is the only destructive action. A flip from idle destroys
    // nothing, so it must not claim to.
    App a;

    SUBCASE("from idle it is a plain start") {
        dial(a, 5 * MIN, 0);
        CHECK(a.onMotion(MotionEvent::Flip, 1 * SEC) == Feedback::Started);
        CHECK(a.timer().isRunning());
    }

    SUBCASE("over a running timer it is a reset") {
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        CHECK(a.onMotion(MotionEvent::Flip, 2 * MIN) == Feedback::Reset);
        CHECK(a.timer().remaining(2 * MIN) == 5 * MIN);
    }

    SUBCASE("over a paused timer it is a reset") {
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        a.onMotion(MotionEvent::Settled, 2 * MIN);
        REQUIRE(a.timer().state() == TimerModel::State::Paused);
        CHECK(a.onMotion(MotionEvent::Flip, 3 * MIN) == Feedback::Reset);
    }

    SUBCASE("over a finished timer it is a reset") {
        dial(a, 1 * MIN, 0);
        startRun(a, 0);
        a.tick(60 * SEC);
        REQUIRE(a.timer().isExpired());
        CHECK(a.onMotion(MotionEvent::Flip, 61 * SEC) == Feedback::Reset);
        CHECK(a.timer().isRunning());
    }
}

TEST_CASE("the picker follows the timer state, not only the posture") {
    // settingPosture() is `flat_ || timer is Idle`. Both clauses are needed and
    // each of these subcases dies if the other clause is dropped.
    App a;

    SUBCASE("idle and NOT flat -- the new clause") {
        dial(a, 5 * MIN, 0);
        a.onMotion(MotionEvent::Raised, 1 * SEC);   // stood up, still idle
        CHECK_FALSE(a.timer().isRunning());
        CHECK(a.settingPosture());
    }

    SUBCASE("paused and flat -- the old clause") {
        dial(a, 5 * MIN, 0);
        startRun(a, 0);
        CHECK(a.onMotion(MotionEvent::Settled, 1 * MIN) == Feedback::Paused);
        CHECK(a.timer().state() == TimerModel::State::Paused);
        CHECK(a.settingPosture());
    }

    SUBCASE("expired keeps the hourglass, not the picker") {
        dial(a, 1 * MIN, 0);
        startRun(a, 0);
        a.tick(60 * SEC);
        REQUIRE(a.timer().isExpired());
        CHECK_FALSE(a.settingPosture());
    }
}

TEST_CASE("a full session runs end to end") {
    // The whole product, as a sequence of gestures.
    App a;
    uint64_t t = 0;

    dial(a, 2 * MIN, t);                                          // lay it down, set 2:00
    CHECK(a.onMotion(MotionEvent::Flip, t) == Feedback::Started);   // turn it over

    t = 30 * SEC;
    CHECK(a.onMotion(MotionEvent::Settled, t) == Feedback::Paused); // put it down
    t = 5 * MIN;                                                    // ...for a while
    CHECK(a.onMotion(MotionEvent::Raised, t) == Feedback::Resumed);
    CHECK(a.timer().remaining(t) == 90 * SEC);                      // pause was free

    t += 30 * SEC;
    CHECK(a.onMotion(MotionEvent::Flip, t) == Feedback::Reset);     // turn it over
    CHECK(a.timer().remaining(t) == 2 * MIN);                       // back to full

    t += 2 * MIN;
    CHECK(a.tick(t) == Feedback::AlarmOn);
    CHECK(a.onMotion(MotionEvent::Silence, t) == Feedback::AlarmOff); // face down
    CHECK(a.timer().state() == TimerModel::State::Idle);
}
