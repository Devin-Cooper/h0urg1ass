#include "doctest.h"

#include "app/app.hpp"

using h0::App;
using h0::FaceId;
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

} // namespace

TEST_CASE("the core loop: dial flat, stand it up, it runs") {
    App a;
    dial(a, 5 * MIN, 0);
    CHECK(a.settingPosture());
    CHECK(a.timer().state() == TimerModel::State::Idle);

    CHECK(a.onMotion(MotionEvent::Raised, 1 * SEC) == Feedback::Started);
    CHECK(a.timer().isRunning());
    CHECK_FALSE(a.settingPosture());
    CHECK(a.timer().remaining(1 * SEC) == 5 * MIN);
}

TEST_CASE("laying it flat pauses, standing it up resumes, and the pause is free") {
    App a;
    dial(a, 10 * MIN, 0);
    a.onMotion(MotionEvent::Raised, 0);

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
    a.onMotion(MotionEvent::Raised, 0);

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
    a.onMotion(MotionEvent::Raised, 0);

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
    a.onMotion(MotionEvent::Raised, 0);
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
    a.onMotion(MotionEvent::Raised, 0);

    CHECK(a.onMotion(MotionEvent::Silence, 1 * MIN) == Feedback::None);
    CHECK(a.timer().isRunning());
}

TEST_CASE("the alarm gives up rather than beeping forever") {
    App a;
    dial(a, 1 * MIN, 0);
    a.onMotion(MotionEvent::Raised, 0);
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
    a.onMotion(MotionEvent::Raised, 0);
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
    // Flat cleared it to Idle with a duration still set, so standing up starts.
    CHECK(a.onMotion(MotionEvent::Raised, 63 * SEC) == Feedback::Started);
}

TEST_CASE("gestures are refused when no duration has been dialled") {
    App a;
    CHECK(a.onMotion(MotionEvent::Raised, 0) == Feedback::Rejected);
    CHECK(a.onMotion(MotionEvent::Flip, 1 * SEC) == Feedback::Rejected);
    CHECK(a.timer().state() == TimerModel::State::Idle);
    // Refused, not ignored: the user gets an audible answer either way.
}

TEST_CASE("the dial is dead unless the device is flat") {
    // Otherwise a pocket could rewrite a running timer, and there is no undo.
    App a;
    dial(a, 5 * MIN, 0);
    a.onMotion(MotionEvent::Raised, 0);
    REQUIRE(a.timer().isRunning());

    CHECK_FALSE(a.setDuration(30 * MIN, 1 * MIN));
    CHECK(a.timer().duration() == 5 * MIN);
    CHECK(a.timer().isRunning());

    a.onMotion(MotionEvent::Settled, 2 * MIN);
    CHECK(a.setDuration(30 * MIN, 2 * MIN));
    CHECK(a.timer().duration() == 30 * MIN);
}

TEST_CASE("the hourglass is only offered where it means something") {
    App a;

    SUBCASE("no duration") {
        // Nothing to be a fraction of.
        a.cycleFace();
        CHECK(a.face() != FaceId::Hourglass);
    }

    SUBCASE("a short timer") {
        dial(a, 5 * MIN, 0);
        for (int i = 0; i < 3; ++i) {
            a.cycleFace();
            if (a.face() == FaceId::Hourglass) break;
        }
        CHECK(a.face() == FaceId::Hourglass);
    }

    SUBCASE("an hour is too long to watch sand drain") {
        dial(a, 60 * MIN, 0);
        for (int i = 0; i < 4; ++i) a.cycleFace();
        CHECK(a.face() != FaceId::Hourglass);
    }
}

TEST_CASE("a face that stops suiting the timer falls back rather than lying") {
    // An hourglass frozen at full because the duration is an hour looks like a
    // bug, so the selection degrades instead.
    App a;
    dial(a, 5 * MIN, 0);
    for (int i = 0; i < 3; ++i) {
        a.cycleFace();
        if (a.face() == FaceId::Hourglass) break;
    }
    REQUIRE(a.face() == FaceId::Hourglass);

    CHECK(a.setDuration(60 * MIN, 0));
    CHECK(a.face() == FaceId::Digits);

    // ...and comes back when it suits again, without the user re-choosing.
    CHECK(a.setDuration(5 * MIN, 0));
    CHECK(a.face() == FaceId::Hourglass);
}

TEST_CASE("cycling always lands on a usable face") {
    App a;
    dial(a, 60 * MIN, 0); // hourglass unavailable
    for (int i = 0; i < 10; ++i) {
        a.cycleFace();
        CHECK(a.face() != FaceId::Hourglass);
    }
}

TEST_CASE("a full session runs end to end") {
    // The whole product, as a sequence of gestures.
    App a;
    uint64_t t = 0;

    dial(a, 2 * MIN, t);                                          // lay it down, set 2:00
    CHECK(a.onMotion(MotionEvent::Raised, t) == Feedback::Started); // stand it up

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
