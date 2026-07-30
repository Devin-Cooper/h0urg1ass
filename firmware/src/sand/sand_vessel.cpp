#include "sand/sand_vessel.hpp"

namespace h0 {

namespace {
constexpr int HOLE_X = sandgeom::HOLE_CX;
constexpr int FLOOR = sandgeom::FLOOR_ROW;
} // namespace

uint32_t SandVessel::next() {
    prng_ ^= prng_ << 13;
    prng_ ^= prng_ >> 17;
    prng_ ^= prng_ << 5;
    return prng_;
}

void SandVessel::begin() {
    const SandGrid base = makeVessel(kHoleHalf);

    // Physics gets the lintel SOLID; ink gets only its outline. Splitting the
    // two roles here is the whole legibility mechanism -- see walls().
    open_ = base;
    fillLintelSolid(open_);

    drawn_ = base;
    drawLintelOutline(drawn_);

    // The shut vessel bricks up EVERY hole cell.
    //
    // The tempting refinement -- wall only the hole cells that are empty right
    // now, so no grain is ever caught inside a wall -- does not close the gate
    // at all. Within one tick the grain sitting in the hole falls out, the cell
    // it vacated is still unwalled, and the grain above drops straight into it.
    // The hole passes its full width every tick while reporting itself shut,
    // and a ten-minute timer empties in seconds.
    //
    // Walling the lot leaves at most one row of grains momentarily inside a
    // wall. They fall clear on the next tick, because a wall blocks what tries
    // to ENTER a cell, not what is already in it.
    shut_ = open_;
    for (int x = HOLE_X - kHoleHalf; x <= HOLE_X + kHoleHalf; ++x) {
        shut_.set(x, FLOOR, true);
    }

    sim_.setWalls(open_);
    gateOpen_ = true;
}

void SandVessel::reset(uint32_t seed, int grains) {
    prng_ = seed ? seed : 1u;
    sim_.seed(seed ? seed : 1u);
    sim_.sand().clear();
    setGate(true);

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
                if (open_.get(x, y) || sim_.sand().get(x, y)) continue;
                sim_.sand().set(x, y, true);
                ++placed;
            }
            if (d > 0 && placed < grains) {
                const int x = HOLE_X + d;
                if (x <= SandGrid::W - 2 && !open_.get(x, y) && !sim_.sand().get(x, y)) {
                    sim_.sand().set(x, y, true);
                    ++placed;
                }
            }
        }
    }
    charge_ = sim_.sand().count();
}

bool SandVessel::solid(int x, int y) const {
    return open_.get(x, y) || sim_.sand().get(x, y);
}

bool SandVessel::resting(int x, int y) const {
    // Only nudge grains that cannot fall, so the attractor never fights one
    // that is already in free fall.
    return solid(x, y + 1) && solid(x - 1, y + 1) && solid(x + 1, y + 1);
}

int SandVessel::runAttractor() {
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
            if (!resting(x, y)) continue;
            if (solid(x + step, y)) continue;
            if (static_cast<int>(next() % 1000u) >= kAttractorPerMille) continue;
            s.set(x, y, false);
            s.set(x + step, y, true);
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

void SandVessel::setGate(bool open) {
    if (open == gateOpen_) return;
    sim_.setWalls(open ? open_ : shut_);
    gateOpen_ = open;
}

int SandVessel::lowerCount() const {
    return sim_.sand().countRows(FLOOR, SandGrid::H - 1);
}

void SandVessel::tick(float fractionRemaining) {
    // Deadbeat position control on a cumulative count, not a rate loop. The
    // plant is a pure counter and the measurement is exact, so a PI controller
    // would only add overshoot to a system that has none: open the gate while
    // behind schedule, shut it while ahead.
    if (fractionRemaining < 0.0f) fractionRemaining = 0.0f;
    if (fractionRemaining > 1.0f) fractionRemaining = 1.0f;
    const int target =
        static_cast<int>(static_cast<float>(charge_) * (1.0f - fractionRemaining));
    setGate(lowerCount() < target);

    // Attractor first, then gravity. The attractor only moves grains that
    // cannot fall, so running it first cannot steal a grain from free fall.
    runAttractor();
    sim_.step(gravity_);
}

} // namespace h0
