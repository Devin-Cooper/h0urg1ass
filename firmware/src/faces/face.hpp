#pragma once

#include <1bit/core/framebuffer.hpp>

#include <cstdint>

#include "timer/timer_model.hpp"

namespace h0 {

/// A way of drawing the timer's state.
///
/// Faces are interchangeable renderers over one shared `TimerModel` -- they own
/// no state that matters and never mutate the timer. Switching face must never
/// be able to change what the timer is doing, which is why `render` takes the
/// model by const reference.
class IFace {
public:
    virtual ~IFace() = default;

    /// Draw the whole face into `fb`. Implementations own clearing it.
    /// `now` is passed rather than read so a face can be rendered off-line for
    /// a golden-image test at any chosen instant.
    virtual void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) = 0;

    virtual const char* name() const = 0;

    /// False when this face cannot represent the given timer -- the hourglass
    /// has no meaning without a duration to be a fraction of, for instance.
    virtual bool supports(const TimerModel& t) const {
        (void)t;
        return true;
    }
};

/// The rounded corners physically clip the panel at a ~44 px radius, measured
/// on hardware. Minimum diagonal clearance is r - r/sqrt(2) = 12.9 px, and a
/// 16 px inset was confirmed fully visible with margin. Nothing readable or
/// touchable goes outside this box.
namespace safe {
inline constexpr int16_t INSET = 16;
inline constexpr int16_t X = INSET;
inline constexpr int16_t Y = INSET;
inline constexpr int16_t W = 240 - 2 * INSET; // 208
inline constexpr int16_t H = 280 - 2 * INSET; // 248
} // namespace safe

} // namespace h0
