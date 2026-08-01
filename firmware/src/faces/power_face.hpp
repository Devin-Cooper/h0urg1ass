#pragma once

#include <1bit/core/framebuffer.hpp>

#include "power/power_policy.hpp"

namespace h0 {

/// The power-off prompt: a full-screen face, never an overlay.
///
/// At one bit, text drawn over the falling sand is unreadable. The flap board
/// gets away with it only because it is an OPAQUE PANEL composited after the
/// sand, with every cell stamped at whichever polarity keeps it in contrast
/// with what is behind it. A prompt drawn as an overlay has neither, so it
/// takes the whole screen instead.
class PowerFace {
public:
    static void renderAt(onebit::IFramebuffer& fb, PowerAction action, uint8_t progress);
};

} // namespace h0
