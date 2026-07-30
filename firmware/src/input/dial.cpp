#include "input/dial.hpp"

#include <cmath>

namespace h0 {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Wrap a raw angle difference into (-pi, pi].
///
/// Without this, crossing the atan2 branch cut at +-pi reads as a near-full
/// revolution in the wrong direction -- dragging smoothly past the top of the
/// dial would subtract an hour.
float wrap(float d) {
    while (d > kPi) d -= 2.0f * kPi;
    while (d <= -kPi) d += 2.0f * kPi;
    return d;
}

} // namespace

void Dial::reset() {
    tracking_ = false;
    residual_ = 0.0f;
}

int Dial::update(bool pressed, int16_t x, int16_t y) {
    if (!pressed) {
        reset();
        return 0;
    }

    const float dx = static_cast<float>(x - cfg_.cx);
    const float dy = static_cast<float>(y - cfg_.cy);
    const float r2 = dx * dx + dy * dy;
    const float dead = static_cast<float>(cfg_.deadZone);

    if (r2 < dead * dead) {
        // Inside the dead zone the angle is meaningless, so stop tracking
        // rather than integrate noise. Dragging back out starts a fresh drag,
        // which is what a user expects after passing through the middle.
        reset();
        return 0;
    }

    const float coarseR = static_cast<float>(cfg_.coarseRadius);
    coarse_ = r2 >= coarseR * coarseR;

    // atan2(dy, dx) with y down the screen makes increasing angle clockwise on
    // the glass, which is the direction a right hand naturally turns a knob.
    const float angle = std::atan2(dy, dx);

    if (!tracking_) {
        // First sample of a drag establishes the reference and moves nothing.
        // Emitting a step here would make every touch jump.
        tracking_ = true;
        lastAngle_ = angle;
        residual_ = 0.0f;
        return 0;
    }

    const float delta = wrap(angle - lastAngle_);
    lastAngle_ = angle;

    const float perStep = 2.0f * kPi / static_cast<float>(cfg_.stepsPerTurn);
    residual_ += delta / perStep;

    // Truncate toward zero and keep the remainder, so a slow drag accumulates
    // instead of being rounded away sample by sample.
    const int steps = static_cast<int>(residual_);
    residual_ -= static_cast<float>(steps);
    return steps;
}

} // namespace h0
