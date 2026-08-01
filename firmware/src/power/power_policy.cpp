#include "power/power_policy.hpp"

#include "power/battery_floor.hpp"

namespace h0 {

namespace {

uint8_t progressByte(uint64_t held, uint64_t target) {
    if (held >= target) return 255;
    return static_cast<uint8_t>((held * 255ull) / target);
}

} // namespace

PowerDecision PowerPolicy::update(const PowerInput& in, const Settings& s) {
    if (!in.buttonDown) seenRelease_ = true;

    const bool wasDown = wasDown_;
    wasDown_ = in.buttonDown;

    // A press already down the first time this ever sees the button run must
    // not be able to arm a power-off -- see seenRelease_'s declaration.
    // Ignored entirely: no Wake, no PromptHold, no PromptRelease, no PowerOff.
    if (in.buttonDown && !seenRelease_) {
        return {PowerAction::None, 0};
    }

    // --- the button ------------------------------------------------------
    if (in.buttonDown) {
        if (!wasDown) {
            downSince_ = in.now;
            armed_ = false;
            // A press while blanked is a wake and nothing else. Requiring the
            // release first would make waking feel like a delay.
            if (in.blanked) return {PowerAction::Wake, 0};
            // The first frame of a press (held == 0) shows nothing yet --
            // otherwise every ordinary tap would flash the hold prompt for
            // one frame before the release is even seen.
            return {PowerAction::None, 0};
        }

        const uint64_t held = in.now - downSince_;
        const uint64_t target = in.timerRunning ? kConfirmUs : kHoldUs;

        if (held >= target) {
            armed_ = true;
            // D4 keeps VSYS alive on USB regardless of GPIO15, so the hold
            // can never actually arm a power-off there -- say so for every
            // frame the hold spends past the threshold, not only the single
            // release sample, or the panel spends the whole hold claiming
            // "release to power off" and only admits the truth for ~25 ms.
            if (in.onUsb) return {PowerAction::UsbCannotPowerOff, 255};
            return {PowerAction::PromptRelease, 255};
        }
        if (in.timerRunning && held >= kHoldUs) {
            return {PowerAction::PromptTimerRunning, progressByte(held, target)};
        }
        return {PowerAction::PromptHold, progressByte(held, target)};
    }

    // --- the release -----------------------------------------------------
    if (wasDown) {
        const bool commit = armed_;
        armed_ = false;
        // Releasing before the threshold is the cancel: GPIO15 was never
        // dropped, so there is nothing to undo.
        if (commit) return {in.onUsb ? PowerAction::UsbCannotPowerOff
                                     : PowerAction::PowerOff, 0};
        return {PowerAction::None, 0};
    }

    // --- the automatic routes --------------------------------------------
    // A running timer blocks both, and so does a sounding alarm: the alarm
    // that timer's expiry just raised needs `isRunning()` to have gone false
    // to start ringing at all, so gating on timerRunning alone would let the
    // idle route fire in the very frame the alarm starts, if idleUs had
    // already crossed offAfterS while the timer was counting down (idleUs
    // accumulates for the whole run, since nothing else about a quietly
    // counting timer resets it). Section 9: never enter OFF with a timer
    // running or an alarm sounding -- both go with the RTC's supply and never
    // fire/finish. Silent on USB: D4 keeps VSYS alive regardless of GPIO15,
    // so these routes can never actually act there, and re-announcing that
    // every offAfterS would relight the screen forever for no reason.
    if (!in.timerRunning && !in.alarmSounding) {
        // The cutoff is DERIVED, not fixed, and zero means "no floor learned
        // yet, so there is nothing to act on". That gate used to be
        // `battery.calibrated`, which in practice was never true -- the gain
        // stayed at its 1000 default because calibrating it needed a meter --
        // so this route has never once fired on a real device.
        const uint16_t cutoff =
            BatteryFloor::cutoffMv(s.batFloorRawMv, s.batCalPermille);
        // Belt and braces: cutoffMv already returns 0 when unlearned, and
        // milliVolts < 0 would be false anyway after unsigned promotion, so
        // this check is redundant. It stays so the disarm is visible where
        // the decision is made, not inferred from integer promotion.
        if (in.battery.valid && cutoff != 0 && in.battery.milliVolts < cutoff) {
            if (in.onUsb) return {PowerAction::None, 0};
            return {PowerAction::PowerOff, 0};
        }
        if (s.offAfterS != 0 &&
            in.idleUs >= static_cast<uint64_t>(s.offAfterS) * 1'000'000ull) {
            if (in.onUsb) return {PowerAction::None, 0};
            return {PowerAction::PowerOff, 0};
        }
    }

    return {PowerAction::None, 0};
}

} // namespace h0
