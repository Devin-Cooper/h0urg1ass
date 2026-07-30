#pragma once

#include <cstdint>

namespace h0 {

/// Turns a dragged finger into a rotary dial.
///
/// The panel is 240x280 with a ~44 px corner radius, which leaves no room for a
/// keypad and barely any for buttons. A dial has no target smaller than a thumb:
/// the whole rim is the control, and precision comes from how far you drag
/// rather than from where you land.
///
/// Reports **steps**, not time. What a step is worth is the caller's business,
/// which keeps this pure geometry and lets the same dial drive anything.
///
/// Two behaviours matter more than they look:
///
/// * **A dead zone at the centre.** Near the middle a small movement swings the
///   angle wildly -- at two pixels from centre, one pixel of travel is 30
///   degrees. Without the dead zone a resting thumb would spin the dial.
/// * **Sub-step movement accumulates.** Dragging slowly must not be silently
///   discarded and then jump; the remainder carries between calls, so a slow
///   drag and a fast one over the same arc produce the same total.
/// Geometry and resolution. At namespace scope rather than nested in `Dial`,
/// because a nested type's default member initializers are not available for a
/// default argument inside the enclosing class definition.
struct DialConfig {
    int16_t cx = 120;          ///< centre, panel coordinates
    int16_t cy = 140;
    int16_t deadZone = 30;     ///< inside this radius, ignore the touch
    int16_t coarseRadius = 72; ///< outside this, report coarse
    int stepsPerTurn = 60;     ///< one full revolution
};

class Dial {
public:
    using Config = DialConfig;

    Dial() = default;
    explicit Dial(const DialConfig& cfg) : cfg_(cfg) {}

    /// Feed one touch sample. Returns steps moved since the previous call:
    /// positive clockwise, negative anticlockwise, zero when not tracking.
    int update(bool pressed, int16_t x, int16_t y);

    /// True when the last tracked touch was in the outer band. Callers use this
    /// to scale a step -- coarse out at the rim, fine near the middle -- so one
    /// gesture covers both "about twenty minutes" and "twenty past".
    bool coarse() const { return coarse_; }

    /// True while a finger is being tracked.
    bool tracking() const { return tracking_; }

    /// Forget the current drag. Call when the dial stops being the active
    /// control, or a later touch will be measured against a stale angle.
    void reset();

private:
    DialConfig cfg_;
    bool tracking_ = false;
    bool coarse_ = false;
    float lastAngle_ = 0.0f;
    float residual_ = 0.0f; ///< sub-step remainder, carried between calls
};

} // namespace h0
