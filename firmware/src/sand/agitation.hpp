#pragma once

#include <cmath>

#include "input/orientation.hpp"

namespace h0 {

/// How much the device is being *handled*, from 0 (sitting still) to 1.
///
/// The sand should answer to a small tilt in the hand and be completely dead on
/// a desk. Tilt angle alone cannot tell those apart -- a device resting at a
/// slight angle reads the same as one being tipped -- so this measures JERK,
/// how fast the acceleration vector is changing, and gates the response on it.
///
/// **The vector is low-passed, not its magnitude, and that is the whole trick.**
/// A 15 deg/s deliberate tilt and an 8 mg desk buzz produce almost the same raw
/// jerk magnitude (measured: 0.0175 against ~0.016 g per sample). What separates
/// them is direction: a tilt's jerk points one way for many samples, while a
/// vibration's alternates and cancels when the vector is averaged. Filtering
/// |jerk| instead would keep both. A shake is oscillatory too, but it is fifty
/// times larger, so it survives the cancellation on amplitude alone.
class Agitation {
public:
    /// Feed one accelerometer sample, at the simulation's tick rate.
    void update(const Vec3& a) {
        const Vec3 d{a.x - prev_.x, a.y - prev_.y, a.z - prev_.z};
        prev_ = a;
        if (!primed_) { primed_ = true; return; } // the first delta is meaningless

        // Low-pass the jerk VECTOR. ~0.08 s time constant.
        c_.x += 0.35f * (d.x - c_.x);
        c_.y += 0.35f * (d.y - c_.y);
        c_.z += 0.35f * (d.z - c_.z);

        const float m = std::sqrt(c_.x * c_.x + c_.y * c_.y + c_.z * c_.z);
        float e = (m - kFloor) / (kCeil - kFloor);
        if (e < 0.0f) e = 0.0f;
        if (e > 1.0f) e = 1.0f;

        // Instant attack, exponential release: the sand should wake the moment
        // it is picked up and take a beat to settle, not the reverse.
        const float released = value_ * kRelease;
        value_ = (e > released) ? e : released;

        // A HARD zero, not an epsilon. An asymptote that never quite reaches
        // zero leaves a drift probability that never quite reaches zero, and the
        // sand shimmers on a desk forever -- which is the exact complaint this
        // whole mechanism exists to avoid.
        if (value_ < kCutoff) value_ = 0.0f;
    }

    /// 0 when still, 1 when being handled.
    float value() const { return value_; }

    void reset() {
        value_ = 0.0f;
        c_ = Vec3{0.0f, 0.0f, 0.0f};
        primed_ = false;
    }

private:
    /// Derived, not tuned: 0.05^(1/30) makes a full-scale reading decay to the
    /// cutoff in exactly 30 ticks, which is one second at the simulation rate.
    static constexpr float kRelease = 0.9050f;
    static constexpr float kCutoff = 0.05f;

    /// Below the floor is bench noise and desk buzz; the ceiling is a brisk
    /// handling movement. Both in g per sample.
    static constexpr float kFloor = 0.0045f;
    static constexpr float kCeil = 0.030f;


    Vec3 prev_{0.0f, 0.0f, 0.0f};
    Vec3 c_{0.0f, 0.0f, 0.0f};
    float value_ = 0.0f;
    bool primed_ = false;
};

} // namespace h0
