#include "faces/splitflap_face.hpp"

#include <1bit/fonts/flap_13x26.hpp>
#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/primitives.hpp>

#include <cstdio>
#include <cstring>

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

constexpr int16_t kCols = 5;      // M M : S S
constexpr int16_t kCellW = 21;    // 13 px glyph + 4 px each side
constexpr int16_t kCellH = 34;    // 26 px glyph + 4 px top and bottom
constexpr int16_t kBoardW = kCols * kCellW;  // 105
constexpr int16_t kBoardX = safe::X + (safe::W - kBoardW) / 2;
constexpr int16_t kBoardY = 120;

/// Descending, digits only, with the separator. See the header -- an ascending
/// or alphanumeric sequence makes every countdown tick cost 39 flaps.
///
/// ':' must be present. Without it `findInSequence` falls through to 0 and the
/// separator cell would display whatever character sits at index 0 -- a '9'
/// here. Including it is free: the colon reaches its target during the initial
/// settle and never flips again.
const char kSequence[] = "9876543210:";
constexpr int16_t kSequenceLen = 11;

onebit::SplitFlapConfig makeConfig() {
    onebit::SplitFlapConfig cfg;
    cfg.font = &onebit::fonts::FLAP_13X26;
    cfg.cell_width = kCellW;
    cfg.cell_height = kCellH;
    cfg.cols = kCols;
    cfg.rows = 1;
    cfg.bounds = {kBoardX, kBoardY, kBoardW, kCellH};
    cfg.flap_sequence = kSequence;
    cfg.sequence_length = kSequenceLen;
    // Worst transition is six flaps, so this must stay under ~166 ms to land
    // inside one second. 110 leaves comfortable margin.
    cfg.ms_per_flap = 110;
    cfg.split_line_thickness = 1;
    cfg.cell_borders = true;
    return cfg;
}

/// MM:SS, clamped. The board has five cells and cannot grow, so durations of an
/// hour or more are shown as minutes rather than switching format -- 99:59 is
/// the ceiling and anything above pins there.
void formatMMSS(uint32_t totalSeconds, char* out, size_t n) {
    uint32_t m = totalSeconds / 60;
    const uint32_t s = totalSeconds % 60;
    if (m > 99) { std::snprintf(out, n, "99:59"); return; }
    std::snprintf(out, n, "%02lu:%02lu", static_cast<unsigned long>(m),
                  static_cast<unsigned long>(s));
}

} // namespace

SplitFlapFace::SplitFlapFace() : board_(makeConfig()) {}

void SplitFlapFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    char want[8];
    formatMMSS(t.remainingSeconds(now), want, sizeof(want));

    if (std::strcmp(want, shown_) != 0) {
        board_.setRow(0, want);
        std::snprintf(shown_, sizeof(shown_), "%s", want);
    }

    // Derive the animation delta from the caller's clock rather than counting
    // frames, so the flaps run at a real cadence regardless of render rate.
    uint32_t deltaMs = 0;
    if (!settled_) {
        // Snap on the first frame instead of cascading through the sequence from
        // a cold start -- update() catches up in one call, so a large delta
        // settles the board rather than animating it.
        deltaMs = 5000;
        settled_ = true;
    } else if (now > lastNow_) {
        const uint64_t d = (now - lastNow_) / 1000ull;
        deltaMs = (d > 1000ull) ? 1000u : static_cast<uint32_t>(d);
    }
    lastNow_ = now;
    board_.update(deltaMs);

    // The library's render() only ever writes ink -- it never clears -- so
    // without this the glyphs accumulate on top of each other.
    fb.clear(WHITE);
    board_.render(fb);

    const char* label = nullptr;
    switch (t.state()) {
        case TimerModel::State::Idle:    label = "SET"; break;
        case TimerModel::State::Running: label = nullptr; break;
        case TimerModel::State::Paused:  label = "PAUSED"; break;
        case TimerModel::State::Expired: label = "DONE"; break;
    }
    if (label) {
        const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, label);
        onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                               static_cast<int16_t>(safe::X + (safe::W - w) / 2),
                               static_cast<int16_t>(kBoardY + kCellH + 18), label, BLACK);
    }
}

} // namespace h0
