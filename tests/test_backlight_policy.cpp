#include "doctest.h"

#include "power/backlight_policy.hpp"

using h0::Settings;

namespace {
constexpr uint64_t SEC = 1'000'000ull;
}

TEST_CASE("the ladder steps at the configured timeouts") {
    Settings s = h0::kDefaults; // dim at 20 s, blank 60 s later
    CHECK(h0::backlightFor(s, 0, false).level == s.backlightActive);
    CHECK(h0::backlightFor(s, 19 * SEC, false).level == s.backlightActive);
    CHECK(h0::backlightFor(s, 21 * SEC, false).level == s.backlightDim);
    CHECK(h0::backlightFor(s, 79 * SEC, false).level == s.backlightDim);
    CHECK(h0::backlightFor(s, 81 * SEC, false).level == 0);
}

TEST_CASE("rendering is suspended only in the blank state") {
    Settings s = h0::kDefaults;
    CHECK(h0::backlightFor(s, 0, false).render);
    CHECK(h0::backlightFor(s, 21 * SEC, false).render);
    CHECK_FALSE(h0::backlightFor(s, 81 * SEC, false).render);
}

TEST_CASE("a sounding alarm holds full brightness at any idle time") {
    // The one moment the device is trying to be seen from across a room.
    Settings s = h0::kDefaults;
    const h0::BacklightState st = h0::backlightFor(s, 3600 * SEC, true);
    CHECK(st.level == s.backlightActive);
    CHECK(st.render);
}

TEST_CASE("NEVER really means never") {
    Settings s = h0::kDefaults;
    s.dimAfterS = 0;
    CHECK(h0::backlightFor(s, 3600 * SEC, false).level == s.backlightActive);

    s.dimAfterS = 20;
    s.blankAfterS = 0;
    const h0::BacklightState st = h0::backlightFor(s, 3600 * SEC, false);
    CHECK(st.level == s.backlightDim);
    CHECK(st.render);
}
