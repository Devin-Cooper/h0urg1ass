#include "sand/sand_sim.hpp"

namespace h0 {

GravityOffsets offsetsFor(Gravity g) {
    // Straight ahead plus the two neighbours either side of it. Every entry
    // moves exactly one cell, which is what keeps sand from tunnelling through
    // a one-cell wall.
    switch (g) {
        case Gravity::S:  return {0, 1, -1, 1, 1, 1};
        case Gravity::SW: return {-1, 1, -1, 0, 0, 1};
        case Gravity::W:  return {-1, 0, -1, -1, -1, 1};
        case Gravity::NW: return {-1, -1, 0, -1, -1, 0};
        case Gravity::N:  return {0, -1, -1, -1, 1, -1};
        case Gravity::NE: return {1, -1, 0, -1, 1, 0};
        case Gravity::E:  return {1, 0, 1, -1, 1, 1};
        case Gravity::SE: return {1, 1, 1, 0, 0, 1};
    }
    return {0, 1, -1, 1, 1, 1};
}

bool scanReverseY(Gravity g) {
    // Visit cells furthest along the gravity direction FIRST, so a cell that
    // has already moved is never revisited in the same tick.
    switch (g) {
        case Gravity::S:
        case Gravity::SW:
        case Gravity::SE: return true; // gravity has +y, so scan from the bottom
        default: return false;
    }
}

bool scanReverseX(Gravity g) {
    switch (g) {
        case Gravity::E:
        case Gravity::NE:
        case Gravity::SE: return true; // gravity has +x, so scan from the right
        default: return false;
    }
}

void SandSim::setWalls(const SandGrid& walls) { walls_ = walls; }

bool SandSim::tryMove(int x, int y, int nx, int ny) {
    if (blocked(nx, ny)) return false;
    moved_.set(nx, ny, true);
    // Paired clear/set. This is the whole of the conservation argument: a grain
    // is never created or destroyed, only relocated, and the two halves cannot
    // be separated by any interleaving because there is no interleaving.
    sand_.set(x, y, false);
    sand_.set(nx, ny, true);
    return true;
}

int SandSim::step(Gravity g) {
    const GravityOffsets o = offsetsFor(g);
    const bool revY = scanReverseY(g);

    moved_.clear();
    int moved = 0;

    for (int iy = 0; iy < SandGrid::H; ++iy) {
        const int y = revY ? (SandGrid::H - 1 - iy) : iy;

        const uint32_t r = next();

        // Which diagonal to try first, chosen per ROW rather than per frame.
        // A single global toggle leaves a visible two-frame shear across the
        // whole body; per-row randomness scatters the bias instead of moving it
        // around in lockstep.
        const bool leftFirst = (r & 1u) != 0;

        // The scan direction along the row is randomised too. A fixed direction
        // is a systematic shear: whichever end is visited first gets to move
        // into the space, every row, every tick. Measured, that pushed the
        // left/right mass split to 0.53-0.63 -- a squeegee rather than a hopper
        // -- against 0.01-0.09 once randomised. Safe to randomise only because
        // the moved-mask makes correctness independent of order.
        const bool revX = (r & 2u) != 0;

        for (int ix = 0; ix < SandGrid::W; ++ix) {
            const int x = revX ? (SandGrid::W - 1 - ix) : ix;
            if (!sand_.get(x, y)) continue;
            if (moved_.get(x, y)) continue; // arrived here this tick

            if (tryMove(x, y, x + o.mx, y + o.my)) { ++moved; continue; }

            const int8_t ax = leftFirst ? o.lx : o.rx;
            const int8_t ay = leftFirst ? o.ly : o.ry;
            const int8_t bx = leftFirst ? o.rx : o.lx;
            const int8_t by = leftFirst ? o.ry : o.ly;

            if (tryMove(x, y, x + ax, y + ay)) { ++moved; continue; }
            if (tryMove(x, y, x + bx, y + by)) { ++moved; continue; }
        }
    }
    return moved;
}

} // namespace h0
