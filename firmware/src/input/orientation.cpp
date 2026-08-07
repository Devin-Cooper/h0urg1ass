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

    // On its side: the mirror of the vertical test above. Split out of Edge
    // because Edge is the fallthrough -- it covers a device merely tilted back
    // in the hand, which must not pause anything. For a pure in-plane rotation
    // |x|^2 + |y|^2 = 1, so the device is either upright or on its side and
    // never Edge at all; Edge is what you get tilted OUT of plane.
    if (absf(g.x) >= kSideEnter && absf(g.x) > absf(g.y)) {
        return Orientation::OnSide;
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
    // OnSide gets a longer dwell than everything else -- see kSideDwellUs.
    const uint64_t dwell = (observed == Orientation::OnSide) ? kSideDwellUs : kDwellUs;
    if (now - candidateSince_ < dwell) return MotionEvent::None;

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
            // Stood up off a surface. Distinct from Righted because only this
            // one may START an idle timer: lying flat needs |g.z| >= 0.85 and is
            // an unambiguous, deliberate resting posture, whereas Edge commits
            // after 350 ms on any tilt past ~45 degrees -- straightening up
            // after a wobble must not launch a timer.
            if (previous == Orientation::FlatBack) return MotionEvent::Raised;
            // Back upright from its side, or from a tilt on the way. Resume
            // only. Edge is included so that flat -> edge -> upright still
            // reaches App -- that path produced nothing at all before, which
            // left the picker live in the hand.
            if (previous == Orientation::OnSide || previous == Orientation::Edge)
                return MotionEvent::Righted;
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

        case Orientation::OnSide:
            // Deliberately does NOT clear flipArmed_: the user chose that 180
            // degrees always means restart, whether or not it paused en route.
            //
            // Suppressed at power-on for the same reason Raised is -- a pause is
            // a command, and the device must not report commands it was not
            // given.
            return (previous == Orientation::Unknown) ? MotionEvent::None
                                                      : MotionEvent::Tipped;

        case Orientation::Edge:
            // Edge is the out-of-plane tilt, not "on its side" -- OnSide
            // covers that now, and OnSide *is* a command (Tipped). Edge is
            // deliberately not one: it is a device merely tilted in the hand,
            // or passing between postures on its way somewhere else, and
            // rotating through it is exactly how a flip happens -- so the
            // flip stays armed.
            return MotionEvent::None;

        case Orientation::Unknown:
            return MotionEvent::None;
    }
    return MotionEvent::None;
}

} // namespace h0
