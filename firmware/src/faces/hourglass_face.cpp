#include "faces/hourglass_face.hpp"

#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>

#include <1bit/render/primitives.hpp>

#include <cstdio>

namespace h0 {

using onebit::BLACK;
using onebit::WHITE;

namespace {

/// Simulation rate, independent of the frame rate.
///
/// One tick costs about 4.5 ms on this part, so 30 Hz is roughly 13% of the
/// CPU -- affordable, and enough for the drain to look continuous. 60 Hz drains
/// short timers more cleanly but doubles that, so it waits until the tick is
/// cheaper.
constexpr uint64_t kTickPeriodUs = 33'333;

/// Grains, by duration. Both ends of the range are constrained, in opposite
/// directions, so no single charge serves them.
int chargeFor(uint64_t durationUs) {
    const uint64_t s = durationUs / 1'000'000ull;
    if (s <= 60) return 400;
    if (s <= 180) return 900;
    return 3000;
}

} // namespace

void HourglassFace::restart(const TimerModel& t, uint32_t seed) {
    vessel_.begin();
    vessel_.reset(seed, chargeFor(t.duration()));
    lastDuration_ = t.duration();
    started_ = true;
}

void HourglassFace::tick(const TimerModel& t, uint64_t now) {
    if (!started_ || t.duration() != lastDuration_) {
        // A new duration needs a new charge; carrying the old one over would
        // drain at the wrong rate for the whole run.
        restart(t, static_cast<uint32_t>(now | 1u));
        lastTickUs_ = now;
        return;
    }

    // Fixed-rate, catching up at most a few ticks. Without a cap a long stall --
    // a slow frame, a bus recovery -- would try to replay the whole gap at once
    // and stall the UI further.
    if (now < lastTickUs_) lastTickUs_ = now;
    int budget = 3;
    while (now - lastTickUs_ >= kTickPeriodUs && budget-- > 0) {
        lastTickUs_ += kTickPeriodUs;
        vessel_.tick(t.fraction(now));
    }
    if (now - lastTickUs_ > kTickPeriodUs * 8) lastTickUs_ = now; // gave up catching up
}

void HourglassFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    (void)now; // the sand carries the time; the label only needs the state
    fb.clear(WHITE);
    renderSand(fb, vessel_.sand(), vessel_.walls());

    const char* label = nullptr;
    switch (t.state()) {
        case TimerModel::State::Idle:    label = "SET"; break;
        case TimerModel::State::Running: label = nullptr; break;
        case TimerModel::State::Paused:  label = "PAUSED"; break;
        case TimerModel::State::Expired: label = "DONE"; break;
    }
    if (label) {
        // On the floor line, which is the one horizontal band guaranteed clear
        // of sand.
        const int16_t w = onebit::getBitmapTextWidth(onebit::fonts::TERM_6X9, label);
        const int16_t y = static_cast<int16_t>(sandgeom::ORIGIN_Y +
                                               sandgeom::FLOOR_ROW * sandgeom::SCALE - 14);
        onebit::fillRect(fb, static_cast<int16_t>(120 - w / 2 - 5), static_cast<int16_t>(y - 2),
                         static_cast<int16_t>(w + 10), 13, WHITE);
        onebit::drawBitmapText(fb, onebit::fonts::TERM_6X9,
                               static_cast<int16_t>(120 - w / 2), y, label, BLACK);
    }
}

} // namespace h0
