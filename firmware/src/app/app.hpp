#pragma once

#include <cstdint>

#include "input/orientation.hpp"
#include "timer/timer_model.hpp"

namespace h0 {

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
    SettingsOpen,  ///< the settings screen appeared
    SettingsSaved, ///< settings were written to flash
    Booted,        ///< powered on -- the first thing the device says
};

/// Ties motion to the timer, and owns everything the faces do not.
///
/// Pure logic: no hardware, no graphics, no clock of its own. Time arrives as a
/// parameter exactly as it does in `TimerModel`, which is what lets the whole
/// interaction be tested against synthetic gesture sequences rather than by
/// waving a board around.
class App {
public:
    /// How long a finished alarm keeps sounding before it gives up.
    ///
    /// A device that beeps forever in an empty room is worse than one that
    /// stops. The expiry itself is not forgotten -- the face still reads DONE --
    /// only the noise stops.
    static constexpr uint64_t kAlarmTimeoutUs = 60ull * 1'000'000ull;

    /// How long a finished alarm keeps sounding. Runtime, so the settings menu
    /// can reach it; kAlarmTimeoutUs remains the compiled-in default.
    void setAlarmTimeout(uint64_t us) { alarmTimeoutUs_ = us; }

    /// Apply a motion event. Returns what the buzzer should say.
    Feedback onMotion(MotionEvent e, uint64_t now);

    /// Drive time-based transitions: expiry raising the alarm, and the alarm
    /// eventually giving up. Cheap and safe to call every frame.
    Feedback tick(uint64_t now);

    /// Dial in a duration. Only honoured in the setting posture --
    /// settingPosture(), flat OR upright-and-Idle, not merely flat -- because
    /// a dial that worked while running would let a pocket rewrite the timer.
    bool setDuration(uint64_t us, uint64_t now);

    bool alarmSounding() const { return alarmOn_; }

    /// True when the picker is live: the device is flat, OR the timer is idle.
    ///
    /// Two clauses, and both are load-bearing. The `flat_` clause is the
    /// original behaviour, kept whole. The Idle clause is what lets a device
    /// powered on upright be set in the hand without laying it down.
    ///
    /// Deliberately Idle rather than !isRunning(): a PAUSED timer keeps the
    /// hourglass, because at 90 degrees what the user needs to see is how much
    /// is left rather than a pair of wheels, and an EXPIRED one must keep
    /// reading DONE.
    bool settingPosture() const {
        return flat_ || timer_.state() == TimerModel::State::Idle;
    }

    /// The measured flat posture on its own, WITHOUT settingPosture()'s Idle
    /// clause. Only for callers that genuinely mean "lying on a table" rather
    /// than "the picker is live" -- the deferred flash erase, which needs the
    /// device physically at rest.
    bool isFlat() const { return flat_; }

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
    TimerModel timer_;
    bool flat_ = false;
    bool alarmOn_ = false;
    uint64_t alarmSince_ = 0;
    uint64_t alarmTimeoutUs_ = kAlarmTimeoutUs;
};

} // namespace h0
