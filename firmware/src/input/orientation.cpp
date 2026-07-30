#include "input/orientation.hpp"

namespace h0 {

namespace {
float absf(float v) { return v < 0.0f ? -v : v; }
} // namespace

Orientation OrientationTracker::classify(const Vec3& g) const {
    const float az = absf(g.z);

    // Flat first. When the device lies on a surface the in-plane rotation is
    // undefined -- x and y are both near zero and their ratio is noise -- so
    // asking "which way up" there would produce a coin flip between UprightA
    // and UprightB, and a coin flip drives the one destructive gesture.
    const bool wasFlat = (current_ == Orientation::FlatBack || current_ == Orientation::FaceDown);
    const float flatThreshold = wasFlat ? kFlatExit : kFlatEnter;
    if (az >= flatThreshold) {
        return (g.z < 0.0f) ? Orientation::FlatBack : Orientation::FaceDown;
    }

    // Vertical: gravity runs along the screen's long axis. Requiring |y| to beat
    // |x| as well as the threshold keeps a device resting on its side out of the
    // upright classes.
    if (absf(g.y) >= kVerticalEnter && absf(g.y) > absf(g.x)) {
        return (g.y > 0.0f) ? Orientation::UprightA : Orientation::UprightB;
    }

    return Orientation::Edge;
}

MotionEvent OrientationTracker::update(const Vec3& g, uint64_t now) {
    const Orientation observed = classify(g);

    // Restart the dwell whenever the observation changes.
    if (observed != candidate_) {
        candidate_ = observed;
        candidateSince_ = now;
        return MotionEvent::None;
    }
    if (observed == current_) return MotionEvent::None;
    if (now - candidateSince_ < kDwellUs) return MotionEvent::None;

    // The candidate has held long enough. Commit it.
    const Orientation previous = current_;
    current_ = observed;

    switch (observed) {
        case Orientation::UprightA:
        case Orientation::UprightB: {
            const bool isFlip = flipArmed_ && lastVertical_ != Orientation::Unknown &&
                                lastVertical_ != observed;
            lastVertical_ = observed;
            flipArmed_ = true;
            if (isFlip) return MotionEvent::Flip;
            // Standing up from flat is a resume; standing up from Unknown at
            // power-on is not an event at all, or the device would appear to
            // start itself.
            if (previous == Orientation::FlatBack) return MotionEvent::Raised;
            return MotionEvent::None;
        }

        case Orientation::FlatBack:
            // Settling breaks the flip chain: the user put it down, and picking
            // it up rotated must not be read as turning it over.
            flipArmed_ = false;
            return MotionEvent::Settled;

        case Orientation::FaceDown:
            flipArmed_ = false;
            return MotionEvent::Silence;

        case Orientation::Edge:
            // On its side is deliberately not a command, but it is also not a
            // settled posture -- rotating the device through its side is exactly
            // how you turn it over, so the flip stays armed.
            return MotionEvent::None;

        case Orientation::Unknown:
            return MotionEvent::None;
    }
    return MotionEvent::None;
}

} // namespace h0
