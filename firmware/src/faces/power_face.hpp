#pragma once

#include <1bit/core/framebuffer.hpp>

#include "power/power_policy.hpp"

namespace h0 {

/// The power-off prompt: a full-screen face, never an overlay.
///
/// At one bit, text drawn over the falling sand is unreadable -- the flap board
/// is only legible because its housing is a WALL the sand cannot enter, and a
/// prompt has no such protection.
class PowerFace {
public:
    static void renderAt(onebit::IFramebuffer& fb, PowerAction action, uint8_t progress);
};

} // namespace h0
