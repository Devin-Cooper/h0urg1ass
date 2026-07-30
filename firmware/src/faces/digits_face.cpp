#include "faces/digits_face.hpp"

#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>
#include <1bit/render/vector_font.hpp>

#include <cstdio>

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

// Vector-font metrics for the countdown. The glyphs are stroked polylines in a
// 0-100 space, so size is a free parameter -- unlike the bitmap fonts, whose
// largest cell is 26 px and nowhere near enough for a glanceable clock.
constexpr int16_t kCharW = 40;
constexpr int16_t kCharH = 62;
constexpr int16_t kSpacing = 5;
constexpr int16_t kStroke = 5;

/// Format remaining time for the big readout.
///
/// `MM:SS` under an hour, `H:MM:SS` above it. The blink replaces the separator
/// with a space rather than removing it, so the layout cannot shift: the vector
/// font gives ':' and ' ' the same width multiplier, and dropping the character
/// entirely would make the digits jump sideways once a second.
///
/// The hours form carries seconds deliberately. `H:MM` would render one hour as
/// "1:00", which is indistinguishable from one minute -- and a second later it
/// becomes "59:59", so the number appears to jump from 1 to 59.
void formatTime(uint32_t totalSeconds, char* out, size_t n, bool showColon) {
    const char colon = showColon ? ':' : ' ';
    if (totalSeconds >= 3600) {
        const uint32_t h = totalSeconds / 3600;
        const uint32_t m = (totalSeconds % 3600) / 60;
        const uint32_t s = totalSeconds % 60;
        std::snprintf(out, n, "%lu%c%02lu%c%02lu", static_cast<unsigned long>(h), colon,
                      static_cast<unsigned long>(m), colon, static_cast<unsigned long>(s));
    } else {
        const uint32_t m = totalSeconds / 60;
        const uint32_t s = totalSeconds % 60;
        std::snprintf(out, n, "%02lu%c%02lu", static_cast<unsigned long>(m), colon,
                      static_cast<unsigned long>(s));
    }
}

/// Centre a vector string horizontally in the safe area and draw it, shrinking
/// it if it would otherwise overflow.
///
/// Without the shrink, anything from `100:00` upward runs past the safe box and
/// off the panel -- at 1000 hours the glyphs are silently clipped by the
/// framebuffer's own bounds check, which looks like a rendering fault rather
/// than a layout one. Scaling keeps long durations legible and inside the glass.
void drawCentred(onebit::IFramebuffer& fb, const char* text, int16_t y, onebit::Color c) {
    int16_t cw = kCharW, sp = kSpacing, st = kStroke;
    int16_t w = onebit::getStringWidth(text, cw, sp);
    while (w > safe::W && cw > 10) {
        cw = static_cast<int16_t>(cw - 2);
        sp = static_cast<int16_t>((sp > 2) ? sp - 1 : sp);
        if (st > 2) st = static_cast<int16_t>(st - 1);
        w = onebit::getStringWidth(text, cw, sp);
    }
    // Scale the height with the width so the digits keep their proportions.
    const int16_t ch = static_cast<int16_t>(kCharH * cw / kCharW);
    const int16_t x = static_cast<int16_t>(safe::X + (safe::W - w) / 2);
    onebit::renderString(fb, text, x, y, cw, ch, sp, st, c);
}

void drawLabelCentred(onebit::IFramebuffer& fb, const char* text, int16_t y, onebit::Color c) {
    const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, text);
    const int16_t x = static_cast<int16_t>(safe::X + (safe::W - w) / 2);
    onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9, x, y, text, c);
}

} // namespace

void DigitsFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    const bool expired = t.isExpired();

    // Expiry inverts the whole screen. On a 1-bit panel there is no colour and
    // no brightness to signal with, so the largest available signal is the
    // field itself -- and it reads from across a room, which is the point.
    const onebit::Color bg = expired ? BLACK : WHITE;
    const onebit::Color fg = expired ? WHITE : BLACK;
    fb.clear(bg);

    // The colon blinks once a second while running and is steady otherwise.
    // That is the "is it counting?" tell, and it costs nothing: a stopped clock
    // with a frozen colon is a universally understood idiom.
    bool colon = true;
    if (t.isRunning()) {
        colon = ((now / 500'000ull) % 2ull) == 0ull;
    }

    char buf[16];
    formatTime(t.remainingSeconds(now), buf, sizeof(buf), colon);

    constexpr int16_t kDigitsY = 96;
    drawCentred(fb, buf, kDigitsY, fg);

    const char* label = nullptr;
    switch (t.state()) {
        case TimerModel::State::Idle:    label = "SET"; break;
        case TimerModel::State::Running: label = nullptr; break;
        case TimerModel::State::Paused:  label = "PAUSED"; break;
        case TimerModel::State::Expired: label = "DONE"; break;
    }
    if (label) {
        drawLabelCentred(fb, label, static_cast<int16_t>(kDigitsY + kCharH + 22), fg);
    }

    // A progress rule under the digits. Cheap, and it gives the eye something
    // continuous to read when the seconds are changing too slowly to notice.
    if (t.duration() > 0 && !expired) {
        constexpr int16_t kBarY = 200;
        constexpr int16_t kBarH = 6;
        onebit::drawRect(fb, safe::X, kBarY, safe::W, kBarH, fg);
        const int16_t inner = static_cast<int16_t>(safe::W - 4);
        const int16_t filled = static_cast<int16_t>(static_cast<float>(inner) * t.fraction(now));
        if (filled > 0) {
            onebit::fillRect(fb, static_cast<int16_t>(safe::X + 2),
                             static_cast<int16_t>(kBarY + 2), filled,
                             static_cast<int16_t>(kBarH - 4), fg);
        }
    }
}

bool DigitsFace::supports(const TimerModel& t) const {
    (void)t;
    return true; // digits are meaningful for every state, including no duration
}

} // namespace h0
