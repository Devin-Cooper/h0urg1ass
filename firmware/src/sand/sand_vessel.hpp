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

    /// Gate gain, on grains-behind-schedule. Pure proportional -- see tick().
    /// Measured against the old bang-bang gate over 300 s and 1800 s drains:
    /// zero full-width discharges out of ~2,000 events against 89%, every event
    /// exactly one grain, 4.7x as many ticks with visible flow, longest silence
    /// down from 4.47 s to 1.67 s, and timekeeping equal or better.
    static constexpr float kGateGain = 0.02f;

    /// Top speed of a grain in free fall, cells per tick.
    ///
    /// Four is the knee. Measured, a 58-row drop takes 60 ticks at one cell per
    /// tick and 18 at four; going to six saves only three more ticks while
    /// doubling the per-frame jump to 12 px, which at 30 Hz starts to strobe
    /// rather than fall.
    static constexpr int kMaxFallSpeed = 4;

    /// Lateral drift strength at full agitation and a full half-sector of tilt.
    static constexpr int kDriftPerMille = 300;

    void begin();

    /// Charge the upper chamber with roughly `grains` and restart the drain.
    void reset(uint32_t seed, int grains);

    /// Set gravity from the filtered accelerometer vector, in PANEL axes, plus
    /// how much the device is being handled.
    ///
    /// Takes the vector rather than a quantised `Gravity` so the sub-sector
    /// remainder survives. Eight directions means a tilt does nothing at all
    /// until 22.5 degrees; the remainder is what turns that cliff into a ramp,
    /// by biasing grains sideways in proportion to how far past the sector
    /// centre the device actually is.
    void setTilt(float gx, float gy, float agitation);

    /// Direct control, for tests and for callers with no accelerometer.
    void setGravity(Gravity g) {
        gravity_ = g;
        driftPerMille_ = 0;
        agitation_ = 0.0f;
    }

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
    void applyGate(float p);
    int runAttractor(int perMille);
    int runDrift();
    bool buried(int x, int y) const;
    bool solid(int x, int y) const;
    bool resting(int x, int y) const;
    uint32_t next();

    SandSim sim_;
    SandGrid open_;  ///< physics, aperture clear
    SandGrid drawn_; ///< ink

    /// Cells something has already moved into LATERALLY this tick.
    ///
    /// The attractor and the drift both move resting grains sideways, so without
    /// a shared mask a grain could be picked up by one and then the other and
    /// travel two cells in a tick -- the same class of bug the simulation's own
    /// moved_ mask exists to prevent for falls.
    SandGrid latMoved_;

    /// Per-cell error-diffusion accumulators for the aperture. See applyGate:
    /// these are what make the hole trickle instead of discharging in bars.
    float gateAcc_[2 * kHoleHalf + 1] = {0.0f};

    Gravity gravity_ = Gravity::S;
    int driftPerMille_ = 0;   ///< lateral bias strength, from the sub-sector tilt
    int driftDx_ = 0;         ///< which way the bias points
    int driftDy_ = 0;
    float agitation_ = 0.0f;  ///< 0 on a desk, 1 in the hand
    uint32_t prng_ = 1;
    int charge_ = 0;
};

} // namespace h0
