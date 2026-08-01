#include "doctest.h"

#include <1bit/hal/pixel_format.hpp>

#include <string>

#include "settings/rows.hpp"
#include "settings/settings.hpp"
#include "settings/theme.hpp"

using h0::RowId;
using h0::Settings;

TEST_CASE("defaults match the constants the firmware shipped with") {
    // These are the compile-time values from main.cpp, so a device with a blank
    // flash sector behaves exactly as it did before settings existed.
    const Settings d = h0::kDefaults;
    CHECK(d.backlightActive == 64);
    CHECK(d.backlightDim == 36);
    CHECK(d.dimAfterS == 20);
    CHECK(d.alarmS == 60);
    CHECK(d.mute == 0);
    CHECK(d.batCalPermille == 1000);
}

TEST_CASE("brightness has a hard floor so the screen is never unreadable") {
    // The one clamp that prevents a setting from destroying its own escape
    // route. There is no undo anywhere in this device.
    Settings s = h0::kDefaults;
    s.backlightActive = 0;
    h0::clamp(s);
    CHECK(s.backlightActive >= 16);
}

TEST_CASE("the dim level can never exceed the active level") {
    Settings s = h0::kDefaults;
    s.backlightActive = 24;
    s.backlightDim = 200;
    h0::clamp(s);
    CHECK(s.backlightDim <= s.backlightActive);
}

TEST_CASE("nonsense survives clamping as something usable") {
    // A CRC-valid record can still hold garbage -- a downgrade, a bit flip, a
    // bug. Clamping per field, not merely defaulting on a blank sector, is what
    // makes that non-fatal.
    Settings s{};
    s.version = 9999;
    s.themeId = 200;
    s.backlightActive = 0;
    s.backlightDim = 255;
    s.dimAfterS = 60000;
    s.blankAfterS = 60000;
    s.alarmS = 0;
    s.mute = 77;
    s.batCalPermille = 3;
    h0::clamp(s);

    CHECK(s.themeId < static_cast<uint8_t>(h0::ThemeId::Count));
    CHECK(s.backlightActive >= 16);
    CHECK(s.backlightDim <= s.backlightActive);
    CHECK(s.mute <= 1);
    CHECK(s.batCalPermille >= 850);
    CHECK(s.batCalPermille <= 1150);
}

TEST_CASE("every ladder round-trips value to index and back") {
    for (uint8_t r = 0; r < h0::rowCount(); ++r) {
        const RowId id = static_cast<RowId>(r);
        const uint8_t n = h0::ladderSize(id);
        if (n == 0) continue; // read-only rows have no ladder

        // DEFAULTS is deliberately not round-trippable: it is an ACTION wheel,
        // not a value. Selecting RESET rewrites the whole struct and the wheel
        // then rests back on KEEP, which is the honest reading -- there is no
        // persistent "reset" state for it to report.
        if (id == RowId::Defaults) continue;

        for (uint8_t i = 0; i < n; ++i) {
            Settings s = h0::kDefaults;

            if (id == RowId::Cal) {
                // AUTO (index 0) is like DEFAULTS: an ACTION, not a value.
                // Selecting it re-arms automatic calibration and leaves
                // batCalPermille wherever it already was, so there is no
                // persistent value for it to round-trip through -- checking
                // ladderIndex == 0 here would be trivially true no matter
                // what the gain field held, which is not what this loop is
                // for. See "leaving AUTO keeps the learned gain instead of
                // jumping to the bottom" in test_settings_ui.cpp for the real
                // AUTO/MAN transition.
                if (i == 0) continue;
                // Every nonzero index round-trips through its permille
                // exactly like any other ladder, PROVIDED the wheel is
                // already off AUTO -- starting from batCalAuto == 1
                // (kDefaults' value) would instead take the "leave AUTO"
                // branch, which discards the index. Clearing it here is what
                // exercises the index<->permille formula itself, all 151
                // manual positions of it, kCalMin through kCalMax.
                s.batCalAuto = 0;
            }

            h0::applyLadder(id, i, s);
            h0::clamp(s);
            CHECK(h0::ladderIndex(id, s) == i);
        }
    }
}

