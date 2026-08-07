#include "faces/boot_face.hpp"

#include <1bit/fonts/term_8x12.hpp>
#include <1bit/render/bitmap_font.hpp>

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

constexpr int16_t CX = 120;

// TERM_8X12 advances 8 px, so the 9-character wordmark is 72 px and spans
// x in [84, 156]. That is inside h0::safe (16, 16, 208, 248) and inside
// x in [44, 196], which clears both rounded-corner quadrants at every y.
constexpr int16_t WORDMARK_Y = 134;

} // namespace

void BootFace::renderAt(onebit::IFramebuffer& fb) {
    fb.clear(WHITE);
    const char* s = "h0urg1ass";
    const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_8X12, s);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_8X12,
                           static_cast<int16_t>(CX - w / 2), WORDMARK_Y, s, BLACK);
}

} // namespace h0
