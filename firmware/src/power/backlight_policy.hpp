#pragma once

#include <cstdint>

#include "settings/settings.hpp"

namespace h0 {

struct BacklightState {
    uint8_t level;
    bool render; ///< false suspends render, tracker.update AND the SPI push
};

/// power-and-time.md section 8.3: the entire power design is one decision --
/// how long does the screen stay on.
///
/// The dim step saves about 5 mA of 28. The BLANK step takes the backlight term
/// to zero and stops the render/SPI duty cycle, landing near 17 mA -- roughly
/// 0.6x ACTIVE. It is NOT "screen off": that needs SLPIN plus POWMAN P1.0,
/// which is issue #8, and none of section 8.3's screen-off figures apply here.
BacklightState backlightFor(const Settings& s, uint64_t idleUs, bool alarmSounding);

} // namespace h0
