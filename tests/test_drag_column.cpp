#include "doctest.h"

#include "input/drag_column.hpp"

using h0::DragColumn;

namespace {

constexpr int16_t PPU = DragColumn::kPixelsPerUnit;

/// Drag from `y0` to `y1` in `samples` steps, summing the reported movement.
/// Sample count controls speed, which matters: the column accelerates.
int drag(DragColumn& c, int16_t y0, int16_t y1, int samples) {
    int total = 0;
    for (int i = 0; i <= samples; ++i) {
        const int16_t y = static_cast<int16_t>(
            y0 + (static_cast<int>(y1 - y0) * i) / samples);
        total += c.update(true, y);
    }
    return total;
}

} // namespace

TEST_CASE("dragging up increases, dragging down decreases") {
    // The wheel turns with the finger. Getting this backwards is the single most
    // jarring thing a picker can do.
    DragColumn up, down;
    CHECK(drag(up, 200, 100, 100) > 0);
    CHECK(drag(down, 100, 200, 100) < 0);
}

TEST_CASE("a slow drag of one pitch moves exactly one unit") {
    // Slow enough to stay in the 1x gain band, so this pins the base scale.
    DragColumn c;
    CHECK(drag(c, 200, static_cast<int16_t>(200 - PPU), PPU) == 1);
}

TEST_CASE("the first sample of a drag moves nothing") {
    // Otherwise every tap jumps the value by wherever the finger landed.
    DragColumn c;
    CHECK(c.update(true, 150) == 0);
    CHECK(c.tracking());
    CHECK(c.offsetPx() == 0);
}

TEST_CASE("releasing stops tracking and a new touch does not jump") {
    DragColumn c;
    drag(c, 200, 150, 50);
    CHECK(c.update(false, 0) == 0);
    CHECK_FALSE(c.tracking());
    CHECK(c.offsetPx() == 0); // no stale offset left to render

    // A fresh touch far away is a new reference, not a delta against the old.
    CHECK(c.update(true, 20) == 0);
}

TEST_CASE("sub-unit movement is carried, not discarded") {
    // Nudges below one pitch must accumulate across calls. Without the residual
    // each truncates to zero and the wheel feels dead until a single sample
    // happens to clear a whole pitch.
    //
    // 2 px per sample stays in the 1x gain band, so this isolates the carry from
    // the acceleration -- at PPU/3 per nudge the samples are fast enough to be
    // scaled 4x and the test would measure the wrong thing.
    DragColumn c;
    c.update(true, 200);
    int total = 0;
    int16_t y = 200;
    for (int i = 0; i < PPU / 2; ++i) {
        y = static_cast<int16_t>(y - 2);
        total += c.update(true, y);
    }
    CHECK(total == 1);
}

TEST_CASE("the offset moves between steps, in the drag direction") {
    // This is what makes it feel like a wheel rather than a counter: the digits
    // must move continuously, not only when a step lands.
    //
    // The offset carries the GAIN-SCALED movement, not the raw finger travel --
    // it has to, or the animation and the value would disagree during a flick,
    // with the wheel visibly lagging the number it is showing. So it is at least
    // as large as the finger moved, and in the same direction.
    DragColumn c;
    c.update(true, 200);
    c.update(true, static_cast<int16_t>(200 - 3)); // 3 px up, well under a pitch
    CHECK(c.offsetPx() < 0);
    CHECK(c.offsetPx() <= -3);
    CHECK(c.tracking());
}

TEST_CASE("a flick covers more ground than a careful drag") {
    // The whole point of acceleration. Twenty-five minutes at one unit per
    // 34 px would otherwise be four full-height drags.
    DragColumn slow, fast;
    const int slowSteps = drag(slow, 240, 40, 200); // 1 px per sample
    const int fastSteps = drag(fast, 240, 40, 10);  // 20 px per sample
    CHECK(fastSteps > slowSteps * 3);
}

TEST_CASE("a careful drag still gives single-unit precision") {
    // Acceleration must not cost fine control, or the picker becomes unusable
    // for the last few units.
    DragColumn c;
    c.update(true, 200);
    int16_t y = 200;
    int total = 0;
    for (int i = 0; i < PPU; ++i) { // 1 px at a time: always the 1x band
        y = static_cast<int16_t>(y - 1);
        total += c.update(true, y);
    }
    CHECK(total == 1);
}

TEST_CASE("a stationary finger produces nothing") {
    DragColumn c;
    c.update(true, 150);
    int total = 0;
    for (int i = 0; i < 500; ++i) total += c.update(true, 150);
    CHECK(total == 0);
    CHECK(c.offsetPx() == 0);
}

TEST_CASE("one-pixel jitter never accumulates into a step") {
    // A resting thumb wobbles. Over a long hold that must not drift the value.
    DragColumn c;
    c.update(true, 150);
    int total = 0;
    for (int i = 0; i < 400; ++i) {
        total += c.update(true, static_cast<int16_t>(150 + (i % 2)));
    }
    CHECK(total == 0);
}

TEST_CASE("reset clears the drag without emitting") {
    DragColumn c;
    drag(c, 200, 180, 20);
    c.reset();
    CHECK_FALSE(c.tracking());
    CHECK(c.offsetPx() == 0);
    CHECK(c.update(true, 100) == 0); // fresh reference
}
