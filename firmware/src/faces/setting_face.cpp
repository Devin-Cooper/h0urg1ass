#include "faces/setting_face.hpp"

#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>
#include <1bit/render/vector_font.hpp>

#include <cstdio>

#include "input/drag_column.hpp"

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

constexpr int16_t COL_M_CX = 76;  ///< minutes column centre
constexpr int16_t COL_S_CX = 164; ///< seconds column centre
constexpr int16_t COL_HALF = 40;  ///< half the touch/draw width of a column

constexpr int16_t WIN_CY = 156;   ///< the selected row
constexpr int16_t WIN_HALF = 19;  ///< half the selection window height

/// Row pitch, in step with the drag. If these disagree the wheel slides at a
/// different rate than the finger, which reads as lag that no amount of
/// smoothing will fix.
constexpr int16_t PITCH = DragColumn::kPixelsPerUnit;

constexpr int16_t BIG_W = 22, BIG_H = 30, BIG_STROKE = 3;
constexpr int16_t SMALL_W = 13, SMALL_H = 18, SMALL_STROKE = 2;

// The pitch must clear the tallest glyph or the rows collide into each other.
static_assert(DragColumn::kPixelsPerUnit >= BIG_H + 4, "rows would overlap");

void centredNumber(onebit::IFramebuffer& fb, int16_t cx, int16_t cy, uint32_t v, bool big) {
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02lu", static_cast<unsigned long>(v % 100));
    const int16_t w = big ? BIG_W : SMALL_W;
    const int16_t h = big ? BIG_H : SMALL_H;
    const int16_t st = big ? BIG_STROKE : SMALL_STROKE;
    const int16_t sp = big ? 4 : 3;
    const int16_t tw = onebit::getStringWidth(buf, w, sp);
    onebit::renderString(fb, buf, static_cast<int16_t>(cx - tw / 2),
                         static_cast<int16_t>(cy - h / 2), w, h, sp, st, BLACK);
}

/// One spinner column: the selected value and its neighbours, sliding with the
/// drag.
///
/// `wrap` is the modulus, so the wheel is endless in both directions rather
/// than stopping at a boundary the user cannot see coming.
void column(onebit::IFramebuffer& fb, int16_t cx, uint32_t value, int16_t offset,
            uint32_t wrap) {
    for (int k = -2; k <= 2; ++k) {
        const int16_t y = static_cast<int16_t>(WIN_CY + k * PITCH + offset);

        // Values arrive from below when dragging up, so a row below the centre
        // holds a HIGHER value. Adding `wrap` before the modulus keeps the
        // operand positive rather than relying on signed-modulus behaviour.
        const uint32_t v =
            (value + wrap + static_cast<uint32_t>(static_cast<int32_t>(k))) % wrap;

        // "Selected" is decided by position, not index: mid-drag, the neighbour
        // that has slid into the window is the one that should look chosen.
        const bool selected = (y > WIN_CY - PITCH / 2) && (y <= WIN_CY + PITCH / 2);
        centredNumber(fb, cx, y, v, selected);
    }
}

/// Erase everything outside the wheel band.
///
/// Clipping by overdraw, because the pinned graphics library has no clip
/// rectangle. Without it the neighbouring rows slide over the window rules and
/// the labels, which reads as a rendering fault rather than as a wheel.
void maskOutsideBand(onebit::IFramebuffer& fb, int16_t top, int16_t bottom) {
    onebit::fillRect(fb, 0, 0, 240, top, WHITE);
    onebit::fillRect(fb, 0, bottom, 240, static_cast<int16_t>(280 - bottom), WHITE);
}

} // namespace

void SettingFace::renderAt(onebit::IFramebuffer& fb, const PickerState& s) {
    fb.clear(WHITE);

    constexpr int16_t BAND_TOP = static_cast<int16_t>(WIN_CY - 2 * PITCH);
    constexpr int16_t BAND_BOT = static_cast<int16_t>(WIN_CY + 2 * PITCH);

    // Wheels first, then erase outside the band, then draw the furniture on top
    // of the clean edge.
    column(fb, COL_M_CX, s.minutes, s.minutesOffset, 60);
    column(fb, COL_S_CX, s.seconds, s.secondsOffset, 60);
    maskOutsideBand(fb, BAND_TOP, BAND_BOT);

    // The selection window. Bold, because these two rules are the only thing
    // saying which row is the value.
    for (int16_t d = 0; d < 2; ++d) {
        onebit::drawLine(fb, 24, static_cast<int16_t>(WIN_CY - WIN_HALF + d), 216,
                         static_cast<int16_t>(WIN_CY - WIN_HALF + d), BLACK);
        onebit::drawLine(fb, 24, static_cast<int16_t>(WIN_CY + WIN_HALF - d), 216,
                         static_cast<int16_t>(WIN_CY + WIN_HALF - d), BLACK);
    }

    onebit::fillCircle(fb, 120, static_cast<int16_t>(WIN_CY - 9), 3, BLACK);
    onebit::fillCircle(fb, 120, static_cast<int16_t>(WIN_CY + 9), 3, BLACK);

    // Mark the dragged column by thickening the window rules across it, rather
    // than underlining below the window -- an underline lands exactly where the
    // next row sits and collides with it.
    if (s.activeColumn == 1 || s.activeColumn == 2) {
        const int16_t cx = (s.activeColumn == 1) ? COL_M_CX : COL_S_CX;
        const int16_t x0 = static_cast<int16_t>(cx - COL_HALF);
        const int16_t w = static_cast<int16_t>(2 * COL_HALF);
        onebit::fillRect(fb, x0, static_cast<int16_t>(WIN_CY - WIN_HALF), w, 4, BLACK);
        onebit::fillRect(fb, x0, static_cast<int16_t>(WIN_CY + WIN_HALF - 3), w, 4, BLACK);
    }

    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(COL_M_CX - 9), 70, "MIN", BLACK);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(COL_S_CX - 9), 70, "SEC", BLACK);

    // Hours only when there are any. An always-present "0 h" is noise on a timer
    // that is usually minutes.
    if (s.hours > 0) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%lu h", static_cast<unsigned long>(s.hours));
        const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, buf);
        onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                               static_cast<int16_t>(120 - w / 2), 46, buf, BLACK);
    }

    const char* hint = "DRAG TO SET";
    const int16_t hw = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, hint);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(120 - hw / 2), 240, hint, BLACK);
}

void SettingFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    const uint32_t total = t.remainingSeconds(now);
    PickerState s;
    s.hours = total / 3600;
    s.minutes = (total % 3600) / 60;
    s.seconds = total % 60;
    renderAt(fb, s);
}

} // namespace h0
