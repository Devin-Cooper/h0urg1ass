#pragma once

#include <cstdint>

namespace h0 {

/// Gravity direction in the device frame, normalised-ish.
///
/// Axes follow the panel: **+x right across the screen, +y down the screen,
/// +z out of the screen toward the viewer.** A value is the direction gravity
/// points, so holding the device upright and facing you gives (0, +1, 0) --
/// gravity runs down the screen.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// Which way the device is being held.
///
/// `UprightA` and `UprightB` are the two ways it can stand vertically -- normal
/// and turned over. Both are "running" postures; the *transition* between them
/// is the flip.
enum class Orientation : uint8_t {
    Unknown,   ///< not yet settled after power-on
    UprightA,  ///< vertical, normal way up
    UprightB,  ///< vertical, turned over
    FlatBack,  ///< lying on its back, screen up -- the paused, setting posture
    FaceDown,  ///< lying face down -- silence
    Edge,      ///< on its side; deliberately not a command
};

/// What just happened, if anything.
enum class MotionEvent : uint8_t {
    None,
    Flip,     ///< turned over between the two vertical postures: reset and run
    Settled,  ///< laid flat on its back: pause
    Raised,   ///< stood back up from flat: resume
    Silence,  ///< turned face down: silence a ringing alarm
};

/// Classifies how the device is being held, with the hysteresis that makes it
/// usable rather than merely correct.
///
/// Three things separate this from a naive threshold:
///
/// * **Dwell.** A candidate posture must persist before it is accepted. Walking
///   with the device in hand crosses every threshold repeatedly; without a dwell
///   the timer would pause and resume continuously.
/// * **Split thresholds.** Entering a posture needs a stronger reading than
///   leaving it, so a reading sitting exactly on a boundary cannot oscillate.
/// * **The flip requires a direct vertical-to-vertical change.** Turning the
///   device over is one continuous motion, so passing *through* other angles is
///   expected -- but if it *settles* flat in between, the user set it down and
///   picked it up again, which must not destroy a running timer. Settling into
///   any non-vertical posture disarms the flip.
///
/// That last rule is the one that matters. Reset is the only destructive action
/// in the product, and "picked it up the other way round" is a thing people do
/// without meaning anything by it.
class OrientationTracker {
public:
    // Thresholds are on the gravity components, so they are cosines of tilt.
    // Entering flat needs |z| >= 0.85 (about 32 degrees from horizontal);
    // leaving it only needs |z| < 0.70 (about 45 degrees). The gap is the
    // hysteresis band and nothing is reported inside it.
    static constexpr float kFlatEnter = 0.85f;
    static constexpr float kFlatExit = 0.70f;
    static constexpr float kVerticalEnter = 0.70f;

    /// How long a candidate posture must hold before it is accepted.
    static constexpr uint64_t kDwellUs = 350'000;

    /// Feed a filtered gravity reading. Returns the event this sample caused,
    /// which is `None` on the overwhelming majority of calls.
    MotionEvent update(const Vec3& g, uint64_t now);

    Orientation current() const { return current_; }

    /// The posture being considered but not yet accepted. Diagnostics only --
    /// acting on this would defeat the dwell.
    Orientation pending() const { return candidate_; }

    /// True when a flip would be honoured. False after the device has settled
    /// somewhere non-vertical, until it stands up again.
    bool flipArmed() const { return flipArmed_; }

private:
    Orientation classify(const Vec3& g) const;

    Orientation current_ = Orientation::Unknown;
    Orientation candidate_ = Orientation::Unknown;
    uint64_t candidateSince_ = 0;
    Orientation lastVertical_ = Orientation::Unknown;
    bool flipArmed_ = false;
};

/// First-order low-pass for the raw accelerometer.
///
/// Not for noise -- the part is quiet enough that tilt-angle noise is under a
/// tenth of a degree. This is for *hand tremor and footfall*, which move the
/// vector far more than the sensor's own noise does, and which would otherwise
/// push readings across a threshold.
///
/// `alpha` is the weight given to each new sample. The time constant is
/// approximately `dt / alpha`, so at 100 Hz and alpha 0.1 the constant is about
/// 100 ms -- responsive enough that a deliberate movement is not perceptibly
/// delayed, slow enough that a footstep is not a gesture.
class GravityFilter {
public:
    explicit GravityFilter(float alpha = 0.1f) : alpha_(alpha) {}

    const Vec3& push(const Vec3& sample) {
        if (!primed_) {
            // Seed with the first sample rather than ramping up from zero: a
            // ramp from the origin passes through every posture on the way and
            // would emit a spurious event at power-on.
            g_ = sample;
            primed_ = true;
            return g_;
        }
        g_.x += alpha_ * (sample.x - g_.x);
        g_.y += alpha_ * (sample.y - g_.y);
        g_.z += alpha_ * (sample.z - g_.z);
        return g_;
    }

    const Vec3& value() const { return g_; }
    bool primed() const { return primed_; }

private:
    Vec3 g_;
    float alpha_;
    bool primed_ = false;
};

} // namespace h0
