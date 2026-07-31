#include "faces/power_face.hpp"

#include <1bit/fonts/term_8x12.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

constexpr int16_t CX = 120;
constexpr int16_t LINE1_Y = 118;
constexpr int16_t LINE2_Y = 136;
constexpr int16_t BAR_X = 40, BAR_W = 160, BAR_Y = 168, BAR_H = 18;

void centred(onebit::IFramebuffer& fb, int16_t y, const char* s) {
    const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_8X12, s);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_8X12,
                           static_cast<int16_t>(CX - w / 2), y, s, BLACK);
}

} // namespace

void PowerFace::renderAt(onebit::IFramebuffer& fb, PowerAction action, uint8_t progress) {
    fb.clear(WHITE);

    bool bar = false;
    switch (action) {
        case PowerAction::PromptHold:
            centred(fb, LINE1_Y, "HOLD TO");
            centred(fb, LINE2_Y, "POWER OFF");
            bar = true;
            break;
        case PowerAction::PromptTimerRunning:
            centred(fb, LINE1_Y, "TIMER RUNNING");
            centred(fb, LINE2_Y, "KEEP HOLDING");
            bar = true;
            break;
        case PowerAction::PromptRelease:
            centred(fb, LINE1_Y, "RELEASE TO");
            centred(fb, LINE2_Y, "POWER OFF");
            break;
        case PowerAction::UsbCannotPowerOff:
            centred(fb, LINE1_Y, "ON USB");
            centred(fb, LINE2_Y, "CANNOT POWER OFF");
            break;
        default:
            return; // nothing to draw
    }

    if (bar) {
        onebit::drawRect(fb, BAR_X, BAR_Y, BAR_W, BAR_H, BLACK);
        const int16_t fill =
            static_cast<int16_t>((static_cast<int>(progress) * (BAR_W - 6)) / 255);
        if (fill > 0) {
            onebit::fillRect(fb, static_cast<int16_t>(BAR_X + 3),
                             static_cast<int16_t>(BAR_Y + 3), fill,
                             static_cast<int16_t>(BAR_H - 6), BLACK);
        }
    }
}

} // namespace h0
