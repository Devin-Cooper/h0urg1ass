#pragma once

#include <cstdint>

#include "sand/sand_render.hpp"
#include "sand/sand_sim.hpp"

namespace h0 {

/// The hourglass: a vessel, a charge of sand, an attractor and a metered gate.
///
/// Wraps `SandSim` with the three things a *timer* needs that a falling-sand
/// toy does not:
///
///  * a **centreline attractor**, without which the upper chamber never empties;
///  * a **gate** at the hole, so the drain follows the clock instead of physics;
///  * a **charge** sized to the duration.
class SandVessel {
public:
    /// Hole half-width, in cells. Free flow is exactly `2*hw+1` grains per tick
    /// on a flat floor -- linear and jam-free at every width tested, unlike the
    /// tapered vessel this replaced, where narrow gates jammed outright.
    static constexpr int kHoleHalf = 2;

    /// Attractor strength, in parts per thousand.
    ///
    /// **Not optional.** On a flat floor only material above a 45-degree cone
    /// rising from the hole edge can ever reach it; everything else is a stable
    /// wedge and 33-86% of the sand strands forever. Measured residual drops to
    /// zero with this on.
    ///
    /// The ceiling is about 500: beyond that the far flank is scraped to a film
    /// while a mound stacks against the hole, and it stops reading as sand.
    static constexpr int kAttractorPerMille = 400;

    void begin();

    /// Charge the upper chamber with roughly `grains` and restart the drain.
    void reset(uint32_t seed, int grains);

    void setGravity(Gravity g) { gravity_ = g; }

    /// Advance one tick. `fractionRemaining` is 1.0 at full and 0.0 at expiry;
    /// the gate opens only while the sand is behind that schedule, which is what
    /// makes the drain track the clock rather than the physics.
    void tick(float fractionRemaining);

    const SandGrid& sand() const { return sim_.sand(); }

    /// What to DRAW -- which is not what the sand collides with.
    ///
    /// The two roles split at the lintel: it is solid to the physics so no grain
    /// can ever be inside the readout, but only its jambs and soffit are inked,
    /// so `renderSand` leaves the interior white. Returning `open_` here instead
    /// would paint the housing solid black, which is precisely the black-on-black
    /// failure the lintel exists to prevent.
    ///
    /// The gate is not reflected either: the shut shape is a physics detail, and
    /// drawing it would make the hole blink shut several times a second.
    const SandGrid& walls() const { return drawn_; }

    int charge() const { return charge_; }
    int lowerCount() const;

private:
    void setGate(bool open);
    int runAttractor();
    bool solid(int x, int y) const;
    bool resting(int x, int y) const;
    uint32_t next();

    SandSim sim_;
    SandGrid open_;  ///< physics, gate open
    SandGrid shut_;  ///< physics, gate shut
    SandGrid drawn_; ///< ink
    Gravity gravity_ = Gravity::S;
    uint32_t prng_ = 1;
    int charge_ = 0;
    bool gateOpen_ = true;
};

} // namespace h0
