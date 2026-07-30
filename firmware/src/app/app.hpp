#pragma once

#include <cstdint>

#include "input/orientation.hpp"
#include "timer/timer_model.hpp"

namespace h0 {

/// Which face to draw. An identifier rather than a face pointer, deliberately:
/// it keeps the application logic free of any dependency on the graphics
/// library, so this whole layer builds and tests on a host in milliseconds.
enum class FaceId : uint8_t {
    Hourglass,
    Digits,
    SplitFlap,
};

/// What the buzzer should say.
///
/// Every motion command gets an acknowledgement, because none of these gestures
/// is discoverable and the user has no other way to know the device saw them.
/// The three outcomes must be distinguishable by ear alone: **committed**,
/// **seen but refused**, and **nothing happened**.
enum class Feedback : uint8_t {
    None,
    Started,   ///< a run began
    Paused,
    Resumed,
    Reset,     ///< flipped: back to full and running. Deliberately distinct.
    Rejected,  ///< the gesture was understood and declined
    AlarmOn,
    AlarmOff,
};

/// Ties motion to the timer, and owns everything the faces do not.
///
/// Pure logic: no hardware, no graphics, no clock of its own. Time arrives as a
/// parameter exactly as it does in `TimerModel`, which is what lets the whole
/// interaction be tested against synthetic gesture sequences rather than by
/// waving a board around.
class App {
public:
    /// Longest duration the hourglass face is offered for.
    ///
    /// Beyond this the sand is not worth watching: the drain is imperceptible,
    /// and for a one-hour timer the screen is asleep for most of the run
    /// anyway. The other faces exist precisely so this one can have a range.
    static constexpr uint64_t kHourglassMaxUs = 10ull * 60ull * 1'000'000ull;

    /// How long a finished alarm keeps sounding before it gives up.
    ///
    /// A device that beeps forever in an empty room is worse than one that
    /// stops. The expiry itself is not forgotten -- the face still reads DONE --
    /// only the noise stops.
    static constexpr uint64_t kAlarmTimeoutUs = 60ull * 1'000'000ull;

    /// Apply a motion event. Returns what the buzzer should say.
    Feedback onMotion(MotionEvent e, uint64_t now);

    /// Drive time-based transitions: expiry raising the alarm, and the alarm
    /// eventually giving up. Cheap and safe to call every frame.
    Feedback tick(uint64_t now);

    /// Dial in a duration. Only honoured while the device is flat -- that is
    /// the setting posture, and a dial that worked while running would let a
    /// pocket rewrite the timer.
    bool setDuration(uint64_t us, uint64_t now);

    /// Step to the next face that suits the current timer.
    void cycleFace();

    FaceId face() const;
    bool alarmSounding() const { return alarmOn_; }

    /// True when the device is laid flat, which is when the picker is live.
    bool settingPosture() const { return flat_; }

    /// Keep the setting posture in sync with the *measured* orientation.
    ///
    /// Deriving it from events alone goes stale whenever an event is missed,
    /// and events are missed routinely: at power-on there is no transition to
    /// observe, and `flat -> edge -> upright` never produces the
    /// `FlatBack -> vertical` transition that `Raised` requires. Both leave the
    /// picker live in the hand with the timer face never drawn.
    void setFlat(bool flat) { flat_ = flat; }

    const TimerModel& timer() const { return timer_; }

private:
    bool faceSuits(FaceId f) const;

    TimerModel timer_;
    FaceId face_ = FaceId::Digits;
    bool flat_ = false;
    bool alarmOn_ = false;
    uint64_t alarmSince_ = 0;
};

} // namespace h0
