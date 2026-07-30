#include "doctest.h"

#include "golden.hpp"

#include <1bit/core/framebuffer.hpp>

#include <cstdio>

#include "faces/digits_face.hpp"
#include "faces/hourglass_face.hpp"
#include "sand/sand_vessel.hpp"
#include "faces/splitflap_face.hpp"
#include "timer/timer_model.hpp"

using onebit::BLACK;
using onebit::WHITE;

namespace {

constexpr uint64_t SEC = 1'000'000ull;
using Panel = onebit::Framebuffer<240, 280>;

/// Count ink pixels. The cheap invariant behind most of these tests: sand that
/// appears or vanishes is a conservation bug, and a blank face is a silent
/// failure that a golden baseline would happily enshrine.
int inkCount(const onebit::IFramebuffer& fb) {
    int n = 0;
    for (int16_t y = 0; y < fb.height(); ++y)
        for (int16_t x = 0; x < fb.width(); ++x)
            if (fb.getPixel(x, y) == BLACK) ++n;
    return n;
}

/// Ink within a horizontal band, used to check where the sand actually is.
int inkInRows(const onebit::IFramebuffer& fb, int16_t y0, int16_t y1) {
    int n = 0;
    for (int16_t y = y0; y <= y1 && y < fb.height(); ++y)
        for (int16_t x = 0; x < fb.width(); ++x)
            if (fb.getPixel(x, y) == BLACK) ++n;
    return n;
}

/// Nothing may be drawn under the rounded corners. The panel physically clips a
/// ~44 px radius, so ink there is invisible at best and clipped text at worst.
///
/// The clip is a rounded rectangle: within a corner quadrant the visible region
/// is a disc of radius R centred at (R, R) inwards from that corner, NOT the
/// complement of a disc centred on the corner point itself. The difference is
/// not academic -- the corner-point form rejects (16, 16), which `safe::INSET`
/// records as measured fully visible on hardware, and it went unnoticed until
/// the sand vessel became the first face to actually fill the safe box.
int inkInCorners(const onebit::IFramebuffer& fb) {
    constexpr int R = 44;
    const int16_t w = fb.width(), h = fb.height();
    int n = 0;
    for (int16_t y = 0; y < h; ++y) {
        for (int16_t x = 0; x < w; ++x) {
            // Distance from the arc centre of whichever corner this pixel is in.
            const int dx = (x < R) ? (R - x) : (x >= w - R ? x - (w - 1 - R) : 0);
            const int dy = (y < R) ? (R - y) : (y >= h - R ? y - (h - 1 - R) : 0);
            if (dx == 0 || dy == 0) continue; // not in a corner quadrant
            if (dx * dx + dy * dy > R * R && fb.getPixel(x, y) == BLACK) ++n;
        }
    }
    return n;
}

} // namespace

// ------------------------------------------------------------- hourglass --

TEST_CASE("hourglass is not offered without a duration") {
    h0::HourglassFace face;
    h0::TimerModel t;
    CHECK_FALSE(face.supports(t));   // no duration -- nothing to be a fraction of
    t.setDuration(60 * SEC);
    CHECK(face.supports(t));
}

