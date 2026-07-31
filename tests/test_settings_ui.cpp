#include "doctest.h"

#include "app/settings_ui.hpp"

using h0::RowId;
using h0::Settings;
using h0::SettingsUi;

namespace {

constexpr int16_t PPU = h0::DragColumn::kPixelsPerUnit;

/// Drag a column slowly enough to stay in the 1x band, one unit per PPU.
void dragUnits(SettingsUi& ui, uint8_t column, int units) {
    const int16_t dir = units > 0 ? -1 : 1; // up is negative y and is positive units
    const int n = (units > 0 ? units : -units) * PPU;
    int16_t y = 200;
    ui.onDrag(column, true, y);
    for (int i = 0; i < n; ++i) {
        y = static_cast<int16_t>(y + dir);
        ui.onDrag(column, true, y);
    }
    ui.onDrag(column, false, y);
}

/// Advance to a named row. BOUNDED by one full lap: the wheel wraps, so an
/// unbounded search would hang the whole suite on any indexing bug rather than
/// failing it.
bool seekRow(SettingsUi& ui, RowId want) {
    for (uint8_t i = 0; i < h0::rowCount(); ++i) {
        if (ui.currentRow() == want) return true;
        dragUnits(ui, 1, 1);
    }
    return ui.currentRow() == want;
}

} // namespace

TEST_CASE("opening snapshots the settings") {
    SettingsUi ui;
    Settings s = h0::kDefaults;
    s.backlightActive = 128;
    ui.open(s);
    CHECK(ui.isOpen());
    CHECK(ui.live() == s);
}

TEST_CASE("cancel restores the snapshot exactly") {
    // The whole undo story. Every edit previews live, and lifting the device
    // puts every one of them back.
    SettingsUi ui;
    const Settings before = h0::kDefaults;
    ui.open(before);

    dragUnits(ui, 1, 1); // move to BRIGHT
    dragUnits(ui, 2, 3); // change it
    REQUIRE(ui.live() != before);

    const Settings after = ui.cancel();
    CHECK(after == before);
    CHECK_FALSE(ui.isOpen());
    // The returned copy matching `before` is not enough: `after` is a
    // separate value that could equal the snapshot by construction (it is
    // literally `snapshot_`, which the drag calls above never touched)
    // even if `live_` itself were never put back. A caller that reads
    // live() during the close transition -- before the screen actually
    // tears down -- must not see a flash of the edited value, so pin that
    // live_ itself was reset, not merely the return value.
    CHECK(ui.live() == before);
}

TEST_CASE("commit returns the edited settings and closes") {
    SettingsUi ui;
    ui.open(h0::kDefaults);
    dragUnits(ui, 1, 1);
    dragUnits(ui, 2, 2);
    const Settings edited = ui.live();

    const Settings out = ui.commit();
    CHECK(out == edited);
    CHECK_FALSE(ui.isOpen());
}

TEST_CASE("the row wheel wraps but value wheels clamp") {
    // Inverts the picker's rule deliberately: 0-99 minutes has no natural end,
    // but a setting does, and hiding it is dishonest. It also fixes a rendering
    // defect -- a wrapping two-entry wheel shows the same value repeated across
    // the five visible rows.
    SettingsUi ui;
    ui.open(h0::kDefaults);

    dragUnits(ui, 1, h0::rowCount()); // a full lap of the row wheel
    CHECK(ui.rowIndex() == 0);

    // A partial lap must land on the row actually reached, not merely return
    // to 0 -- pinning that the wheel counts rows one at a time rather than
    // silently normalizing back to the start on every drag.
    dragUnits(ui, 1, 3);
    CHECK(ui.rowIndex() == 3);
    dragUnits(ui, 1, static_cast<int>(h0::rowCount()) - 3); // finish the lap

    // THEME is row 0, four entries. Drive far past the end.
    dragUnits(ui, 2, 50);
    CHECK(ui.live().themeId == static_cast<uint8_t>(h0::ThemeId::Count) - 1);
    dragUnits(ui, 2, -50);
    CHECK(ui.live().themeId == 0);

    // Prove the clamp is a real ceiling/floor, not merely "large drags don't
    // overflow": one unit short of the end must still move, and one more must
    // not go past it. A wheel that silently wrapped or that clamped too early
    // would both pass the +/-50 checks above but fail this.
    const uint8_t last = static_cast<uint8_t>(h0::ThemeId::Count) - 1;
    dragUnits(ui, 2, last - 1);
    CHECK(ui.live().themeId == last - 1);
    dragUnits(ui, 2, 1);
    CHECK(ui.live().themeId == last);
    dragUnits(ui, 2, 1); // one past the end: must clamp, not wrap to 0
    CHECK(ui.live().themeId == last);
}

