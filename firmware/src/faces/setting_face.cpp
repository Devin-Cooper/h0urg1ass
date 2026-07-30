#include "faces/setting_face.hpp"

#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>
#include <1bit/render/vector_font.hpp>

#include <cmath>
#include <cstdio>

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

// Must match input/dial.hpp's DialConfig, or the drawing lies about where the
// zones are -- which is worse than not drawing them at all.
constexpr int16_t CX = 120;
constexpr int16_t CY = 140;
constexpr int16_t R_DEAD = 30;   ///< inside this, the dial ignores you
constexpr int16_t R_SPLIT = 72;  ///< outside this, a step is a minute
constexpr int16_t R_RING = 92;   ///< where the scale is drawn
constexpr int16_t R_ORBIT = 66;  ///< seconds dot, and the fine-zone indicator
constexpr int16_t R_ZONE_OUT = 80; ///< coarse-zone indicator

// Both indicator radii sit clear of the centre readout, which spans roughly
// +-55 px. A ring drawn through the numbers is worse than no ring.
static_assert(R_ORBIT > 60, "fine-zone indicator would cross the readout");
static_assert(R_ZONE_OUT > R_SPLIT && R_ZONE_OUT < R_RING,
              "coarse indicator must sit between the split and the scale");

constexpr float kPi = 3.14159265358979323846f;

/// Angle for mark `i` of 60, measured so that zero is at the top and the count
/// runs clockwise -- the direction a right hand turns a knob, and the direction
/// a clock face already trains everyone to read.
float markAngle(int i) {
    return -kPi / 2.0f + (2.0f * kPi * static_cast<float>(i) / 60.0f);
}

void radialMark(onebit::IFramebuffer& fb, float a, int16_t rInner, int16_t rOuter,
                bool thick) {
    const float c = std::cos(a), s = std::sin(a);
    const int16_t x0 = static_cast<int16_t>(CX + c * static_cast<float>(rInner));
    const int16_t y0 = static_cast<int16_t>(CY + s * static_cast<float>(rInner));
    const int16_t x1 = static_cast<int16_t>(CX + c * static_cast<float>(rOuter));
    const int16_t y1 = static_cast<int16_t>(CY + s * static_cast<float>(rOuter));
    onebit::drawLine(fb, x0, y0, x1, y1, BLACK);
    if (thick) {
        // Stamp a second line one pixel across rather than using drawThickLine,
        // which leaves gaps on diagonals (1bit-display#10) -- exactly the case
        // most of these marks are.
        onebit::drawLine(fb, static_cast<int16_t>(x0 + 1), y0,
                         static_cast<int16_t>(x1 + 1), y1, BLACK);
    }
}

/// A ring of short dashes. Solid circles at this size read as a hard boundary;
/// dashes read as a guide, which is what these are.
void dashedCircle(onebit::IFramebuffer& fb, int16_t r) {
    for (int i = 0; i < 60; ++i) {
        if (i % 2) continue;
        const float a = markAngle(i);
        const int16_t x = static_cast<int16_t>(CX + std::cos(a) * static_cast<float>(r));
        const int16_t y = static_cast<int16_t>(CY + std::sin(a) * static_cast<float>(r));
        fb.setPixel(x, y, BLACK);
    }
}

} // namespace

void SettingFace::renderAt(onebit::IFramebuffer& fb, uint32_t totalSeconds, int16_t touchR) {
    fb.clear(WHITE);

    const uint32_t minutes = totalSeconds / 60;
    const uint32_t seconds = totalSeconds % 60;
    const int filled = static_cast<int>(minutes % 60);

    // The scale. Marks up to the current minute are long and heavy, the rest
    // short -- a progress ring you can also count.
    for (int i = 0; i < 60; ++i) {
        const bool major = (i % 5) == 0;
        const bool lit = (i <= filled) && (minutes > 0);
        const int16_t len = lit ? 14 : (major ? 10 : 5);
        radialMark(fb, markAngle(i), static_cast<int16_t>(R_RING - len), R_RING, lit || major);
    }

    // The coarse/fine boundary, as a permanent guide.
    //
    // Deliberately NOT drawing the dead-zone radius: at 30 px it runs straight
    // through the readout, and a ring bisecting the numbers is worse than no
    // indicator at all. The centre being inert is discovered instantly anyway --
    // nothing happens.
    dashedCircle(fb, R_SPLIT);

    // Highlight whichever zone the finger is in, so moving between coarse and
    // fine is visible while it happens rather than only in the resulting number.
    // Both indicator radii are chosen to clear the centre text.
    if (touchR >= R_SPLIT) {
        onebit::drawCircle(fb, CX, CY, R_ZONE_OUT, BLACK);
    } else if (touchR >= R_DEAD) {
        onebit::drawCircle(fb, CX, CY, R_ORBIT, BLACK);
    }

    // Seconds as a single travelling dot. A second ring of marks would compete
    // with the minute scale; one dot cannot be misread.
    if (seconds > 0) {
        const float a = markAngle(static_cast<int>(seconds));
        const int16_t sx = static_cast<int16_t>(CX + std::cos(a) * static_cast<float>(R_ORBIT));
        const int16_t sy = static_cast<int16_t>(CY + std::sin(a) * static_cast<float>(R_ORBIT));
        onebit::fillCircle(fb, sx, sy, 3, BLACK);
    }

    char buf[12];
    std::snprintf(buf, sizeof(buf), "%02lu:%02lu", static_cast<unsigned long>(minutes),
                  static_cast<unsigned long>(seconds));
    const int16_t w = onebit::getStringWidth(buf, 22, 4);
    onebit::renderString(fb, buf, static_cast<int16_t>(CX - w / 2),
                         static_cast<int16_t>(CY - 16), 22, 34, 4, 3, BLACK);

    const char* hint = "DRAG TO SET";
    const int16_t hw = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, hint);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                           static_cast<int16_t>(CX - hw / 2),
                           static_cast<int16_t>(CY + 34), hint, BLACK);
}

void SettingFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    renderAt(fb, t.remainingSeconds(now), -1);
}

} // namespace h0
