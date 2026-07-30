#pragma once

#include "faces/face.hpp"
#include "sand/sand_vessel.hpp"

namespace h0 {

/// A simulated hourglass: a horizontal floor with a hole, and real falling sand.
///
/// Replaces an earlier shaped fill -- a drawn region whose area tracked the
/// remaining fraction. What the simulation buys over that is a crater forming
/// over the hole as the upper chamber drains, grain-scale roughness on every
/// surface, a stream of discrete falling grains, and sand that responds to the
/// device being tilted. What it costs is a tick every frame.
///
/// The vessel is deliberately not an hourglass outline. A bowtie spends most of
/// its area on taper and its sloping walls are what strand grains; a flat floor
/// gives full-width chambers and a shape describable in two lines.
class HourglassFace : public IFace {
public:
    void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) override;
    const char* name() const override { return "hourglass"; }

    /// Meaningless without a duration to be a fraction of.
    bool supports(const TimerModel& t) const override { return t.duration() > 0; }

    /// Advance the simulation. Called on its own clock, NOT once per render:
    /// the sand must fall at a fixed rate regardless of how often the display
    /// happens to be redrawn.
    void tick(const TimerModel& t, uint64_t now);

    /// Restart with a charge sized to the duration.
    ///
    /// Short timers need FEW grains -- the attractor can only move so many per
    /// tick, so a large charge cannot drain in thirty seconds. Long timers need
    /// MANY, because metering granularity is one gate-burst per grain and a
    /// small charge makes a ten-minute drain visibly steppy.
    void restart(const TimerModel& t, uint32_t seed);

    void setGravity(Gravity g) { vessel_.setGravity(g); }

private:
    SandVessel vessel_;
    bool started_ = false;
    uint64_t lastTickUs_ = 0;
    uint64_t lastDuration_ = 0;
};

} // namespace h0
