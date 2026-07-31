#include "sand/sand_vessel.hpp"

namespace h0 {

namespace {
constexpr int HOLE_X = sandgeom::HOLE_CX;
constexpr int FLOOR = sandgeom::FLOOR_ROW;
constexpr int HOLE_W = 2 * SandVessel::kHoleHalf + 1;

/// sin(22.5 deg) -- the half-width of a gravity sector, and the value the
/// shipped "both axes past 0.38" quantiser test is really comparing against.
constexpr float kSinHalfSector = 0.38268343f;
} // namespace

uint32_t SandVessel::next() {
    prng_ ^= prng_ << 13;
    prng_ ^= prng_ >> 17;
    prng_ ^= prng_ << 5;
    return prng_;
}

void SandVessel::begin() {
    // One grid, for both roles. The lintel used to make these two differ: it
    // was solid to the physics and drawn only as an outline, so sand could not
    // enter the readout and the interior still rendered white. The readout is
    // an opaque composited panel now, so the vessel has no such region and
    // there is nothing left for a second grid to say -- see walls().
    walls_ = makeVessel(kHoleHalf);

    sim_.setWalls(walls_);
    sim_.setMaxFallSpeed(kMaxFallSpeed);

    // Staggered, NOT all zero. Every cell of the aperture is handed the same
    // probability every tick, so accumulators that start together cross their
    // threshold together and the hole discharges its full width in one tick --
    // which is exactly the bar this mechanism replaced, arrived at by a
    // different route. Measured with a flat start: 381 of 425 flow events were
    // the full five cells, indistinguishable from the old gate. Staggered:
    // zero out of 1,995.
    for (int i = 0; i < HOLE_W; ++i) {
        gateAcc_[i] = static_cast<float>(i) / static_cast<float>(HOLE_W);
    }
}

void SandVessel::reset(uint32_t seed, int grains) {
    prng_ = seed ? seed : 1u;
    sim_.seed(seed ? seed : 1u);
    sim_.sand().clear();

    // Charge as a heap over the hole rather than a level slab. A slab pressed
    // against the floor drains about 70% slower, because every grain has to
    // creep to the centre before it can leave -- and a slab is exactly what a
    // flip produces, so measuring from a tidy heap flatters the design.
    int placed = 0;
    for (int y = FLOOR - 1; y > 0 && placed < grains; --y) {
        const int reach = FLOOR - y; // 45-degree heap, apex over the hole
        for (int d = 0; d <= reach && placed < grains; ++d) {
            for (int s = (d == 0 ? 0 : -1); s <= 0; ++s) {
                const int x = (d == 0) ? HOLE_X : (s < 0 ? HOLE_X - d : HOLE_X + d);
                if (x < 1 || x > SandGrid::W - 2) continue;
                if (walls_.get(x, y) || sim_.sand().get(x, y)) continue;
                sim_.sand().set(x, y, true);
                ++placed;
            }
            if (d > 0 && placed < grains) {
                const int x = HOLE_X + d;
                if (x <= SandGrid::W - 2 && !walls_.get(x, y) && !sim_.sand().get(x, y)) {
                    sim_.sand().set(x, y, true);
                    ++placed;
                }
            }
        }
    }
    charge_ = sim_.sand().count();
}

bool SandVessel::solid(int x, int y) const {
    return walls_.get(x, y) || sim_.sand().get(x, y);
}

bool SandVessel::resting(int x, int y) const {
    // Only nudge grains that cannot fall, so a lateral pass never fights one
    // already in free fall. Asked in GRAVITY's frame, not the screen's: at 30
    // degrees gravity is SE, and "is there something below it on screen" is the
    // wrong question.
    const GravityOffsets o = offsetsFor(gravity_);
    return solid(x + o.mx, y + o.my) && solid(x + o.lx, y + o.ly) &&
           solid(x + o.rx, y + o.ry);
}

bool SandVessel::buried(int x, int y) const {
    // A grain with material directly on top of it is under load; only the
    // surface should creep sideways.
    const GravityOffsets o = offsetsFor(gravity_);
    return solid(x - o.mx, y - o.my);
}

void SandVessel::setTilt(float gx, float gy, float agitation) {
    agitation_ = agitation < 0.0f ? 0.0f : (agitation > 1.0f ? 1.0f : agitation);

    // Quantise exactly as the shipped classifier does, then KEEP THE REMAINDER.
    const float ax = gx < 0 ? -gx : gx;
    const float ay = gy < 0 ? -gy : gy;
    const bool diag = (ax > kSinHalfSector && ay > kSinHalfSector);
    Gravity q;
    if (diag) {
        if (gy > 0) q = gx > 0 ? Gravity::SE : Gravity::SW;
        else        q = gx > 0 ? Gravity::NE : Gravity::NW;
    } else if (ay >= ax) {
        q = gy > 0 ? Gravity::S : Gravity::N;
    } else {
        q = gx > 0 ? Gravity::E : Gravity::W;
    }
    gravity_ = q;

    // How far past the sector centre the device is, signed, as a fraction of a
    // half-sector. This is the entire fix for the dead zone: below 22.5 degrees
    // the quantised direction does not change, but this does, continuously.
    const GravityOffsets o = offsetsFor(q);
    const float norm = (o.mx && o.my) ? 0.70710678f : 1.0f;
    const float qx = static_cast<float>(o.mx) * norm;
    const float qy = static_cast<float>(o.my) * norm;

    // z of the cross product of the sector centre with the true direction: its
    // sign says which side of centre, its magnitude how far.
    const float cross = qx * gy - qy * gx;
    float r = cross / kSinHalfSector;
    if (r < -1.0f) r = -1.0f;
    if (r > 1.0f) r = 1.0f;
    const float mag = r < 0 ? -r : r;

    driftPerMille_ = static_cast<int>(kDriftPerMille * agitation_ * mag);

    // Perpendicular to gravity, on the side the remainder points: two steps
    // round the eight-direction ring.
    const int k = static_cast<int>(q);
    const GravityOffsets d = offsetsFor(static_cast<Gravity>((r >= 0 ? k + 2 : k + 6) & 7));
    driftDx_ = d.mx;
    driftDy_ = d.my;
}

int SandVessel::runDrift() {
    if (driftPerMille_ <= 0) return 0;
    SandGrid& s = sim_.sand();
    int moves = 0;

    // Walk AGAINST the drift, so a moved grain always lands on a cell already
    // visited this pass and cannot be picked up twice.
    const bool revX = driftDx_ > 0;
    const bool revY = driftDy_ > 0;
    for (int iy = 0; iy < SandGrid::H; ++iy) {
        const int y = revY ? (SandGrid::H - 1 - iy) : iy;
        for (int ix = 0; ix < SandGrid::W; ++ix) {
            const int x = revX ? (SandGrid::W - 1 - ix) : ix;
            if (!s.get(x, y)) continue;
            if (latMoved_.get(x, y)) continue;
            if (!resting(x, y) || buried(x, y)) continue;
            const int nx = x + driftDx_, ny = y + driftDy_;
            if (solid(nx, ny) || latMoved_.get(nx, ny)) continue;
            if (static_cast<int>(next() % 1000u) >= driftPerMille_) continue;
            s.set(x, y, false);
            s.set(nx, ny, true);
            latMoved_.set(nx, ny, true);
            ++moves;
        }
    }
    return moves;
}

int SandVessel::runAttractor(int perMille) {
    if (perMille <= 0) return 0;
    int moves = 0;
    SandGrid& s = sim_.sand();

    // Each half of a row is walked from the centre OUTWARDS and grains nudged
    // inwards -- always into a cell already visited this pass. That ordering is
    // what stops a nudged grain being picked up again and skating several cells
    // in one tick.
    auto inwardHalf = [&](int y, int dir) {
        const int step = -dir;
        for (int x = HOLE_X + dir; x >= 1 && x <= SandGrid::W - 2; x += dir) {
            if (!s.get(x, y)) continue;
            if (latMoved_.get(x, y)) continue;
            if (!resting(x, y)) continue;
            if (solid(x + step, y) || latMoved_.get(x + step, y)) continue;
            if (static_cast<int>(next() % 1000u) >= perMille) continue;
            s.set(x, y, false);
            s.set(x + step, y, true);
            latMoved_.set(x + step, y, true);
            ++moves;
        }
    };

    for (int y = FLOOR - 1; y >= 1; --y) {
        // Which half goes first is a coin flip PER ROW. The halves are not
        // independent -- both write into the hole's own column -- so a fixed
        // order hands the same side first claim on every tick of a run
        // thousands of ticks long, and one flank ends up scraped bare while the
        // other still stands at full height.
        const bool leftFirst = (next() & 1u) != 0;
        inwardHalf(y, leftFirst ? -1 : 1);
        inwardHalf(y, leftFirst ? 1 : -1);
    }
    return moves;
}

void SandVessel::applyGate(float p) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    // Each cell of the aperture decides for ITSELF, every tick, by error
    // diffusion rather than by a coin.
    //
    // The bar the old gate produced was not caused by the gate being binary. It
    // was caused by the five cells being perfectly CORRELATED -- and a per-cell
    // accumulator that starts them in phase reproduces it exactly, which is how
    // the cause was identified. Staggering the phases in begin() is the fix.
    //
    // Deterministic rather than random on purpose: a coin gives the same mean
    // rate but clumps, while error diffusion spreads a given number of openings
    // as evenly as they can be spread.
    SandGrid& w = sim_.wallsMut();
    for (int i = 0; i < HOLE_W; ++i) {
        gateAcc_[i] += p;
        const bool pass = gateAcc_[i] >= 1.0f;
        if (pass) gateAcc_[i] -= 1.0f;
        // A wall blocks what tries to ENTER a cell, not what already sits in it,
        // so a grain caught by a cell that has just closed falls clear on the
        // next tick rather than being trapped inside the floor.
        w.set(HOLE_X - kHoleHalf + i, FLOOR, !pass);
    }
}

int SandVessel::lowerCount() const {
    return sim_.sand().countRows(FLOOR, SandGrid::H - 1);
}

void SandVessel::tick(float fractionRemaining) {
    // Proportional control on a cumulative count. The plant is a pure counter
    // and the measurement is exact, so no integrator is wanted: the count is
    // already an integral, and a second one only buys overshoot.
    if (fractionRemaining < 0.0f) fractionRemaining = 0.0f;
    if (fractionRemaining > 1.0f) fractionRemaining = 1.0f;
    const int target =
        static_cast<int>(static_cast<float>(charge_) * (1.0f - fractionRemaining));
    const int err = target - lowerCount();
    applyGate(err > 0 ? kGateGain * static_cast<float>(err) : 0.0f);

    latMoved_.clear();

    // While the device is being handled the sand should behave like sand; once
    // it is put down the timer resumes. The attractor is a clock mechanism
    // rather than physics, so it fades out exactly as the drift fades in, and
    // the two share latMoved_ so no grain moves twice sideways in a tick.
    runDrift();
    runAttractor(static_cast<int>(kAttractorPerMille * (1.0f - agitation_)));

    sim_.step(gravity_);
}

} // namespace h0
