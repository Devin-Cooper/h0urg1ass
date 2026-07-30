#pragma once

#include <cstdint>

namespace h0 {

/// A countdown timer, as pure logic. No hardware, no globals, no clock of its
/// own -- every call that needs the time is handed it. That makes the whole
/// model host-testable, which matters because this is the one component whose
/// correctness a user can actually check against a wall clock.
///
/// **The central rule: elapsed time is anchored, never accumulated.** A model
/// that did `elapsed += dt` every frame would drift with every dropped frame,
/// every sleep, and every long display push -- invisibly, and worse the longer
/// the timer runs. Instead a running timer stores the clock reading it started
/// from and derives elapsed by subtraction, so a frame that takes 200 ms costs
/// nothing in accuracy and the model is exact regardless of how it is polled.
///
/// Times are microseconds from a monotonic source (`time_us_64()` on the
/// RP2350). 64 bits of microseconds is ~584,000 years, so there is no wrap to
/// defend against -- but every subtraction below is still guarded, because
/// these are *unsigned* and an underflow would produce a colossal remaining
/// time rather than zero.
class TimerModel {
public:
    enum class State : uint8_t {
        Idle,    ///< a duration is set; not counting
        Running, ///< counting down
        Paused,  ///< counting stopped, elapsed retained
        Expired, ///< reached zero; awaiting acknowledgement
    };

    // ------------------------------------------------------------- config --

    /// Set the countdown length. Legal in any state; the timer returns to Idle
    /// and elapsed is discarded, because changing the duration of a run in
    /// progress has no meaning a user would predict.
    void setDuration(uint64_t us) {
        duration_ = us;
        elapsedBase_ = 0;
        anchor_ = 0;
        state_ = State::Idle;
    }

    uint64_t duration() const { return duration_; }
    State state() const { return state_; }

    bool isRunning() const { return state_ == State::Running; }
    bool isExpired() const { return state_ == State::Expired; }

    // ---------------------------------------------------------- transitions --

    /// Begin counting from full. Idempotent while already running.
    /// A zero duration expires immediately rather than running forever.
    void start(uint64_t now) {
        if (state_ == State::Running) return;
        elapsedBase_ = 0;
        anchor_ = now;
        state_ = (duration_ == 0) ? State::Expired : State::Running;
    }

    /// Stop counting, keeping elapsed. No-op unless running.
    void pause(uint64_t now) {
        if (state_ != State::Running) return;
        elapsedBase_ = elapsed(now);
        anchor_ = 0;
        state_ = State::Paused;
    }

    /// Continue from where pause left off. No-op unless paused.
    ///
    /// Re-anchoring to `now` is what makes a pause of any length free: the
    /// time spent paused never enters the elapsed calculation at all.
    void resume(uint64_t now) {
        if (state_ != State::Paused) return;
        anchor_ = now;
        state_ = (remaining(now) == 0) ? State::Expired : State::Running;
    }

    /// Back to full and counting -- the physical "turn the hourglass over".
    /// Legal from every state, including Expired, since flipping a spent
    /// hourglass is exactly how you start the next one.
    void reset(uint64_t now) {
        elapsedBase_ = 0;
        anchor_ = now;
        state_ = (duration_ == 0) ? State::Expired : State::Running;
    }

    /// Back to full and NOT counting.
    void stop() {
        elapsedBase_ = 0;
        anchor_ = 0;
        state_ = State::Idle;
    }

    /// Acknowledge an expiry (alarm silenced). Returns to Idle at full.
    void acknowledge() {
        if (state_ != State::Expired) return;
        stop();
    }

    /// Drive expiry detection. Safe and cheap to call every frame; the model
    /// is correct whether or not it is called, because remaining() is derived
    /// rather than stored. This only moves Running -> Expired so callers can
    /// notice the edge.
    void tick(uint64_t now) {
        if (state_ == State::Running && remaining(now) == 0) {
            state_ = State::Expired;
        }
    }

    // ------------------------------------------------------------ queries --

    /// Microseconds counted so far, excluding any paused intervals.
    uint64_t elapsed(uint64_t now) const {
        if (state_ != State::Running) return elapsedBase_;
        // Guard against a non-monotonic or stale `now` rather than trusting
        // the caller; unsigned underflow here would read as time travel.
        const uint64_t delta = (now > anchor_) ? (now - anchor_) : 0;
        return elapsedBase_ + delta;
    }

    /// Microseconds left, saturating at zero. Never underflows.
    uint64_t remaining(uint64_t now) const {
        if (state_ == State::Expired) return 0;
        const uint64_t e = elapsed(now);
        return (e >= duration_) ? 0 : (duration_ - e);
    }

    /// Whole seconds left, rounded UP.
    ///
    /// Ceiling, not truncation: a timer displaying "1" should show it for the
    /// whole of the last second and reach "0" exactly at expiry. Truncating
    /// shows "0" for a full second while the timer is still running, which
    /// reads as a stuck clock.
    uint32_t remainingSeconds(uint64_t now) const {
        const uint64_t us = remaining(now);
        return static_cast<uint32_t>((us + 999'999u) / 1'000'000u);
    }

    /// Fraction still to run: 1.0 at full, 0.0 at expiry. Drives the hourglass
    /// fill level. A zero duration reads as 0.0 (empty) rather than dividing.
    float fraction(uint64_t now) const {
        if (duration_ == 0) return 0.0f;
        return static_cast<float>(remaining(now)) / static_cast<float>(duration_);
    }

private:
    uint64_t duration_ = 0;
    uint64_t elapsedBase_ = 0; ///< elapsed banked by previous run segments
    uint64_t anchor_ = 0;      ///< clock reading the current segment began at
    State state_ = State::Idle;
};

} // namespace h0
