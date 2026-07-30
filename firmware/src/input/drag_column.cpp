#include "input/drag_column.hpp"

namespace h0 {

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
    if (gain4 > kGainMax) gain4 = kGainMax;
    residual_ = static_cast<int16_t>(residual_ + (dy * gain4) / kGainBase);

    // Truncation toward zero, with the remainder carried. Without carrying, a
    // slow drag loses a fraction of a unit on every sample and travels visibly
    // less than a fast drag over the same distance.
    const int steps = residual_ / kPixelsPerUnit;
    residual_ = static_cast<int16_t>(residual_ - steps * kPixelsPerUnit);

    // Screen y grows downward, so dragging up is negative dy -- and dragging up
    // must increase the value.
    return -steps;
}

} // namespace h0
