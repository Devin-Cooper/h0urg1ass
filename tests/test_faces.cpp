#include "doctest.h"

#include "golden.hpp"

#include <1bit/core/framebuffer.hpp>

#include "faces/digits_face.hpp"
#include "faces/hourglass_face.hpp"
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
