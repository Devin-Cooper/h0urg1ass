#pragma once

#include <cstdint>

#include "power/battery_model.hpp"
#include "settings/settings.hpp"

namespace h0 {

/// What the loop should do about the power button this frame.
enum class PowerAction : uint8_t {
    None,
    Wake,                 ///< a press while blanked
    PromptHold,           ///< the filling bar; see PowerDecision::progress
    PromptRelease,        ///< threshold reached; let go and the board dies
    PromptTimerRunning,   ///< threshold reached with a timer running: keep holding
    PowerOff,             ///< run the shutdown sequence
    UsbCannotPowerOff,    ///< the latch is bypassed by D4; say so
};

struct PowerDecision {
    PowerAction action = PowerAction::None;
    uint8_t progress = 0; ///< 0..255 across the current hold stage
};

struct PowerInput {
    uint64_t now = 0;
    bool buttonDown = false;
    uint64_t idleUs = 0;     ///< since the last touch, motion or button press
    bool timerRunning = false;
    bool alarmSounding = false; ///< app.alarmSounding() -- see backlightFor's precedent
    bool blanked = false;    ///< the backlight ladder's third step
    bool onUsb = false;
    BatteryReading battery;
    /// `Settings::batFloorRawMv` AS IT WAS AT BOOT, in RAW mV. 0 = no floor has
    /// survived a power cycle yet, which leaves the low-battery route disabled.
    ///
    /// Deliberately NOT read from the live `Settings` handed to update(): see
    /// the comment at its only use, which is the whole reason this field exists.
    uint16_t armedFloorRawMv = 0;
};

/// Every power-off threshold in one testable place.
///
/// Mirrors backlightFor(), which is the existing precedent for a pure policy
/// driven once per frame from main(). Nothing here touches hardware, so the
/// two-stage hold and the calibration gate can be proved without a board --
/// which matters, because the only way to test them on hardware is to switch
/// the device off.
class PowerPolicy {
public:
    /// Hold to arm. Two seconds is long enough that a pocket cannot do it.
    static constexpr uint64_t kHoldUs = 2'000'000ull;
    /// Total hold needed to override a running timer.
    static constexpr uint64_t kConfirmUs = 4'000'000ull;

    PowerDecision update(const PowerInput& in, const Settings& s);

private:
    bool wasDown_ = false;
    uint64_t downSince_ = 0;
    bool armed_ = false; ///< the hold passed its threshold; the press is committed
    /// True once the button has been observed up at least once. A press that
    /// is already down the first time update() ever runs may have started
    /// before boot -- PowerButton::begin() lands seconds after main()'s idle
    /// window, LCD init and the sand probe, with the panel dark the whole
    /// time -- so racing it against the ordinary hold thresholds would treat
    /// "already holding when the device switched on" as "holding to power
    /// off". Such a press is ignored entirely until it is released.
    bool seenRelease_ = false;
};

} // namespace h0