TEST_CASE("digits face renders and respects the corners") {
    Panel fb;
    h0::TimerModel t;
    t.setDuration(12 * 60 * SEC);
    t.start(0);

    h0::DigitsFace face;
    face.render(fb, t, 0);
    CHECK(inkCount(fb) > 500);
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("digits golden across states") {
    Panel fb;
    h0::DigitsFace face;
    h0::TimerModel t;
    t.setDuration(12 * 60 * SEC);

    checkGolden((face.render(fb, t, 0), fb), "digits@idle");

    t.start(0);
    checkGolden((face.render(fb, t, 90 * SEC), fb), "digits@running");

    t.pause(90 * SEC);
    checkGolden((face.render(fb, t, 90 * SEC), fb), "digits@paused");

    t.resume(90 * SEC);
    t.tick(13 * 60 * SEC);
    REQUIRE(t.isExpired());
    checkGolden((face.render(fb, t, 13 * 60 * SEC), fb), "digits@expired");
}

TEST_CASE("digits stay inside the safe box for every duration") {
    // Regression: anything from 100 hours up ran past the safe box and off the
    // panel, where the framebuffer's own bounds check silently clipped it.
    // Expired is excluded -- the inversion deliberately fills the whole frame.
    auto outsideSafe = [](const onebit::IFramebuffer& fb) {
        int n = 0;
        for (int16_t y = 0; y < fb.height(); ++y)
            for (int16_t x = 0; x < fb.width(); ++x)
                if (fb.getPixel(x, y) == BLACK &&
                    (x < h0::safe::X || x >= h0::safe::X + h0::safe::W ||
                     y < h0::safe::Y || y >= h0::safe::Y + h0::safe::H)) ++n;
        return n;
    };
    for (uint64_t s : {59ull, 600ull, 3599ull, 3600ull, 35999ull, 359999ull, 360000ull}) {
        Panel fb;
        h0::TimerModel t;
        t.setDuration(s * SEC);
        t.start(0);
        h0::DigitsFace face;
        face.render(fb, t, 0);
        CAPTURE(s);
        CHECK(outsideSafe(fb) == 0);
    }
}

TEST_CASE("one hour is not confusable with one minute") {
    // H:MM would render an hour as "1:00" -- identical to one minute -- and a
    // second later "59:59", so the number appears to jump from 1 to 59.
    Panel fb;
    h0::DigitsFace face;
    h0::TimerModel t;
    t.setDuration(3600 * SEC);
    t.start(0);

    Panel oneMinute;
    face.render(fb, t, 0); // exactly 1 h remaining

    h0::TimerModel m;
    m.setDuration(60 * SEC);
    m.start(0);
    face.render(oneMinute, m, 0); // exactly 1 min remaining

    // The actual property: an hour must not render identically to a minute.
    // Ink count is the wrong proxy -- the hour form is auto-shrunk to fit, so it
    // legitimately carries FEWER pixels than the larger five-glyph form.
    int differing = 0;
    for (int16_t y = 0; y < 280; ++y)
        for (int16_t x = 0; x < 240; ++x)
            if (fb.getPixel(x, y) != oneMinute.getPixel(x, y)) ++differing;
    CHECK(differing > 500);
}

// ------------------------------------------------------------ split-flap --

TEST_CASE("split-flap settles within one second of every tick") {
    // The defect this face exists to avoid: SplitFlapDisplay steps one character
    // at a time, forward only. With the library's default alphanumeric sequence
    // a digit DECREMENT -- what a countdown does every second -- costs 39 flaps,
    // 5.19 s at the default cadence. The board never lands, and what it shows
    // instead of digits is letters.
    //
    // A descending digits-only sequence makes a decrement one flap. This pins
    // that: render at 20 Hz for a second per tick and require the readout to
    // match the model by the end of each one.
    h0::SplitFlapFace face;
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);

    Panel fb;
    for (int sec = 0; sec < 12; ++sec) {
        for (int f = 0; f < 20; ++f) {
            const uint64_t now = static_cast<uint64_t>(sec) * SEC + static_cast<uint64_t>(f) * 50'000ull;
            face.render(fb, t, now);
        }
        // By the end of the second, every cell must have reached its target.
        char want[8];
        const uint32_t rem = t.remainingSeconds(static_cast<uint64_t>(sec) * SEC);
        std::snprintf(want, sizeof(want), "%02u:%02u", rem / 60, rem % 60);
        CAPTURE(sec);
        CAPTURE(want);
        for (int16_t c = 0; c < 5; ++c) {
            CAPTURE(c);
            CHECK(face.boardChar(c) == want[c]);
        }
    }
}

