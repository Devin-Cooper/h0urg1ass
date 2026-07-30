#pragma once

#include "faces/face.hpp"

namespace h0 {

/// The dial, drawn.
///
/// Shown while the device is flat -- the setting posture. Without it the rotary
/// control is invisible: dragging changes the duration but nothing on screen
/// says a dial exists, where its zones are, or which way round it goes.
///
/// The ring is made of sixty marks rather than a smooth arc. Marks are
/// one-bit-native -- no anti-aliasing to fake, no thin curve to alias -- and
/// they double as a scale, so the eye can count minutes instead of estimating
/// an angle.
///
/// The two radii the dial actually uses are drawn as well, because the
/// coarse/fine split is otherwise something a user has to discover by accident.
class SettingFace : public IFace {
public:
    void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) override;
    const char* name() const override { return "setting"; }

    /// Draw at an explicit duration, for golden tests and for rendering without
    /// a live timer. `touchR` is the radius of the active touch in pixels, or a
    /// negative value when nothing is being touched; it highlights whichever
    /// zone the finger is in.
    static void renderAt(onebit::IFramebuffer& fb, uint32_t totalSeconds, int16_t touchR);
};

} // namespace h0
