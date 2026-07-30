#pragma once

#include <cstdint>

#include "sand/sand_grid.hpp"

namespace h0 {

/// Gravity, quantised to eight directions.
///
/// Eight, not sixteen. A sixteen-direction table needs knight-shaped offsets
/// that move two cells at once, and a two-cell step tunnels straight through a
/// one-cell wall -- the sand escapes the vessel. Intermediate angles are better
/// had by dithering between adjacent directions across frames than by adding
/// offsets that can jump the geometry.
enum class Gravity : uint8_t { S, SW, W, NW, N, NE, E, SE };

/// The three offsets a grain tries under a given gravity: straight ahead, then
/// the two diagonals. Data rather than branches, so the inner loop never asks
/// which way is down.
struct GravityOffsets {
    int8_t mx, my;   ///< straight ahead
    int8_t lx, ly;   ///< first diagonal
    int8_t rx, ry;   ///< second diagonal
};

GravityOffsets offsetsFor(Gravity g);

/// Falling-sand cellular automaton.
///
/// In place, never double-buffered. Double buffering is the intuitive choice and
/// it is wrong twice over: a column of grains unzips into a dashed comb instead
/// of collapsing, and it *destroys mass* when a faller and a diagonal slider
/// claim the same cell in the same tick. An hourglass whose sand evaporates is
/// not a timer. In-place with a paired clear/set is exactly conserving, which is
/// the property the tests pin hardest.
class SandSim {
public:
    /// `walls` marks solid cells: the vessel, the dividing floor, everything the
    /// sand cannot enter. Copied, so the caller may discard it.
    void setWalls(const SandGrid& walls);

    SandGrid& sand() { return sand_; }
    const SandGrid& sand() const { return sand_; }
    const SandGrid& walls() const { return walls_; }

    /// The live wall grid, for a caller that needs to change a few cells every
    /// tick rather than swap the whole thing. The gate uses this: rewriting five
    /// hole cells beats copying 1,984 bytes, and it is what lets each cell of
    /// the aperture decide independently.
    SandGrid& wallsMut() { return walls_; }

    /// Top speed of a grain in free fall, in cells per tick. Zero disables the
    /// ballistic pass entirely and restores the original one-cell-per-tick
    /// behaviour, which is what the conservation tests compare against.
    void setMaxFallSpeed(int v) { vmax_ = static_cast<int8_t>(v); }

    /// Advance one tick under `g`.
    ///
    /// Returns the number of MOVES made, which is how "settled" is detected --
    /// zero means a fixpoint.
    ///
    /// A settled pile is NOT free, though: measured, an empty grid still costs
    /// about the same as a busy one, because every cell is still visited. If
    /// that becomes a problem the fix is a per-row occupancy word to skip empty
    /// rows, which is not implemented.
    int step(Gravity g);

    /// Seeded so a run is reproducible: same seed, same drain, which is what
    /// makes golden-image tests of a stochastic system possible at all.
    void seed(uint32_t s) { prng_ = s ? s : 1u; }

private:
    uint32_t next() {
        // xorshift32. Cheap, and good enough for choosing which way a grain
        // topples; nothing here needs statistical quality.
        prng_ ^= prng_ << 13;
        prng_ ^= prng_ >> 17;
        prng_ ^= prng_ << 5;
        return prng_;
    }

    bool blocked(int x, int y) const { return walls_.get(x, y) || sand_.get(x, y); }
    bool tryMove(int x, int y, int nx, int ny);

    /// A grain in free fall, and how fast it is going.
    ///
    /// Velocity lives in a list of the AIRBORNE grains rather than a plane
    /// parallel to the grid, because almost nothing is airborne: measured, a
    /// mean of 1.18 and a peak of 5 in steady state on a 900-grain drain. A
    /// 2-bit plane would spend 3 kB to store "zero" for 99.99% of the vessel.
    struct Faller {
        uint8_t x, y, v;
    };

    /// Three times the measured peak. Overflow is graceful -- a grain that finds
    /// no slot simply falls at one cell per tick, as it always did.
    static constexpr int kMaxFallers = 16;

    int stepFallers(const GravityOffsets& o);
    void recruit(int x, int y, const GravityOffsets& o);

    Faller fall_[kMaxFallers];
    int nfall_ = 0;
    int8_t vmax_ = 0;

    SandGrid sand_;
    SandGrid walls_;

    /// Cells a grain has already moved INTO this tick.
    ///
    /// Scan order alone cannot prevent a grain moving twice. Iterating against
    /// gravity handles the straight-ahead case, but the diagonals move on the
    /// *other* axis too -- under E or W gravity a grain slides into a row the
    /// outer loop has not reached yet, is visited again, and slides again.
    /// Measured before this existed: up to 51 cells in a single tick.
    ///
    /// With the mask, correctness no longer depends on scan order at all, which
    /// frees the order to be randomised purely to break up bias. The mask also
    /// pays for itself -- skipping settled cells measured faster than the
    /// unguarded version, not slower.
    SandGrid moved_;

    uint32_t prng_ = 1;
};



/// True when the scan must run in decreasing order on that axis.
///
/// The scan has to run AGAINST gravity, not simply bottom-to-top. Bottom-up is
/// only correct while gravity points down; tilt the device left and a
/// bottom-up scan lets a grain move, then be visited again in the same tick,
/// and a five-grain block teleports across the vessel in one frame. This is the
/// generalisation the usual advice omits.
bool scanReverseX(Gravity g);
bool scanReverseY(Gravity g);

} // namespace h0
