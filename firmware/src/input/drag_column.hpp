#pragma once

#include <cstdint>

namespace h0 {

/// One column of a vertical spinner.
///
/// Replaces the rotary dial, which was clever and hard to use: a ring gives no
/// hint how far a step is, and a thumb on a 240x280 panel covers a third of it.
/// A column is the interaction people already own from every phone picker --
/// drag up, the number goes up, and the distance is legible because it is
/// linear.
///
/// Reports **unit deltas** plus a **sub-unit pixel offset**. The offset is what
/// makes it feel like a physical wheel: without it the digits only change on
/// step boundaries and the control feels dead between them, which reads as
/// dropped input rather than as precision.
class DragColumn {
public:
    /// Row pitch, and the drag distance for one unit at slow speed.
    ///
    /// Must be at least as tall as the largest glyph the renderer draws, or the
    /// rows overlap; and it sets how far a careful drag travels.
    static constexpr int16_t kPixelsPerUnit = 34;

    /// Velocity acceleration.
    ///
    /// At one unit per 34 px, setting twenty-five minutes would be four
    /// full-height drags. Every picker solves this the same way: a quick flick
    /// is worth more per pixel than a careful nudge, so the same control gives
    /// both coarse traversal and single-unit precision without a mode switch.
    ///
    /// Gain rises smoothly with per-sample speed rather than in steps.
    ///
    /// Banded gain (1x / 2x / 4x) is audible in the hand: a drag hovering near a
    /// threshold jumps between rates and reads as the control stuttering. The
    /// curve is `1 + mag/4`, capped, which is continuous and monotonic.
    ///
    /// Kept in quarters so it is integer arithmetic all the way down.
    static constexpr int16_t kGainBase = 4;   ///< = 1.0x, in quarters
    static constexpr int16_t kGainMax = 20;   ///< = 5.0x

    /// Cap the velocity gain, in quarters.
    ///
    /// Defaults to kGainMax. Set to kGainBase to collapse the curve to exactly
    /// 1x, with the sub-unit residual carry intact -- which is what a short
    /// ladder wants: a settings wheel with four entries under a 5x flick is a
    /// slot machine, while the 151-entry CAL wheel genuinely needs the gain.
    void setGainMax(int16_t quarters) { gainMax_ = quarters; }
    int16_t gainMax() const { return gainMax_; }

    /// Feed a touch sample. Returns the change in units since the last call --
    /// **positive when dragging up**, matching a physical wheel turning under
    /// the finger rather than a list being scrolled.
    int update(bool pressed, int16_t y);

    /// How far the wheel has been dragged within the current unit, in pixels.
    /// Negative means dragged up. Renderers offset the whole column by this so
    /// it tracks the finger continuously.
    int16_t offsetPx() const { return tracking_ ? residual_ : static_cast<int16_t>(0); }

    bool tracking() const { return tracking_; }

    /// Forget the drag. Call when the finger moves to another column, or the
    /// next sample is measured against a stale position.
    void reset();

private:
    bool tracking_ = false;
    int16_t lastY_ = 0;
    int16_t residual_ = 0; ///< pixels not yet converted to a unit
    int16_t gainMax_ = kGainMax;
};

} // namespace h0
