#pragma once

#include <cstdint>

#include "app/app.hpp"

namespace board {

/// The buzzer, as the acknowledgement channel.
///
/// None of this device's motion gestures is discoverable, and a gesture with no
/// feedback is indistinguishable from a gesture the device did not see. So every
/// command gets an answer, and the three outcomes have to be separable **by ear
/// alone, without looking**: committed, refused, and nothing-happened.
///
/// The vocabulary leans on pitch and rhythm rather than volume, which a piezo
/// through an NPN cannot vary meaningfully:
///
///   started   two rising blips      "off you go"
///   paused    one short low blip    "held"
///   resumed   one short high blip   "carry on"
///   reset     three fast rising     deliberately the most emphatic -- it is
///                                   the only destructive action
///   rejected  one low buzz          clearly not a confirmation
///   alarm     a repeating triplet   must carry across a room
///
/// Non-blocking: `update()` advances a small script so the UI keeps running
/// while a pattern plays. A blocking beep would stall the display mid-gesture,
/// which reads as the device hanging at exactly the moment it should feel
/// responsive.
class Buzzer {
public:
    void begin();

    /// Start playing the pattern for `f`. Interrupts anything already playing --
    /// the newest gesture is the one the user is waiting to hear about.
    void play(h0::Feedback f);

    /// Advance the script. Call every frame.
    void update(uint64_t now);

    /// Silence immediately and abandon the pattern.
    void stop();

    bool busy() const { return step_ < count_; }

    /// The alarm repeats until stopped, unlike every other pattern.
    bool alarmLooping() const { return looping_; }

private:
    struct Note {
        uint16_t hz;      ///< 0 = rest
        uint16_t ms;
    };

    void startStep(uint64_t now);
    void tone(uint16_t hz);

    static constexpr int kMaxNotes = 8;
    Note notes_[kMaxNotes] = {};
    int count_ = 0;
    int step_ = 0;
    uint64_t stepUntil_ = 0;
    bool looping_ = false;
    unsigned slice_ = 0;
};

} // namespace board
