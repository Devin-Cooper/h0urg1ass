#include "faces/settings_face.hpp"

#include <1bit/fonts/term_6x9.hpp>
#include <1bit/fonts/term_8x12.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>

#include <cstdio>

#include "settings/theme.hpp"

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

// Inherited from setting_face.cpp, which is proven on hardware.
constexpr int16_t COL_NAME_CX = 76;
constexpr int16_t COL_VALUE_CX = 164;
constexpr int16_t WIN_CY = 156;
constexpr int16_t WIN_HALF = 19;
constexpr int16_t PITCH = DragColumn::kPixelsPerUnit;

// Wider than the picker's 40: the value column's active bar has to span
// "GOOD 3.9v" at 8x12, which is 80 px.
constexpr int16_t COL_HALF = 45;

constexpr int16_t BIG_H = 12;   // TERM_8X12
constexpr int16_t SMALL_H = 9;  // TERM_6X9

static_assert(PITCH >= BIG_H + 4, "rows would overlap");

/// Rows are CENTRED on WIN_CY + k*PITCH, so the outer two are half-cut by the
/// band mask -- three whole rows and two sliced, exactly as the picker looks.
void centredText(onebit::IFramebuffer& fb, int16_t cx, int16_t cy, const char* s,
                 bool big) {
    const onebit::BitmapFont& f = big ? onebit::fonts::TERM_8X12 : onebit::fonts::TERM_6X9;
    const int16_t h = big ? BIG_H : SMALL_H;
    const int16_t w = onebit::getBitmapTextWidth(f, s);
    onebit::drawBitmapText(fb, f, static_cast<int16_t>(cx - w / 2),
                           static_cast<int16_t>(cy - h / 2), s, BLACK);
}

void maskOutsideBand(onebit::IFramebuffer& fb, int16_t top, int16_t bottom) {
    onebit::fillRect(fb, 0, 0, 240, top, WHITE);
    onebit::fillRect(fb, 0, bottom, 240, static_cast<int16_t>(280 - bottom), WHITE);
}

} // namespace

void SettingsFace::formatValue(RowId id, uint8_t index, const Settings& s,
                               const BatteryReading& bat, char* out, size_t n) {
    // DEFAULTS is the only row whose label is not a function of Settings.
    if (const char* label = ladderLabel(id, index)) {
        std::snprintf(out, n, "%s", label);
        return;
    }

    switch (id) {
        case RowId::Theme:
            std::snprintf(out, n, "%s", themeFor(static_cast<ThemeId>(s.themeId)).name);
            return;
        case RowId::Bright:
            std::snprintf(out, n, "%u", static_cast<unsigned>(s.backlightActive));
            return;
        case RowId::DimTo:
            std::snprintf(out, n, "%u", static_cast<unsigned>(s.backlightDim));
            return;
        case RowId::DimAt:
            if (s.dimAfterS == 0) { std::snprintf(out, n, "NEVER"); return; }
            if (s.dimAfterS < 60) { std::snprintf(out, n, "%us", s.dimAfterS); return; }
            std::snprintf(out, n, "%um", static_cast<unsigned>(s.dimAfterS / 60));
            return;
        case RowId::BlankAt:
            if (s.blankAfterS == 0) { std::snprintf(out, n, "NEVER"); return; }
            if (s.blankAfterS < 60) { std::snprintf(out, n, "%us", s.blankAfterS); return; }
            std::snprintf(out, n, "%um", static_cast<unsigned>(s.blankAfterS / 60));
            return;
        case RowId::Alarm:
            if (s.alarmS < 60) { std::snprintf(out, n, "%us", s.alarmS); return; }
            std::snprintf(out, n, "%um", static_cast<unsigned>(s.alarmS / 60));
            return;
        case RowId::Sound:
            std::snprintf(out, n, "%s", s.mute ? "MUTE" : "ON");
            return;
        case RowId::Battery:
            if (!bat.valid) { std::snprintf(out, n, "--"); return; }
            if (!bat.calibrated) {
                // Checked BEFORE charging: an uncalibrated divider reads up to
                // +9% high, which is enough on its own to put a full 4.10 V
                // cell at an apparent 4.47 V -- so a charging check ahead of
                // this one would claim CHARGING on a device that is not.
                // Uncalibrated the gain error is +/-9%, which at 3.9 V is
                // +/-0.35 V -- WIDER THAN THE WHOLE BUCKET RANGE. So the bucket
                // is not merely imprecise, it is uninformative, and printing
                // "GOOD" beside a caveat would still be read as "GOOD". Section
                // 7.2: "not a fuel gauge, it is a rumour."
                std::snprintf(out, n, "UNCAL");
                return;
            }
            if (bat.charging) { std::snprintf(out, n, "CHARGING"); return; }
            // One decimal: the honest resolution against a +/-74 mV calibrated
            // residual. Nine characters at 8x12 is the column's widest value.
            std::snprintf(out, n, "%s %u.%uv", bucketName(bucketFor(bat.milliVolts)),
                          static_cast<unsigned>(bat.milliVolts / 1000),
                          static_cast<unsigned>((bat.milliVolts / 100) % 10));
            return;
        case RowId::Cal:
            if (s.batCalPermille <= kCalMin || s.batCalPermille >= kCalMax) {
                // A unit needing more than the range has a divider or LDO out
                // of spec. Say so rather than clamping silently and calling it
                // calibrated.
                std::snprintf(out, n, "AT LIMIT");
                return;
            }
            if (!bat.valid) { std::snprintf(out, n, "--"); return; }
            {
                // The VOLTAGE this gain produces, not the gain itself -- the
                // voltage is what you match against a meter, and the multiplier
                // is an implementation detail nobody can check against anything.
                // Two decimals, and only here: the number is used relatively, so
                // the resolution is legitimate (section 9.1).
                const uint16_t mv = applyCal(bat.rawMilliVolts, s.batCalPermille);
                std::snprintf(out, n, "%u.%02uv", static_cast<unsigned>(mv / 1000),
                              static_cast<unsigned>((mv / 10) % 100));
            }
            return;
        case RowId::Defaults: // handled by ladderLabel above
        case RowId::Count:
            break;
    }
    std::snprintf(out, n, "?");
}

