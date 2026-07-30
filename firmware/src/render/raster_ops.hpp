#pragma once

#include <1bit/core/framebuffer.hpp>

namespace h0 {

/// Invert the safe box, leaving the clipped margin alone.
///
/// Scoped deliberately. A whole-panel invert puts about 1,800 ink pixels under
/// the rounded-corner clip -- invisible on the glass, but it turns the
/// corner-clearance check from a real guard into noise.
void invertSafeBox(onebit::IFramebuffer& fb);

/// Turn the finished frame upside down, in place.
///
/// This is how the flipped upright posture is handled. Rather than teach the
/// simulation, the picker, the touch mapping and the font that the device can
/// be turned over, the whole composed frame is rotated once at the end, and
/// everything upstream keeps working in a content space where "up" is up.
void rotate180(onebit::IFramebuffer& fb);

} // namespace h0
