#include "doctest.h"

#include "golden.hpp"

#include <1bit/core/framebuffer.hpp>

#include <cstdio>

#include "faces/digits_face.hpp"
#include "faces/hourglass_face.hpp"
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
/// Checked against the measured radius with the safe inset as the tolerance.
int inkInCorners(const onebit::IFramebuffer& fb) {
    constexpr int R = 44;
    const int16_t w = fb.width(), h = fb.height();
    int n = 0;
    const int16_t cx[4] = {0, static_cast<int16_t>(w - 1), 0, static_cast<int16_t>(w - 1)};
    const int16_t cy[4] = {0, 0, static_cast<int16_t>(h - 1), static_cast<int16_t>(h - 1)};
    for (int c = 0; c < 4; ++c) {
        for (int16_t dy = 0; dy < R; ++dy) {
            for (int16_t dx = 0; dx < R; ++dx) {
                // Outside the corner arc => physically clipped.
                if (dx * dx + dy * dy >= R * R) {
                    const int16_t x = (cx[c] == 0) ? dx : static_cast<int16_t>(w - 1 - dx);
                    const int16_t y = (cy[c] == 0) ? dy : static_cast<int16_t>(h - 1 - dy);
                    if (fb.getPixel(x, y) == BLACK) ++n;
                }
            }
        }
    }
    return n;
}

} // namespace

// ------------------------------------------------------------- hourglass --

TEST_CASE("hourglass draws something at every fill level") {
    Panel fb;
    for (float f : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        h0::HourglassFace::renderAt(fb, f, false, 0);
        CAPTURE(f);
        // The glass outline alone is ~700 px, so anything near zero means the
        // face silently rendered nothing.
        CHECK(inkCount(fb) > 500);
    }
}

