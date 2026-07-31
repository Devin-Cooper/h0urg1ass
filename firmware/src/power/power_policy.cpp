#include "power/power_policy.hpp"

namespace h0 {

namespace {

uint8_t progressByte(uint64_t held, uint64_t target) {
    if (held >= target) return 255;
    return static_cast<uint8_t>((held * 255ull) / target);
}

} // namespace

PowerDecision PowerPolicy::update(const PowerInput& in, const Settings& s) {
    const bool wasDown = wasDown_;
    wasDown_ = in.buttonDown;

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
    // A running timer blocks both. Section 9: never enter OFF with a timer
    // running -- the alarm goes with the RTC's supply and never fires.
    if (!in.timerRunning) {
        if (in.battery.valid && in.battery.calibrated &&
            in.battery.milliVolts < kCutoffMv) {
            return {in.onUsb ? PowerAction::UsbCannotPowerOff
                             : PowerAction::PowerOff, 0};
        }
        if (s.offAfterS != 0 &&
            in.idleUs >= static_cast<uint64_t>(s.offAfterS) * 1'000'000ull) {
            return {in.onUsb ? PowerAction::UsbCannotPowerOff
                             : PowerAction::PowerOff, 0};
        }
    }

    return {PowerAction::None, 0};
}

} // namespace h0
