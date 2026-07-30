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

void SandSim::recruit(int x, int y, const GravityOffsets& o) {
    if (vmax_ <= 0 || nfall_ >= kMaxFallers) return;
    // "Moved, and still has open space straight ahead" is the whole definition
    // of airborne. A grain that merely slid down a slope onto a solid bed is not
    // falling and must not accelerate.
    if (blocked(x + o.mx, y + o.my)) return;
    fall_[nfall_].x = static_cast<uint8_t>(x);
    fall_[nfall_].y = static_cast<uint8_t>(y);
    fall_[nfall_].v = 2; // it has already made this tick's first cell
    ++nfall_;
}

int SandSim::stepFallers(const GravityOffsets& o) {
    if (nfall_ <= 0) return 0;

    // Deepest-downstream first. Without this a grain stacked above another in
    // the same column rear-ends it, and both lose their speed for nothing. The
    // list is single-digit in steady state, so an insertion sort is the cheapest
    // thing that works.
    for (int i = 1; i < nfall_; ++i) {
        const Faller key = fall_[i];
        const int kp = key.x * o.mx + key.y * o.my;
        int j = i - 1;
        while (j >= 0 && (fall_[j].x * o.mx + fall_[j].y * o.my) < kp) {
            fall_[j + 1] = fall_[j];
            --j;
        }
        fall_[j + 1] = key;
    }

    int moved = 0;
    int out = 0;
    for (int i = 0; i < nfall_; ++i) {
        Faller f = fall_[i];
        int x = f.x, y = f.y;

        // The grid is the single source of truth. The attractor, the gate and a
        // reset can all remove a grain from under us, so a stale entry is
        // retired rather than trusted.
        if (!sand_.get(x, y)) continue;

        // Straight along gravity only, ONE CELL AT A TIME, testing before each.
        // Never diagonally: diagonals are what made a grain revisitable in the
        // cellular scan, and testing every intermediate cell is what stops a
        // fast grain tunnelling through a one-cell wall.
        int k = 0;
        for (; k < f.v; ++k) {
            const int nx = x + o.mx, ny = y + o.my;
            if (blocked(nx, ny)) break;
            sand_.set(x, y, false);
            sand_.set(nx, ny, true);
            x = nx;
            y = ny;
        }

        if (k > 0) {
            ++moved;
            moved_.set(x, y, true);
        }

        // A clean flight keeps its slot and speeds up; anything that hit
        // something drops off the list, which is how speed resets to one with no
        // per-cell storage at all.
        if (k == f.v && f.v < vmax_) {
            fall_[out].x = static_cast<uint8_t>(x);
            fall_[out].y = static_cast<uint8_t>(y);
            fall_[out].v = static_cast<uint8_t>(f.v + 1);
            ++out;
        } else if (k == f.v) {
            fall_[out].x = static_cast<uint8_t>(x);
            fall_[out].y = static_cast<uint8_t>(y);
            fall_[out].v = f.v;
            ++out;
        }
    }
    nfall_ = out;
    return moved;
}

int SandSim::step(Gravity g) {
    const GravityOffsets o = offsetsFor(g);
    const bool revY = scanReverseY(g);

    moved_.clear();
    int moved = 0;

    // Ballistics first, then the cellular scan. The two are kept strictly apart:
    // this pass moves only STRAIGHT along gravity and tests every cell it
    // crosses, while diagonals stay exclusively in the scan below and stay
    // exactly one cell. That separation is what preserves the property the
    // moved_ mask exists for -- bounded, intentional movement -- while still
    // letting a grain in clean air cover four cells in a tick.
    moved += stepFallers(o);

    for (int iy = 0; iy < SandGrid::H; ++iy) {
        const int y = revY ? (SandGrid::H - 1 - iy) : iy;

        // An empty-row skip was tried here and REMOVED: measured on the board,
        // it made the tick 27% SLOWER (4.49 -> 5.7 ms, same harness, four rounds
        // each, stable). `SandGrid::rowEmpty` is kept as a reasonable accessor,
        // but do not reintroduce the skip without re-measuring on hardware.
        //
        // The likely cause is specific to this part and invisible on a host:
        // code executes from external QSPI flash through a 16 kB XIP cache, so
        // an extra branch in the hot loop costs more in instruction-fetch misses
        // than it saves in skipped cells. The inner loop's own bit test is
        // already cheap enough that skipping it is not worth a branch.
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

            if (tryMove(x, y, x + o.mx, y + o.my)) {
                ++moved;
                recruit(x + o.mx, y + o.my, o);
                continue;
            }

            const int8_t ax = leftFirst ? o.lx : o.rx;
            const int8_t ay = leftFirst ? o.ly : o.ry;
            const int8_t bx = leftFirst ? o.rx : o.lx;
            const int8_t by = leftFirst ? o.ry : o.ly;

            if (tryMove(x, y, x + ax, y + ay)) {
                ++moved;
                recruit(x + ax, y + ay, o);
                continue;
            }
            if (tryMove(x, y, x + bx, y + by)) {
                ++moved;
                recruit(x + bx, y + by, o);
                continue;
            }
        }
    }
    return moved;
}

} // namespace h0