TEST_CASE("the read-only battery row ignores its value wheel") {
    SettingsUi ui;
    ui.open(h0::kDefaults);
    REQUIRE(seekRow(ui, RowId::Battery));

    const Settings before = ui.live();
    dragUnits(ui, 2, 5);
    CHECK(ui.live() == before);
}

TEST_CASE("the dim level cannot be dragged above the active level") {
    // Enforced on every edit, not only on load, or the invariant holds in flash
    // and not on screen.
    SettingsUi ui;
    Settings s = h0::kDefaults;
    s.backlightActive = 24;
    s.backlightDim = 16;
    ui.open(s);

    REQUIRE(seekRow(ui, RowId::DimTo));
    dragUnits(ui, 2, 20); // drive DIM TO to its ceiling
    CHECK(ui.live().backlightDim <= ui.live().backlightActive);
}

TEST_CASE("lowering BRIGHT drags DIM TO down with it") {
    SettingsUi ui;
    Settings s = h0::kDefaults;
    s.backlightActive = 255;
    s.backlightDim = 36;
    ui.open(s);

    REQUIRE(seekRow(ui, RowId::Bright));
    dragUnits(ui, 2, -20); // BRIGHT to its floor
    CHECK(ui.live().backlightDim <= ui.live().backlightActive);
    CHECK(ui.live().backlightActive >= h0::kBacklightFloor);
}

TEST_CASE("RESET applies defaults live and cancel still undoes it") {
    SettingsUi ui;
    Settings s = h0::kDefaults;
    s.backlightActive = 255;
    s.themeId = 3;
    ui.open(s);

    REQUIRE(seekRow(ui, RowId::Defaults));
    dragUnits(ui, 2, 1); // KEEP -> RESET
    CHECK(ui.live().backlightActive == h0::kDefaults.backlightActive);

    CHECK(ui.cancel() == s);
}

TEST_CASE("closing clears the drag state so re-entry does not jump") {
    SettingsUi ui;
    ui.open(h0::kDefaults);
    ui.onDrag(1, true, 200);
    ui.cancel();

    ui.open(h0::kDefaults);
    CHECK(ui.rowOffsetPx() == 0);
    CHECK(ui.activeColumn() == 0);
    CHECK(ui.onDrag(1, true, 20) == false); // fresh reference, no movement
}

using h0::GestureGate;

TEST_CASE("a clean swipe fires exactly once") {
    GestureGate g;
    CHECK(g.onTouch(true, true, false, 1000));
    CHECK_FALSE(g.onTouch(false, false, false, 2000));
}

TEST_CASE("a latching gesture register still yields one command") {
    // The single most likely bug in the design: main.cpp polls every frame
    // while a finger is down, so a register that keeps reporting the same code
    // would open settings and commit them in the same gesture.
    GestureGate g;
    CHECK(g.onTouch(true, true, false, 1000));
    for (int i = 0; i < 20; ++i) CHECK_FALSE(g.onTouch(true, true, false, 1000 + i * 33));
}

TEST_CASE("a swipe is ignored once a wheel has moved in this touch") {
    GestureGate g;
    g.onTouch(true, false, true, 1000);  // a drag starts
    CHECK_FALSE(g.onTouch(true, true, false, 1100)); // sloppy diagonal
}

TEST_CASE("a release re-arms the gate") {
    GestureGate g;
    g.onTouch(true, false, true, 1000);
    g.onTouch(false, false, false, 1100);
    CHECK(g.onTouch(true, true, false, 1'000'000 + 1100));
}

TEST_CASE("the refractory stops one action producing two commands") {
    GestureGate g;
    CHECK(g.onTouch(true, true, false, 1000));
    g.onTouch(false, false, false, 1100);
    CHECK_FALSE(g.onTouch(true, true, false, 1100 + GestureGate::kRefractoryUs / 2));
    g.onTouch(false, false, false, 1200 + GestureGate::kRefractoryUs / 2);
    CHECK(g.onTouch(true, true, false, 1100 + GestureGate::kRefractoryUs + 1));
}

TEST_CASE("a gesture reported on the release read is still honoured") {
    // Task 0 settles whether the controller does this. The gate must work
    // either way, so that answer cannot invalidate the design.
    GestureGate g;
    g.onTouch(true, false, false, 1000);
    CHECK(g.onTouch(false, true, false, 1100));
}
