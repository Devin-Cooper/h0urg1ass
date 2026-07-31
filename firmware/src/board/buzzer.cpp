#include "board/buzzer.hpp"

#include <hardware/clocks.h>
#include <hardware/pwm.h>
#include <pico/stdlib.h>

#include "board/pins.hpp"

namespace board {

namespace {
constexpr uint16_t LOW = 1200;
constexpr uint16_t MID = 1800;
constexpr uint16_t HIGH = 2600;
} // namespace

void Buzzer::begin() {
    gpio_set_function(board::BUZZER, GPIO_FUNC_PWM);
    slice_ = pwm_gpio_to_slice_num(board::BUZZER);
    pwm_set_enabled(slice_, true);
    tone(0);
}

void Buzzer::tone(uint16_t hz) {
    if (hz == 0) {
        pwm_set_gpio_level(board::BUZZER, 0);
        return;
    }
    // Pick a wrap that lands near the requested frequency at a fixed divider.
    // The piezo is not a musical instrument; being a few percent out is
    // inaudible, so this avoids a divider search every note.
    constexpr float kDiv = 64.0f;
    const uint32_t top = static_cast<uint32_t>(
        static_cast<float>(clock_get_hz(clk_sys)) / (kDiv * static_cast<float>(hz)));
    pwm_set_clkdiv(slice_, kDiv);
    pwm_set_wrap(slice_, static_cast<uint16_t>(top > 65535 ? 65535 : top));
    pwm_set_gpio_level(board::BUZZER, static_cast<uint16_t>(top / 2)); // 50% duty
}

void Buzzer::play(h0::Feedback f) {
    count_ = 0;
    step_ = 0;
    looping_ = false;
    auto add = [&](uint16_t hz, uint16_t ms) {
        if (count_ < kMaxNotes) notes_[count_++] = Note{hz, ms};
    };

    switch (f) {
        case h0::Feedback::Started:
            add(MID, 45); add(0, 30); add(HIGH, 70);
            break;
        case h0::Feedback::Paused:
            add(LOW, 70);
            break;
        case h0::Feedback::Resumed:
            add(HIGH, 55);
            break;
        case h0::Feedback::Reset:
            // The only destructive action, so the most emphatic pattern. If a
            // flip is ever triggered by accident, this is what tells the user
            // in time to do something about it.
            add(MID, 40); add(0, 25); add(HIGH, 40); add(0, 25); add(HIGH, 90);
            break;
        case h0::Feedback::Rejected:
            // Low and flat. Must not be mistakable for any confirmation, which
            // is why nothing else in the set is a single long low note.
            add(LOW, 180);
            break;
        case h0::Feedback::AlarmOn:
            add(HIGH, 120); add(0, 90); add(HIGH, 120); add(0, 90);
            add(HIGH, 120); add(0, 500);
            looping_ = true;
            break;
        case h0::Feedback::AlarmOff:
            add(MID, 50);
            break;
        case h0::Feedback::SettingsOpen:
            // Two flat mid blips. Distinct from Resumed (one HIGH) and from
            // Started (MID then HIGH, rising) -- this one does not rise, which
            // is the audible difference: nothing has started, a door opened.
            add(MID, 40); add(0, 40); add(MID, 40);
            break;
        case h0::Feedback::SettingsSaved:
            // Descending. Every other confirmation in the set rises, so a fall
            // reads as "put away" rather than as "off you go".
            add(HIGH, 45); add(0, 25); add(MID, 70);
            break;
        case h0::Feedback::None:
            tone(0);
            return;
    }

    if (count_ > 0) startStep(time_us_64());
}

void Buzzer::startStep(uint64_t now) {
    tone(notes_[step_].hz);
    stepUntil_ = now + static_cast<uint64_t>(notes_[step_].ms) * 1000ull;
}

void Buzzer::update(uint64_t now) {
    if (step_ >= count_) return;
    if (now < stepUntil_) return;

    ++step_;
    if (step_ >= count_) {
        if (looping_) {
            step_ = 0;
            startStep(now);
            return;
        }
        tone(0);
        return;
    }
    startStep(now);
}

void Buzzer::stop() {
    tone(0);
    step_ = count_;
    looping_ = false;
}

} // namespace board