TEST_CASE("DEFAULTS always rests on KEEP, even after a reset") {
    Settings s = h0::kDefaults;
    s.backlightActive = 255;
    h0::applyLadder(RowId::Defaults, 1, s); // RESET
    CHECK(s == h0::kDefaults);
    CHECK(h0::ladderIndex(RowId::Defaults, s) == 0);
}

TEST_CASE("DEFAULTS labels both its entries") {
    // Without this the value wheel draws KEEP twice and the user is asked to
    // drag onto a word that never appears.
    CHECK(std::string(h0::ladderLabel(RowId::Defaults, 0)) == "KEEP");
    CHECK(std::string(h0::ladderLabel(RowId::Defaults, 1)) == "RESET");
    CHECK(h0::ladderLabel(RowId::Theme, 0) == nullptr);
}

TEST_CASE("only CAL accelerates") {
    // A 5x flick across a 4-entry theme wheel is a slot machine. CAL has 151
    // entries and genuinely needs the gain.
    CHECK(h0::rowAccelerates(RowId::Cal));
    CHECK_FALSE(h0::rowAccelerates(RowId::Theme));
    CHECK_FALSE(h0::rowAccelerates(RowId::Sound));
    CHECK_FALSE(h0::rowAccelerates(RowId::Bright));
}

TEST_CASE("the battery row is read-only") {
    CHECK(h0::ladderSize(RowId::Battery) == 0);
}

TEST_CASE("every theme has distinct ink and paper") {
    // An ink that equals its paper is an invisible screen, and there is no undo.
    for (uint8_t i = 0; i < static_cast<uint8_t>(h0::ThemeId::Count); ++i) {
        const h0::Theme& t = h0::themeFor(static_cast<h0::ThemeId>(i));
        CHECK(t.ink != t.paper);
        CHECK(t.name != nullptr);
    }
}

TEST_CASE("the theme constants are what the arithmetic says") {
    // Expected values are computed from the library's constexpr rgb565(),
    // never duplicated as hex literals: this suite is the closest thing
    // setColors() has to a consumer test, and a hex literal here would only
    // enshrine whatever theme.cpp already emits rather than catch it being
    // wrong. (This class of bug is real -- rgb565(255,176,0) is 0xFD80, and
    // an earlier draft of this file wrote 0xFD20.)
    using onebit::rgb565;
    CHECK(h0::themeFor(h0::ThemeId::White).ink == rgb565(255, 255, 255));
    CHECK(h0::themeFor(h0::ThemeId::White).paper == rgb565(0, 0, 0));
    CHECK(h0::themeFor(h0::ThemeId::Paper).ink == rgb565(0, 0, 0));
    CHECK(h0::themeFor(h0::ThemeId::Paper).paper == rgb565(255, 255, 255));
    CHECK(h0::themeFor(h0::ThemeId::Amber).ink == rgb565(255, 176, 0));
    CHECK(h0::themeFor(h0::ThemeId::Amber).paper == rgb565(0, 0, 0));
    CHECK(h0::themeFor(h0::ThemeId::Night).ink == rgb565(255, 32, 0));
    CHECK(h0::themeFor(h0::ThemeId::Night).paper == rgb565(0, 0, 0));
}

TEST_CASE("automatic calibration is armed by default and the floor is unlearned") {
    // Both defaults are load-bearing. Auto must be on or no unit ever
    // calibrates; the floor must be 0 or the cutoff arms against a number
    // nothing measured.
    CHECK(h0::kDefaults.batCalAuto == 1);
    CHECK(h0::kDefaults.batFloorRawMv == 0);
}

TEST_CASE("clamp rejects a nonsense auto flag and an impossible floor") {
    h0::Settings s;
    s.batCalAuto = 7;
    s.batFloorRawMv = 60000; // no cell reads this; a corrupt record
    h0::clamp(s);
    CHECK(s.batCalAuto == 1);
    CHECK(s.batFloorRawMv == 0); // back to "not learned", not to a wrong floor
}

TEST_CASE("clamp keeps a plausible floor") {
    h0::Settings s;
    s.batFloorRawMv = 3380;
    h0::clamp(s);
    CHECK(s.batFloorRawMv == 3380);
}