void SettingsFace::renderAt(onebit::IFramebuffer& fb, const SettingsUi& ui,
                            const BatteryReading& bat) {
    fb.clear(WHITE);

    constexpr int16_t BAND_TOP = static_cast<int16_t>(WIN_CY - 2 * PITCH);
    constexpr int16_t BAND_BOT = static_cast<int16_t>(WIN_CY + 2 * PITCH);

    const Settings& s = ui.live();
    const int n = rowCount();

    // The name wheel: five rows around the selection, sliding with the drag.
    for (int k = -2; k <= 2; ++k) {
        const int16_t y = static_cast<int16_t>(WIN_CY + k * PITCH + ui.rowOffsetPx());
        const int idx = ((ui.rowIndex() + k) % n + n) % n;
        const bool selected = (y > WIN_CY - PITCH / 2) && (y <= WIN_CY + PITCH / 2);
        centredText(fb, COL_NAME_CX, y, rowName(static_cast<RowId>(idx)), selected);
    }

    // The value wheel: entries of the SELECTED row only, and it CLAMPS, so the
    // rows beyond an end are simply blank -- which is the cue that you have
    // reached one.
    const RowId cur = ui.currentRow();
    const uint8_t size = ladderSize(cur);
    if (size > 0) {
        const int here = ladderIndex(cur, s);
        for (int k = -2; k <= 2; ++k) {
            const int idx = here + k;
            if (idx < 0 || idx >= size) continue;
            const int16_t y = static_cast<int16_t>(WIN_CY + k * PITCH + ui.valueOffsetPx());
            Settings probe = s;
            applyLadder(cur, static_cast<uint8_t>(idx), probe);
            char buf[12];
            formatValue(cur, static_cast<uint8_t>(idx), probe, bat, buf, sizeof(buf));
            const bool selected = (y > WIN_CY - PITCH / 2) && (y <= WIN_CY + PITCH / 2);
            centredText(fb, COL_VALUE_CX, y, buf, selected);
        }
    } else {
        char buf[12];
        formatValue(cur, 0, s, bat, buf, sizeof(buf));
        centredText(fb, COL_VALUE_CX, WIN_CY, buf, true);
    }

    maskOutsideBand(fb, BAND_TOP, BAND_BOT);

    // The selection window. These two rules carry the emphasis: TERM_8X12 over
    // TERM_6X9 is only a 33% height bump, nothing like the picker's 30-vs-18
    // vector contrast, so the font swap alone would not read as "selected".
    for (int16_t d = 0; d < 2; ++d) {
        onebit::drawLine(fb, 24, static_cast<int16_t>(WIN_CY - WIN_HALF + d), 216,
                         static_cast<int16_t>(WIN_CY - WIN_HALF + d), BLACK);
        onebit::drawLine(fb, 24, static_cast<int16_t>(WIN_CY + WIN_HALF - d), 216,
                         static_cast<int16_t>(WIN_CY + WIN_HALF - d), BLACK);
    }
    // NOTE: no centre dots. The picker's two at x=120 sit inside this face's
    // value column, which reaches x=124.

    if (ui.activeColumn() == 1 || ui.activeColumn() == 2) {
        const int16_t cx = (ui.activeColumn() == 1) ? COL_NAME_CX : COL_VALUE_CX;
        const int16_t x0 = static_cast<int16_t>(cx - COL_HALF);
        const int16_t w = static_cast<int16_t>(2 * COL_HALF);
        onebit::fillRect(fb, x0, static_cast<int16_t>(WIN_CY - WIN_HALF), w, 4, BLACK);
        onebit::fillRect(fb, x0, static_cast<int16_t>(WIN_CY + WIN_HALF - 3), w, 4, BLACK);
    }

    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(COL_NAME_CX - 24), 70, "SETTING", BLACK);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(COL_VALUE_CX - 17), 70, "VALUE", BLACK);

    // 20 chars at TERM_6X9 with char_spacing 1 = 139 px, spanning x 50..190 --
    // inside x [44,196], so layout.hpp's one-line clip shortcut applies and no
    // per-row disc arithmetic is needed.
    const char* hint = "SWIPE=SAVE LIFT=UNDO";
    const int16_t hw = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, hint);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(120 - hw / 2), 240, hint, BLACK);
}

} // namespace h0