TEST_CASE("hourglass respects the rounded corners") {
    Panel fb;
    h0::HourglassFace::renderAt(fb, 1.0f, true, 0);
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("sand moves from the upper bulb to the lower one") {
    Panel fb;
    constexpr int16_t NECK = 148;

    h0::HourglassFace::renderAt(fb, 1.0f, false, 0);
    const int fullUpper = inkInRows(fb, 0, NECK);
    const int fullLower = inkInRows(fb, static_cast<int16_t>(NECK + 1), 279);

    h0::HourglassFace::renderAt(fb, 0.0f, false, 0);
    const int emptyUpper = inkInRows(fb, 0, NECK);
    const int emptyLower = inkInRows(fb, static_cast<int16_t>(NECK + 1), 279);

    CHECK(fullUpper > emptyUpper);  // the top drains
    CHECK(emptyLower > fullLower);  // the bottom fills
}

TEST_CASE("upper sand decreases monotonically as the timer runs down") {
    Panel fb;
    int prev = 1 << 30;
    for (int i = 10; i >= 0; --i) {
        h0::HourglassFace::renderAt(fb, static_cast<float>(i) / 10.0f, false, 0);
        const int upper = inkInRows(fb, 0, 147);
        CAPTURE(i);
        CHECK(upper <= prev);
        prev = upper;
    }
}

TEST_CASE("the fill is area-linear, not height-linear") {
    // The bulbs taper ~15:1, so a height mapping would drain fast at the rim and
    // crawl at the neck. Half the sand must be gone at half the time, within a
    // tolerance that allows for the dither and the glass outline.
    Panel fb;
    h0::HourglassFace::renderAt(fb, 1.0f, false, 0);
    const int full = inkInRows(fb, 0, 147);
    h0::HourglassFace::renderAt(fb, 0.0f, false, 0);
    const int empty = inkInRows(fb, 0, 147);
    h0::HourglassFace::renderAt(fb, 0.5f, false, 0);
    const int half = inkInRows(fb, 0, 147);

    const double sandFull = full - empty;
    const double sandHalf = half - empty;
    CHECK(sandHalf / sandFull == doctest::Approx(0.5).epsilon(0.08));
}

TEST_CASE("the running stream is visible and pausing removes it") {
    Panel fb;
    h0::HourglassFace::renderAt(fb, 0.5f, true, 0);
    const int running = inkCount(fb);
    h0::HourglassFace::renderAt(fb, 0.5f, false, 0);
    const int paused = inkCount(fb);

    // Running must add the falling column and nothing else.
    CHECK(running > paused);
    CHECK(running - paused < 400);
}

TEST_CASE("an empty hourglass shows no stream even while running") {
    Panel fb;
    h0::HourglassFace::renderAt(fb, 0.0f, true, 0);
    const int running = inkCount(fb);
    h0::HourglassFace::renderAt(fb, 0.0f, false, 0);
    CHECK(running == inkCount(fb));
}

TEST_CASE("the dither is screen-anchored, not sand-anchored") {
    // The property that stops the whole sand body crawling as the level moves.
    // Where two fill levels both contain a given pixel, that pixel's ink state
    // must be identical -- the boundary moves through a stationary texture.
    Panel a, b;
    h0::HourglassFace::renderAt(a, 0.90f, false, 0);
    h0::HourglassFace::renderAt(b, 0.70f, false, 0);

    int shared = 0, differing = 0;
    for (int16_t y = 100; y <= 147; ++y) {          // deep inside both fills
        for (int16_t x = 90; x <= 150; ++x) {
            if (a.getPixel(x, y) == BLACK || b.getPixel(x, y) == BLACK) {
                ++shared;
                if (a.getPixel(x, y) != b.getPixel(x, y)) ++differing;
            }
        }
    }
    REQUIRE(shared > 200);
    CHECK(differing == 0);
}

TEST_CASE("hourglass is not offered without a duration") {
    h0::HourglassFace face;
    h0::TimerModel t;
    CHECK_FALSE(face.supports(t));   // no duration -- nothing to be a fraction of
    t.setDuration(60 * SEC);
    CHECK(face.supports(t));
}

TEST_CASE("the pile grows all the way to empty") {
    // Regression: the pile used to saturate at fraction 0.13 and then never
    // change again, freezing for the last eighth of every countdown while the
    // top kept draining.
    Panel fb;
    int prev = -1;
    for (int i = 100; i >= 0; --i) {
        h0::HourglassFace::renderAt(fb, static_cast<float>(i) / 100.0f, false, 0);
        const int lower = inkInRows(fb, 149, 279);
        CAPTURE(i);
        CHECK(lower >= prev);
        prev = lower;
    }
    h0::HourglassFace::renderAt(fb, 0.15f, false, 0);
    const int a = inkInRows(fb, 149, 279);
    h0::HourglassFace::renderAt(fb, 0.00f, false, 0);
    const int b = inkInRows(fb, 149, 279);
    CHECK(b > a + 200); // the last 15% must still visibly move
}

TEST_CASE("the stream bridges the neck to the pile at every running level") {
    // Regression: the stream used to vanish for every fraction below 0.14 --
    // showing sand upstairs, a static pile downstairs, and nothing connecting
    // them, for exactly the part of the countdown a user is watching.
    for (int i = 99; i >= 1; --i) {
        const float f = static_cast<float>(i) / 100.0f;
        Panel run, still;
        h0::HourglassFace::renderAt(run, f, true, 0);
        h0::HourglassFace::renderAt(still, f, false, 0);
        CAPTURE(f);
        CHECK(inkCount(run) > inkCount(still)); // a stream exists at all
    }
}

TEST_CASE("the lower pile is a cone, not a level") {
    // Area-based assertions cannot tell a cone from a flat fill of equal area,
    // so this pins the shape itself: width must grow with depth.
    Panel fb;
    h0::HourglassFace::renderAt(fb, 0.5f, false, 0);
    auto width = [&](int16_t y) {
        int lo = 9999, hi = -1;
        for (int16_t x = 47; x <= 193; ++x)
            if (fb.getPixel(x, y) == BLACK) { if (x < lo) lo = x; hi = x; }
        return (hi < 0) ? 0 : (hi - lo + 1);
    };
    int16_t apex = -1;
    for (int16_t y = 149; y <= 250; ++y) if (width(y) > 4) { apex = y; break; }
    REQUIRE(apex >= 149);
    CAPTURE(apex);
    CHECK(width(static_cast<int16_t>(apex + 24)) > width(static_cast<int16_t>(apex + 6)) + 20);
}

TEST_CASE("sand keeps a paper gutter from the glass wall") {
    // The dither is 82% coverage against a solid wall -- close enough in density
    // that without a white gap the two regions read as one and the silhouette is
    // lost.
    Panel fb;
    h0::HourglassFace::renderAt(fb, 1.0f, false, 0);
    for (int16_t y = 45; y <= 140; y = static_cast<int16_t>(y + 5)) {
        int16_t wall = -1;
        for (int16_t x = 40; x < 120; ++x)
            if (fb.getPixel(x, y) == BLACK) { wall = x; break; }
        REQUIRE(wall > 0);
        CAPTURE(y);
        CAPTURE(wall);
        // wall is 2 px, then at least 1 px of paper before any sand
        CHECK(fb.getPixel(static_cast<int16_t>(wall + 2), y) == WHITE);
    }
}

TEST_CASE("the stream does not animate") {
    // A per-frame wobble on the thinnest feature on screen is the shimmer the
    // rule set forbids, and it would make the stream the only thing changing in
    // most frames -- continuous repaints on a battery device.
    Panel a, b;
    h0::HourglassFace::renderAt(a, 0.5f, true, 0);
    h0::HourglassFace::renderAt(b, 0.5f, true, 997);
    for (int16_t y = 0; y < 280; ++y)
        for (int16_t x = 0; x < 240; ++x)
            REQUIRE(a.getPixel(x, y) == b.getPixel(x, y));
}

TEST_CASE("hourglass golden") {
    Panel fb;
    h0::HourglassFace::renderAt(fb, 1.00f, false, 0);
    checkGolden(fb, "hourglass@full");
    h0::HourglassFace::renderAt(fb, 0.50f, true, 0);
    checkGolden(fb, "hourglass@half-running");
    h0::HourglassFace::renderAt(fb, 0.00f, false, 0);
    checkGolden(fb, "hourglass@empty");
}

// ---------------------------------------------------------------- digits --

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
