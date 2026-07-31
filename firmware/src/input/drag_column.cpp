#include "input/drag_column.hpp"

namespace h0 {

namespace {

/// Floor division.
///
/// C++ integer division truncates toward zero, which is symmetric about the
/// origin -- and that symmetry is exactly what makes a round-to-nearest detent
/// oscillate: it leaves the residual sitting on the OPPOSITE commit boundary,
/// which is itself a commit point, so a perfectly still finger flips the value
/// every frame. Flooring lands the residual inside the band instead.
int floorDiv(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

} // namespace

void DragColumn::reset() {
    tracking_ = false;
    residual_ = 0;
}

int DragColumn::update(bool pressed, int16_t y) {
    if (!pressed) {
        reset();
        return 0;
    }

    if (!tracking_) {
        // The first sample of a drag establishes the reference and moves
        // nothing. Emitting here would make every tap jump the value.
        tracking_ = true;
        lastY_ = y;
        residual_ = 0;
        return 0;
    }

    const int16_t dy = static_cast<int16_t>(y - lastY_);
    lastY_ = y;

    // Scale by speed. Applied to the movement rather than to the step count so
    // the wheel still tracks the finger continuously -- gain changes how much
    // value a pixel is worth, not whether the animation follows.
    //
    // Continuous rather than banded: with thresholds, a drag sitting near one
    // flips between rates and reads as the control stuttering.
    const int16_t mag = static_cast<int16_t>(dy < 0 ? -dy : dy);
    int16_t gain4 = static_cast<int16_t>(kGainBase + mag);
    if (gain4 > gainMax_) gain4 = gainMax_;
    residual_ = static_cast<int16_t>(residual_ + (dy * gain4) / kGainBase);

    // Commit where the RENDERER says the wheel has moved, not half a pitch
    // later. The faces draw a row as selected once it is within PITCH/2 of the
    // window centre; committing at a full pitch meant 42.5% of frames showed a
    // row that had not been committed, and releasing there snapped back by one.
    //
    // The offset is PITCH/2 - 1, not PITCH/2, because the renderer's window is
    // half-open -- (CY-17, CY+17] -- so at offset -17 the centre row already
    // loses and its neighbour is drawn large. The rule adopts that asymmetry
    // rather than fighting it, which is what lets all three faces stay
    // untouched. The resulting band is [-16, +17]: exactly one pitch wide, and
    // no reachable residual is itself a commit point.
    const int steps = floorDiv(residual_ + kPixelsPerUnit / 2 - 1, kPixelsPerUnit);
    residual_ = static_cast<int16_t>(residual_ - steps * kPixelsPerUnit);

    // Screen y grows downward, so dragging up is negative dy -- and dragging up
    // must increase the value.
    return -steps;
}

} // namespace h0