TEST_CASE("split-flap renders and stays in the safe box") {
    h0::SplitFlapFace face;
    h0::TimerModel t;
    t.setDuration(12 * 60 * SEC);
    t.start(0);

    Panel fb;
    face.render(fb, t, 0);
    CHECK(inkCount(fb) > 300);
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("split-flap golden") {
    h0::SplitFlapFace face;
    h0::TimerModel t;
    t.setDuration(12 * 60 * SEC);
    t.start(0);
    Panel fb;
    face.render(fb, t, 0);
    checkGolden(fb, "splitflap@running");
}

TEST_CASE("expiry inverts the field") {
    // The largest signal available in one bit, and it must actually be large.
    Panel fb;
    h0::DigitsFace face;
    h0::TimerModel t;
    t.setDuration(SEC);
    t.start(0);

    face.render(fb, t, 0);
    const int running = inkCount(fb);

    t.tick(2 * SEC);
    face.render(fb, t, 2 * SEC);
    const int expired = inkCount(fb);

    CHECK(expired > running * 5);
}

// ------------------------------------------------- simulated hourglass --

namespace {

/// Run the face forward to a given point in a 120 s timer.
void runSand(h0::HourglassFace& face, h0::TimerModel& t, uint64_t to) {
    for (uint64_t now = 0; now <= to; now += 33'333ull) face.tick(t, now);
}

} // namespace

TEST_CASE("the corner helper flags clipped ink and clears the safe box") {
    // This helper gates several faces, so a version that always returns zero
    // would silently disarm all of them. Pin both directions.
    Panel fb;
    fb.clear(WHITE);
    CHECK(inkInCorners(fb) == 0);

    fb.setPixel(2, 2, BLACK); // deep under the top-left arc
    CHECK(inkInCorners(fb) == 1);

    fb.clear(WHITE);
    fb.setPixel(h0::safe::X, h0::safe::Y, BLACK); // measured visible on hardware
    fb.setPixel(239 - h0::safe::INSET, 279 - h0::safe::INSET, BLACK);
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("the sand drains from the upper chamber to the lower one") {
    Panel fb;
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);

    h0::HourglassFace face;
    face.restart(t, 4321);
    face.render(fb, t, 0);
    const int upperStart = inkInRows(fb, 0, 139);

    runSand(face, t, 110 * SEC);
    face.render(fb, t, 110 * SEC);
    const int upperEnd = inkInRows(fb, 0, 139);
    const int lowerEnd = inkInRows(fb, 141, 279);

    CHECK(upperStart > 0);
    CHECK(upperEnd < upperStart / 2); // most of it has gone
    CHECK(lowerEnd > 0);
}

TEST_CASE("the upper chamber actually empties") {
    // On a FLAT floor sand is stable at zero slope, so grains far from the hole
    // never slide in on their own -- measured, 33-86% strand permanently. The
    // centreline attractor is what makes the vessel drainable at all, so this
    // pins the property it exists for.
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);

    h0::HourglassFace face;
    face.restart(t, 99);
    runSand(face, t, 130 * SEC);

    Panel fb;
    face.render(fb, t, 130 * SEC);

    // Measure against an empty vessel rather than a guessed threshold: the
    // border alone puts ~900 px of ink above the floor, which swamps any
    // constant picked by eye.
    Panel bare;
    bare.clear(WHITE);
    h0::SandGrid noSand;
    h0::renderSand(bare, noSand, h0::makeVessel(h0::SandVessel::kHoleHalf));
    const int sandLeft = inkInRows(fb, 0, 138) - inkInRows(bare, 0, 138);

    CHECK(sandLeft >= 0);
    CHECK(sandLeft < 40); // a couple of stragglers at most, out of 900 grains
}

TEST_CASE("the sand tracks the schedule rather than free-falling") {
    // The gate is the whole reason this is a timer and not a toy: unmetered the
    // charge drains in seconds regardless of the duration set.
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);

    h0::HourglassFace face;
    face.restart(t, 7);
    runSand(face, t, 30 * SEC);

    Panel fb;
    face.render(fb, t, 30 * SEC);
    const int upperQuarter = inkInRows(fb, 0, 139);

    runSand(face, t, 90 * SEC);
    face.render(fb, t, 90 * SEC);
    const int upperThreeQuarter = inkInRows(fb, 0, 139);

    // A quarter of the way through, most of the sand is still upstairs.
    CHECK(upperQuarter > upperThreeQuarter);
    CHECK(upperThreeQuarter > 0);
}

TEST_CASE("the simulated hourglass respects the rounded corners") {
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);
    h0::HourglassFace face;
    face.restart(t, 5);
    Panel fb;
    face.render(fb, t, 0);
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("a new duration re-charges the sand") {
    // Carrying an old charge over would drain at the wrong rate for the whole
    // of the next run.
    h0::TimerModel t;
    t.setDuration(60 * SEC);
    t.start(0);
    h0::HourglassFace face;
    face.restart(t, 11);
    runSand(face, t, 40 * SEC);

    Panel fb;
    face.render(fb, t, 40 * SEC);
    const int drained = inkInRows(fb, 0, 139);

    t.setDuration(600 * SEC);
    t.start(0);
    face.tick(t, 41 * SEC); // notices the change and restarts
    face.render(fb, t, 41 * SEC);
    CHECK(inkInRows(fb, 0, 139) > drained);
}
