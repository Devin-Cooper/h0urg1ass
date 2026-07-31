#include "doctest.h"

#include <string>

#include "settings/rows.hpp"
#include "settings/settings.hpp"

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
