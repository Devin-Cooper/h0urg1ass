#pragma once

#include <1bit/core/framebuffer.hpp>

#include <cstdint>

#include "faces/layout.hpp"

namespace h0 {

/// The duration picker: two vertical spinner columns, minutes and seconds.
///
/// Shown while the device is flat -- the setting posture. The timer is not
/// counting then, so the whole panel is available.
///
/// Replaces an earlier rotary dial, which was clever and hard to use. A ring
/// gives no cue how far a step is, and a thumb covers a third of a 240x280
/// panel. Columns are the interaction people already own from every phone
/// picker, and drag distance maps linearly to value, so the control is legible
/// before it is touched.
struct PickerState {
    uint32_t minutes = 0; ///< 0-99; the readout has five cells, so 99:59 is the ceiling
    uint32_t seconds = 0; ///< 0-59

    /// Pixel offset of each column within its current unit, for rendering a
    /// drag in progress. Negative is dragged up.
    int16_t minutesOffset = 0;
    int16_t secondsOffset = 0;

    /// Which column the finger is on: 0 none, 1 minutes, 2 seconds.
    uint8_t activeColumn = 0;
};

class SettingFace {
public:

    /// Draw an explicit picker state. Used by the golden tests and by the app,
    /// which owns the drag offsets.
    static void renderAt(onebit::IFramebuffer& fb, const PickerState& s);
};

} // namespace h0
