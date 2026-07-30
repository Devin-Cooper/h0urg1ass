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

    /// Advance one tick under `g`.
    ///
    /// Returns the number of grains that moved, which is what "settled" is
    /// measured by -- a pile at rest costs almost nothing and lets the caller
    /// skip work entirely.
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

    SandGrid sand_;
    SandGrid walls_;
    uint32_t prng_ = 1;
};

/// The three offsets a grain tries under a given gravity: straight ahead, then
/// the two diagonals. Data rather than branches, so the inner loop never asks
/// which way is down.
struct GravityOffsets {
    int8_t mx, my;   ///< straight ahead
    int8_t lx, ly;   ///< first diagonal
    int8_t rx, ry;   ///< second diagonal
};

GravityOffsets offsetsFor(Gravity g);

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
