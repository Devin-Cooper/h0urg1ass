// Throwaway prototype: a falling-sand cellular automaton running inside the
// shipped hourglass vessel.
//
// It exists to answer exactly one question -- does a grain simulation look
// enough better than the shaped fill to be worth carrying on the device? -- and
// to produce the numbers that decide whether it is affordable at all. It renders
// into the SAME glass (identical constants, identical taper), at the SAME fill
// fractions, in the SAME PBM format as the face renderer, so the two sets of
// frames can be put side by side and judged. Comparing against a differently
// shaped vessel would prove nothing.
//
// This file is deliberately naive: one cell per grain, plain nested loops, no
// bit-parallel tricks. It is a measuring instrument, not a shipping
// implementation. Clarity beats speed here; the RP2350 cost is extrapolated
// from the measured cells-per-tick rather than benchmarked directly.
//
//   sand_proto <outdir> [--grain N] [--seconds T] [--reach R]
//                       [--roll P] [--attract Q] [--seed S] [--dither]
//
// With no --grain, all three candidate grain sizes (2, 3, 4 px) are run. Each
// run also writes shaped-<fraction>.pbm from the shipped face, so one command
// produces both halves of the comparison.

#include <1bit/core/framebuffer.hpp>
#include <1bit/render/pattern.hpp>
#include <1bit/render/primitives.hpp>

#include "faces/hourglass_face.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using onebit::BLACK;
using onebit::WHITE;

using Panel = onebit::Framebuffer<240, 280>;

// ---------------------------------------------------------------- geometry --
//
// Lifted verbatim from the shipped face. If these ever diverge the comparison
// stops meaning anything, so they are copied rather than approximated.

constexpr int16_t CX = 120;
constexpr int16_t TOP = 34;   ///< top of the glass
constexpr int16_t NECK = 148; ///< the waist
constexpr int16_t BOT = 250;  ///< bottom of the glass
constexpr int16_t HW_MAX = 74;
constexpr int16_t HW_NECK = 5;

/// Half-width of the glass interior at row y. Zero outside the glass.
int16_t halfWidthAt(int16_t y) {
    if (y < TOP || y > BOT) return 0;
    if (y <= NECK) {
        const int32_t span = NECK - TOP;
        return static_cast<int16_t>(HW_MAX + (static_cast<int32_t>(HW_NECK - HW_MAX) * (y - TOP)) / span);
    }
    const int32_t span = BOT - NECK;
    return static_cast<int16_t>(HW_NECK + (static_cast<int32_t>(HW_MAX - HW_NECK) * (y - NECK)) / span);
}

void drawGlass(onebit::IFramebuffer& fb) {
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), TOP,
                     static_cast<int16_t>(CX + HW_MAX), TOP, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), BOT,
                     static_cast<int16_t>(CX + HW_MAX), BOT, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), TOP,
                     static_cast<int16_t>(CX - HW_NECK), NECK, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX + HW_MAX), TOP,
                     static_cast<int16_t>(CX + HW_NECK), NECK, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_NECK), NECK,
                     static_cast<int16_t>(CX - HW_MAX), BOT, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX + HW_NECK), NECK,
                     static_cast<int16_t>(CX + HW_MAX), BOT, BLACK);
}

// --------------------------------------------------------------------- io --

/// Binary PBM (P4): one bit per pixel, 1 = black, rows padded to whole bytes.
/// Byte-for-byte the same writer the face renderer uses, so the existing
/// pbm_to_png tool reads these without changes.
bool writePbm(const onebit::IFramebuffer& fb, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P4\n%d %d\n", fb.width(), fb.height());
    const int stride = (fb.width() + 7) / 8;
    for (int16_t y = 0; y < fb.height(); ++y) {
        for (int b = 0; b < stride; ++b) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int16_t x = static_cast<int16_t>(b * 8 + bit);
                if (x < fb.width() && fb.getPixel(x, y) == onebit::BLACK) {
                    byte |= static_cast<uint8_t>(0x80u >> bit);
                }
            }
            std::fputc(byte, f);
        }
    }
    std::fclose(f);
    return true;
}

/// Output path for one frame. Assembled with std::string rather than a fixed
/// char buffer: a long output directory truncated the name in place and left
/// files the PNG tool could not open.
std::string framePath(const std::string& dir, const std::string& stem) {
    return dir + "/" + stem + ".pbm";
}

// ------------------------------------------------------------------- prng --

/// xorshift32. Seeded, so a run is reproducible and a surprising frame can be
/// gone back to.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    bool bit() { return (next() & 1u) != 0u; }
    bool percent(int p) { return static_cast<int>(next() % 100u) < p; }
};

// -------------------------------------------------------------- the grid ---

enum class Vessel { Glass, Box };

/// The automaton. Cells are square, `grain` pixels on a side.
class Sim {
public:
    int grain = 3;
    int cols = 0;
    int rows = 0;
    int plugRow = -1; ///< permanently blocked row at the neck; -1 in Box mode
    int centreCol = 0;

    // Tunables.
    int attractPct = 10; ///< centreline attractor probability, upper bulb only
    int reach = 0;       ///< lateral roll distance, in cells (0 = off)
    int rollPct = 0;     ///< lateral roll probability

    std::vector<uint8_t> open; ///< 1 = cell is inside the vessel
    std::vector<uint8_t> sand; ///< 1 = cell holds a grain
    Rng rng;

    // Valve plumbing: the open columns immediately above and below the plug,
    // ordered centre-outwards so the stream leaves and lands on the axis.
    std::vector<int> srcCols;
    std::vector<int> dstCols;

    Sim(int g, Vessel v, uint32_t seed) : grain(g), rng(seed) {
        if (v == Vessel::Glass) buildGlass();
        else buildBox();
        sand.assign(static_cast<size_t>(cols) * rows, 0);
        buildValveColumns();
    }

    int at(int r, int c) const { return r * cols + c; }
    bool inBounds(int r, int c) const { return r >= 0 && r < rows && c >= 0 && c < cols; }
    bool vacant(int r, int c) const {
        return inBounds(r, c) && open[static_cast<size_t>(at(r, c))] && !sand[static_cast<size_t>(at(r, c))];
    }

    int countSand() const {
        int n = 0;
        for (uint8_t s : sand) n += s;
        return n;
    }
    /// Grains in rows [r0, r1).
    int countSandRows(int r0, int r1) const {
        int n = 0;
        if (r0 < 0) r0 = 0;
        if (r1 > rows) r1 = rows;
        for (int r = r0; r < r1; ++r)
            for (int c = 0; c < cols; ++c) n += sand[static_cast<size_t>(at(r, c))];
        return n;
    }
    int countOpenRows(int r0, int r1) const {
        int n = 0;
        if (r0 < 0) r0 = 0;
        if (r1 > rows) r1 = rows;
        for (int r = r0; r < r1; ++r)
            for (int c = 0; c < cols; ++c) n += open[static_cast<size_t>(at(r, c))];
        return n;
    }

    /// One tick. Returns total cell moves; `valveMoved` receives the number of
    /// grains that crossed the plug.
    int tick(int valveWant, int* valveMoved) {
        int m = gravityPass();
        m += attractorPass();
        const int v = valvePass(valveWant);
        if (valveMoved) *valveMoved = v;
        return m + v;
    }

    /// Run until nothing moves at all. The angle of repose MUST be measured on
    /// a fully relaxed pile: sampling at a fixed iteration count reads whatever
    /// transient the pile happens to be in, which is how a published figure for
    /// this rule came out wrong.
    int relax(int maxTicks, int valveWant) {
        for (int t = 0; t < maxTicks; ++t) {
            int v = 0;
            if (tick(valveWant, &v) == 0) return t;
        }
        return maxTicks; // caller reports non-convergence
    }

private:
    void buildGlass() {
        cols = (2 * HW_MAX + 1 + grain - 1) / grain;
        rows = (BOT - TOP + 1 + grain - 1) / grain;
        centreCol = (CX - (CX - HW_MAX)) / grain;
        open.assign(static_cast<size_t>(cols) * rows, 0);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                open[static_cast<size_t>(at(r, c))] = cellInsideGlass(r, c) ? 1u : 0u;
            }
        }
        // The neck plug. Grains never pass it by falling; the valve teleports
        // them across. Throttling flow with a narrow aperture was measured to
        // have no usable operating point -- a gate of <=2 cells jams solid and
        // a gate of >=3 is indistinguishable from an open hole -- so the rate is
        // controlled explicitly instead, and the neck is simply shut.
        plugRow = (NECK - TOP) / grain;
        if (plugRow >= 0 && plugRow < rows) {
            for (int c = 0; c < cols; ++c) open[static_cast<size_t>(at(plugRow, c))] = 0;
        }
    }

    /// A cell counts as inside only if EVERY pixel it covers is interior. Being
    /// conservative here means a drawn grain can never land on top of the glass
    /// outline, which would read as a leak.
    bool cellInsideGlass(int r, int c) const {
        const int x0 = (CX - HW_MAX) + c * grain;
        const int y0 = TOP + r * grain;
        for (int y = y0; y < y0 + grain; ++y) {
            if (y < TOP || y > BOT) return false;
            const int hw = halfWidthAt(static_cast<int16_t>(y));
            for (int x = x0; x < x0 + grain; ++x) {
                if (std::abs(x - CX) > hw - 1) return false;
            }
        }
        return true;
    }

    /// A plain open box with a floor and side walls. Used only to measure the
    /// angle of repose of a free-standing pile, away from any glass wall that
    /// would pin the surface and bias the fit.
    void buildBox() {
        cols = 121;
        rows = 71;
        centreCol = cols / 2;
        open.assign(static_cast<size_t>(cols) * rows, 1);
        for (int c = 0; c < cols; ++c) open[static_cast<size_t>(at(rows - 1, c))] = 0; // floor
        for (int r = 0; r < rows; ++r) {
            open[static_cast<size_t>(at(r, 0))] = 0;
            open[static_cast<size_t>(at(r, cols - 1))] = 0;
        }
        plugRow = -1;
    }

    void buildValveColumns() {
        if (plugRow <= 0 || plugRow + 1 >= rows) return;
        auto gather = [&](int row, std::vector<int>& out) {
            out.clear();
            for (int c = 0; c < cols; ++c)
                if (open[static_cast<size_t>(at(row, c))]) out.push_back(c);
            // Centre-outwards, so the stream hugs the axis.
            for (size_t i = 1; i < out.size(); ++i) {
                const int v = out[i];
                const int kv = std::abs(v - centreCol);
                size_t j = i;
                while (j > 0 && std::abs(out[j - 1] - centreCol) > kv) {
                    out[j] = out[j - 1];
                    --j;
                }
                out[j] = v;
            }
        };
        gather(plugRow - 1, srcCols);
        gather(plugRow + 1, dstCols);
    }

    bool resting(int r, int c) const {
        return !vacant(r + 1, c) && !vacant(r + 1, c - 1) && !vacant(r + 1, c + 1);
    }

    void move(int rFrom, int cFrom, int rTo, int cTo) {
        sand[static_cast<size_t>(at(rFrom, cFrom))] = 0;
        sand[static_cast<size_t>(at(rTo, cTo))] = 1;
    }

    /// Gravity, sliding and the optional lateral roll, in one in-place bottom-up
    /// sweep.
    ///
    /// IN-PLACE IS NOT AN OPTIMISATION, IT IS THE ALGORITHM. Double-buffering
    /// this rule was measured to do two bad things: it unzips a falling column
    /// into a dashed comb (only every other grain advances per frame, because
    /// each reads the *old* state of the cell below), and it destroys mass -- a
    /// faller and a diagonal slider both write the same destination cell and one
    /// of them is silently deleted. A single paired clear/set on one array is
    /// exactly mass-conserving by construction.
    ///
    /// Scanning rows from the bottom up is what makes a resting column collapse
    /// coherently in one tick: the lowest grain vacates first, and everything
    /// above shuffles down into the space behind it.
    int gravityPass() {
        int moves = 0;
        for (int r = rows - 2; r >= 0; --r) {
            // Per-ROW random handedness. A single global per-frame toggle leaves
            // a visible two-frame shear across the whole body -- the entire mass
            // leans left, then right, in lockstep.
            const bool leftFirst = rng.bit();
            const int d1 = leftFirst ? -1 : 1;
            const int d2 = -d1;
            for (int i = 0; i < cols; ++i) {
                const int c = leftFirst ? i : (cols - 1 - i);
                if (!sand[static_cast<size_t>(at(r, c))]) continue;

                if (vacant(r + 1, c)) { move(r, c, r + 1, c); ++moves; continue; }
                if (vacant(r + 1, c + d1)) { move(r, c, r + 1, c + d1); ++moves; continue; }
                if (vacant(r + 1, c + d2)) { move(r, c, r + 1, c + d2); ++moves; continue; }

                // Lateral roll. The bare fall/slide rule has a hard angle of
                // repose of exactly 45 degrees: on a 45-degree staircase both
                // down-diagonals of every surface grain are occupied, so nothing
                // moves, and no amount of topple *probability* changes that --
                // probability only changes how fast the pile reaches 45, not
                // where it stops. Letting a surface grain travel further than
                // one cell before it drops is the only knob that moves the angle
                // itself. Real dry sand sits near 34 degrees.
                if (reach >= 1 && rollPct > 0 && rng.percent(rollPct)) {
                    int dir = d1;
                    int best = rollTarget(r, c, dir);
                    if (best == 0) { dir = d2; best = rollTarget(r, c, dir); }
                    if (best != 0) {
                        move(r, c, r + 1, c + dir * best);
                        ++moves;
                        continue;
                    }
                }
            }
        }
        return moves;
    }

    /// Farthest distance d in [1, reach] the grain can roll along the surface
    /// and then step down. Zero if it cannot. Taking the farthest rather than
    /// the nearest landing is what actually flattens a slope.
    int rollTarget(int r, int c, int dir) const {
        int best = 0;
        for (int d = 1; d <= reach; ++d) {
            if (!vacant(r, c + dir * d)) break; // the surface path is blocked
            if (vacant(r + 1, c + dir * d)) best = d;
        }
        return best;
    }

    /// Centreline attractor, upper bulb only.
    ///
    /// Mandatory, not decorative. Without it a large fraction of the charge
    /// comes to rest on the funnel wall in a configuration the fall/slide rule
    /// can never break, and the upper bulb never empties -- the timer visibly
    /// fails to finish. The nudge stands in for the wall friction gradient of a
    /// real conical hopper, where material creeps toward the orifice.
    ///
    /// Each half of the row is walked from the centre outwards and grains are
    /// nudged inwards, i.e. always into a cell that has already been visited.
    /// That is what keeps a nudged grain from being re-processed and skating
    /// several cells in a single tick.
    int attractorPass() {
        if (plugRow < 0 || attractPct <= 0) return 0;
        int moves = 0;
        for (int r = plugRow - 1; r >= 0; --r) {
            for (int c = centreCol - 1; c >= 0; --c) {
                if (!sand[static_cast<size_t>(at(r, c))]) continue;
                if (!resting(r, c)) continue;
                if (!rng.percent(attractPct)) continue;
                if (!vacant(r, c + 1)) continue;
                move(r, c, r, c + 1);
                ++moves;
            }
            for (int c = centreCol + 1; c < cols; ++c) {
                if (!sand[static_cast<size_t>(at(r, c))]) continue;
                if (!resting(r, c)) continue;
                if (!rng.percent(attractPct)) continue;
                if (!vacant(r, c - 1)) continue;
                move(r, c, r, c - 1);
                ++moves;
            }
        }
        return moves;
    }

    /// Deadbeat position control on the neck.
    ///
    /// The caller computes want = round(N * elapsed / T) - released, so the
    /// controller tracks a POSITION (total grains released) rather than a rate.
    /// Any tick where the throat cannot supply the demand leaves the deficit in
    /// place and it is made up later; nothing is ever discarded, which is why
    /// the timer still finishes on time after a stall.
    int valvePass(int want) {
        if (want <= 0 || srcCols.empty() || dstCols.empty()) return 0;
        int moved = 0;
        for (int k = 0; k < want; ++k) {
            int src = -1;
            for (int c : srcCols) {
                if (sand[static_cast<size_t>(at(plugRow - 1, c))]) { src = c; break; }
            }
            if (src < 0) break; // nothing has arrived at the throat yet
            int dst = -1;
            for (int c : dstCols) {
                if (vacant(plugRow + 1, c)) { dst = c; break; }
            }
            if (dst < 0) break; // the throat below is backed up
            move(plugRow - 1, src, plugRow + 1, dst);
            ++moved;
        }
        return moved;
    }
};

// ------------------------------------------------------------- rendering ---

/// Grains default to solid grain x grain blocks: that is the honest depiction,
/// because the simulation's resolution IS the block and hiding it under a
/// texture would flatter the comparison.
///
/// `dither` re-renders the same cells through the shipped face's exact pattern
/// instead. Solid-versus-dithered is a large perceptual difference all by
/// itself, so without this the two image sets differ in density AND in shape and
/// there is no way to tell which one is doing the work. Compare solid frames to
/// judge the real thing; compare dithered frames to judge the shape alone.
void renderSim(const Sim& s, onebit::IFramebuffer& fb, bool dither) {
    fb.clear(WHITE);
    drawGlass(fb);
    // Indexed off absolute framebuffer coordinates, exactly as the face does it,
    // so the texture stays screen-anchored and does not crawl with the sand.
    const onebit::Pattern sand = onebit::bayer(210, 8);
    for (int r = 0; r < s.rows; ++r) {
        for (int c = 0; c < s.cols; ++c) {
            if (!s.sand[static_cast<size_t>(s.at(r, c))]) continue;
            const int x0 = (CX - HW_MAX) + c * s.grain;
            const int y0 = TOP + r * s.grain;
            if (dither) {
                onebit::fillPatternRect(fb, static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                                        static_cast<int16_t>(s.grain),
                                        static_cast<int16_t>(s.grain), sand);
            } else {
                for (int y = y0; y < y0 + s.grain; ++y)
                    for (int x = x0; x < x0 + s.grain; ++x)
                        fb.setPixel(static_cast<int16_t>(x), static_cast<int16_t>(y), BLACK);
            }
        }
    }
}

// ------------------------------------------------- angle of repose (fit) ---

struct Repose {
    bool ok = false;
    double leftDeg = 0.0;
    double rightDeg = 0.0;
    double meanDeg = 0.0;
    int nLeft = 0;
    int nRight = 0;
};

double fitSlope(const std::vector<double>& xs, const std::vector<double>& ys) {
    const double n = static_cast<double>(xs.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        sxy += xs[i] * ys[i];
    }
    const double den = n * sxx - sx * sx;
    if (den == 0.0) return 0.0;
    return (n * sxy - sx * sy) / den;
}

/// Least-squares slope of each flank of the pile in rows [rFrom, rTo).
///
/// Columns whose surface cell already touches the vessel wall are dropped: there
/// the surface is pinned by the glass, not by the material, and including those
/// points drags the fit toward the wall angle.
Repose measureRepose(const Sim& s, int rFrom, int rTo) {
    Repose out;
    std::vector<int> top(static_cast<size_t>(s.cols), -1);
    for (int c = 0; c < s.cols; ++c) {
        for (int r = rFrom; r < rTo; ++r) {
            if (s.sand[static_cast<size_t>(s.at(r, c))]) { top[static_cast<size_t>(c)] = r; break; }
        }
    }
    int apex = -1;
    for (int c = 0; c < s.cols; ++c) {
        if (top[static_cast<size_t>(c)] < 0) continue;
        if (apex < 0 || top[static_cast<size_t>(c)] < top[static_cast<size_t>(apex)]) apex = c;
    }
    if (apex < 0) return out;

    auto flank = [&](int dir, std::vector<double>& xs, std::vector<double>& ys) {
        for (int c = apex + dir; c >= 0 && c < s.cols; c += dir) {
            const int t = top[static_cast<size_t>(c)];
            if (t < 0) break;
            const int outward = c + dir;
            if (outward < 0 || outward >= s.cols) break;
            if (!s.open[static_cast<size_t>(s.at(t, outward))]) break; // pinned by the wall
            xs.push_back(static_cast<double>(std::abs(c - apex)));
            ys.push_back(static_cast<double>(t));
        }
    };

    std::vector<double> lx, ly, rx, ry;
    flank(-1, lx, ly);
    flank(+1, rx, ry);
    out.nLeft = static_cast<int>(lx.size());
    out.nRight = static_cast<int>(rx.size());
    if (out.nLeft < 3 || out.nRight < 3) return out;

    const double kDeg = 180.0 / 3.14159265358979323846;
    out.leftDeg = std::atan(std::fabs(fitSlope(lx, ly))) * kDeg;
    out.rightDeg = std::atan(std::fabs(fitSlope(rx, ry))) * kDeg;
    out.meanDeg = 0.5 * (out.leftDeg + out.rightDeg);
    out.ok = true;
    return out;
}

// ------------------------------------------------------------ experiments --

constexpr int TICKS_PER_SEC = 30;

struct Level {
    double f;
    const char* tag;
};
const Level kLevels[] = {
    {1.00, "100"}, {0.85, "085"}, {0.70, "070"}, {0.50, "050"},
    {0.30, "030"}, {0.15, "015"}, {0.00, "000"},
};

/// Fill the upper bulb from the neck upward with `target` grains.
void chargeUpper(Sim& s, int target) {
    int placed = 0;
    for (int r = s.plugRow - 1; r >= 0 && placed < target; --r) {
        for (int c = 0; c < s.cols && placed < target; ++c) {
            if (!s.open[static_cast<size_t>(s.at(r, c))]) continue;
            s.sand[static_cast<size_t>(s.at(r, c))] = 1;
            ++placed;
        }
    }
}

/// Drain the whole charge with the valve demanding everything it can get, and
/// report the sustained crossing rate. This is the number that picks the grain
/// size: N grains must clear the neck within T seconds, so N / (T * 30) has to
/// sit under this ceiling with room to spare.
struct Throughput {
    double sustained = 0.0; ///< grains/tick over the middle half of the drain
    int peak = 0;           ///< best single tick
    int ticks = 0;
    int drained = 0;
    int charge = 0;
    int throatCells = 0; ///< open cells in the row just below the plug
    bool stalled = false;
};

Throughput measureThroughput(int grain, uint32_t seed, int attractPct, int reach, int rollPct) {
    Sim s(grain, Vessel::Glass, seed);
    s.attractPct = attractPct;
    s.reach = reach;
    s.rollPct = rollPct;
    const int upperOpen = s.countOpenRows(0, s.plugRow);
    const int lowerOpen = s.countOpenRows(s.plugRow + 1, s.rows);
    const int total = (upperOpen < lowerOpen) ? upperOpen : lowerOpen;
    chargeUpper(s, total);

    // Demand far more than the neck could conceivably pass, so that what gets
    // measured is the automaton's own limit and not the value of this constant.
    // The valve can never place more grains in a tick than there are vacant
    // cells in the row below the plug, so the throat width is a hard bound and
    // this number only has to sit above it.
    const int cap = 1000;

    Throughput out;
    out.charge = total;
    out.throatCells = static_cast<int>(s.dstCols.size());

    int released = 0;
    int idle = 0;
    const int kMaxTicks = 200000;
    int q1Tick = -1, q3Tick = -1, q1Rel = 0, q3Rel = 0;
    int t = 0;
    for (; t < kMaxTicks; ++t) {
        int v = 0;
        const int moves = s.tick(cap, &v);
        released += v;
        if (v > out.peak) out.peak = v;
        if (q1Tick < 0 && released >= total / 4) { q1Tick = t; q1Rel = released; }
        if (q3Tick < 0 && released >= (3 * total) / 4) { q3Tick = t; q3Rel = released; }
        if (released >= total) break;
        idle = (moves == 0) ? idle + 1 : 0;
        if (idle > 100) { out.stalled = true; break; }
    }

    out.ticks = t + 1;
    out.drained = released;
    if (q1Tick >= 0 && q3Tick > q1Tick) {
        out.sustained = static_cast<double>(q3Rel - q1Rel) / static_cast<double>(q3Tick - q1Tick);
    }
    return out;
}

/// Free-standing pile in an open box: the textbook way to read an angle of
/// repose, with no vessel wall to pin the surface.
Repose freePileRepose(int reach, int rollPct, uint32_t seed) {
    Sim s(1, Vessel::Box, seed);
    s.attractPct = 0; // box mode has no plug, so this is already inert
    s.reach = reach;
    s.rollPct = rollPct;
    const int src = s.at(1, s.centreCol);
    for (int i = 0; i < 12000; ++i) {
        if (!s.sand[static_cast<size_t>(src)]) s.sand[static_cast<size_t>(src)] = 1;
        int v = 0;
        s.tick(0, &v);
    }
    s.relax(50000, 0);
    return measureRepose(s, 0, s.rows);
}

struct RunStats {
    int grain = 0;
    int cols = 0, rows = 0;
    int upperOpen = 0, lowerOpen = 0;
    int charge = 0;
    bool massOk = true;
    int massMin = 0, massMax = 0;
    int strandedWith = 0;
    int strandedWithout = 0;
    int upperAtDeadline = 0; ///< still in the upper bulb when the clock runs out
    int lowerBackedUp = 0;
    double msPerTick = 0.0;
    long long cellsPerTick = 0;
    Repose finalRepose; ///< the packed end state; usually has no free surface
    Repose midRepose;   ///< half drained, relaxed, valve shut -- the usable one
    int midRelaxTicks = 0;
    bool relaxConverged = false;
    int relaxTicks = 0;
};

/// Drain the glass over `seconds` and dump a frame at each of the fill fractions
/// the shaped face is rendered at. Returns everything measured along the way.
RunStats runDrain(int grain, int seconds, int reach, int rollPct, int attractPct,
                  uint32_t seed, const std::string& dir, bool dump, bool dither) {
    RunStats st;
    st.grain = grain;

    Sim s(grain, Vessel::Glass, seed);
    s.attractPct = attractPct;
    s.reach = reach;
    s.rollPct = rollPct;

    st.cols = s.cols;
    st.rows = s.rows;
    st.upperOpen = s.countOpenRows(0, s.plugRow);
    st.lowerOpen = s.countOpenRows(s.plugRow + 1, s.rows);
    st.cellsPerTick = static_cast<long long>(s.cols) * s.rows;

    // The two bulbs are NOT the same size, so the charge is limited by whichever
    // is smaller. Overfilling would back the pile up into the throat and stall
    // the timer short of zero.
    st.charge = (st.upperOpen < st.lowerOpen) ? st.upperOpen : st.lowerOpen;
    chargeUpper(s, st.charge);

    const int total = st.charge;
    const int totalTicks = seconds * TICKS_PER_SEC;
    const int cap = static_cast<int>(s.dstCols.size());
    st.massMin = st.massMax = total;

    Panel fb;
    int released = 0;
    size_t next = 0;
    const auto t0 = std::chrono::steady_clock::now();

    std::printf("  frame  tick    grains   upper   lower   released\n");
    for (int t = 0; t <= totalTicks; ++t) {
        while (next < sizeof(kLevels) / sizeof(kLevels[0])) {
            const int want = static_cast<int>(std::llround(totalTicks * (1.0 - kLevels[next].f)));
            if (t < want) break;
            const int mass = s.countSand();
            if (mass < st.massMin) st.massMin = mass;
            if (mass > st.massMax) st.massMax = mass;
            if (mass != total) st.massOk = false;
            const int up = s.countSandRows(0, s.plugRow);
            const int lo = s.countSandRows(s.plugRow + 1, s.rows);
            std::printf("   %s  %6d  %6d  %6d  %6d  %8d%s\n", kLevels[next].tag, t, mass, up, lo,
                        released, (mass == total) ? "" : "   <-- MASS DRIFT");
            if (dump) {
                renderSim(s, fb, dither);
                const std::string p =
                    framePath(dir, "sandca-" + std::to_string(grain) + "px-" + kLevels[next].tag);
                if (!writePbm(fb, p)) std::printf("   FAILED to write %s\n", p.c_str());
            }
            // Half drained is the only point where the lower pile has a free
            // surface on both flanks: by the end it has packed against the glass
            // and there is no angle left to measure. Relaxed on a COPY with the
            // valve shut, so the live run is not perturbed and the fit is not
            // taken on a pile that is still being rained on.
            if (std::strcmp(kLevels[next].tag, "050") == 0) {
                Sim probe = s;
                st.midRelaxTicks = probe.relax(200000, 0);
                st.midRepose = measureRepose(probe, probe.plugRow + 1, probe.rows);
            }
            ++next;
        }
        if (t == totalTicks) break;

        // Deadbeat: schedule a POSITION, not a rate, so a stalled tick is made
        // up rather than lost.
        int want = static_cast<int>(std::llround(static_cast<double>(total) * (t + 1) / totalTicks)) - released;
        if (want < 0) want = 0;
        if (want > cap) want = cap;
        int v = 0;
        s.tick(want, &v);
        released += v;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    st.msPerTick = (totalTicks > 0) ? ms / totalTicks : 0.0;

    // Sampled BEFORE relaxation: relaxing runs the valve on past the deadline,
    // which would drain a merely-late timer and make it indistinguishable from
    // one that finished on time.
    st.upperAtDeadline = s.countSandRows(0, s.plugRow);

    // Let it settle completely with the valve still wide open. Whatever is left
    // in the upper bulb after that is genuinely stranded -- it never reached the
    // throat and never will.
    st.relaxTicks = s.relax(200000, cap);
    st.relaxConverged = (st.relaxTicks < 200000);
    st.strandedWith = s.countSandRows(0, s.plugRow);
    st.lowerBackedUp = (s.countSandRows(s.plugRow + 1, s.plugRow + 2) ==
                        static_cast<int>(s.dstCols.size()))
                           ? 1
                           : 0;
    if (s.countSand() != total) st.massOk = false;

    st.finalRepose = measureRepose(s, s.plugRow + 1, s.rows);

    if (dump) {
        renderSim(s, fb, dither);
        writePbm(fb, framePath(dir, "sandca-" + std::to_string(grain) + "px-relaxed"));
    }

    // The same run with the attractor disabled, to put a number on how much the
    // funnel wall actually holds back on THIS geometry.
    {
        Sim n(grain, Vessel::Glass, seed);
        n.attractPct = 0;
        n.reach = reach;
        n.rollPct = rollPct;
        chargeUpper(n, total);
        int rel = 0;
        for (int t = 0; t < totalTicks; ++t) {
            int want = static_cast<int>(std::llround(static_cast<double>(total) * (t + 1) / totalTicks)) - rel;
            if (want < 0) want = 0;
            if (want > cap) want = cap;
            int v = 0;
            n.tick(want, &v);
            rel += v;
        }
        n.relax(200000, cap);
        st.strandedWithout = n.countSandRows(0, n.plugRow);
        if (dump) {
            renderSim(n, fb, dither);
            writePbm(fb, framePath(dir, "sandca-" + std::to_string(grain) + "px-noattractor"));
        }
    }

    return st;
}

void writeShapedReference(const std::string& dir) {
    Panel fb;
    for (const Level& l : kLevels) {
        h0::HourglassFace::renderAt(fb, static_cast<float>(l.f), true, 0);
        writePbm(fb, framePath(dir, std::string("shaped-") + l.tag));
    }
    std::printf("wrote shaped-{100,085,070,050,030,015,000}.pbm for side-by-side comparison\n");
}

int parseInt(const char* v, int fallback) {
    if (!v) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v) return fallback;
    return static_cast<int>(n);
}

} // namespace

int main(int argc, char** argv) {
    std::string dir = ".";
    int grain = 0; // 0 = run all three candidates
    int seconds = 60;
    int reach = 0;
    int rollPct = 50;
    int attractPct = 10;
    uint32_t seed = 20250730u;
    bool dither = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (a == "--grain") { grain = parseInt(v, grain); ++i; }
        else if (a == "--seconds") { seconds = parseInt(v, seconds); ++i; }
        else if (a == "--reach") { reach = parseInt(v, reach); ++i; }
        else if (a == "--roll") { rollPct = parseInt(v, rollPct); ++i; }
        else if (a == "--attract") { attractPct = parseInt(v, attractPct); ++i; }
        else if (a == "--seed") { seed = static_cast<uint32_t>(parseInt(v, 0)); ++i; }
        else if (a == "--dither") { dither = true; }
        else if (!a.empty() && a[0] != '-') dir = a;
        else {
            std::printf("usage: sand_proto <outdir> [--grain N] [--seconds T] [--reach R]"
                        " [--roll P] [--attract Q] [--seed S] [--dither]\n");
            return 2;
        }
    }
    if (seconds < 1) seconds = 1;

    std::printf("falling-sand prototype -- vessel CX=%d TOP=%d NECK=%d BOT=%d HW_MAX=%d HW_NECK=%d\n",
                CX, TOP, NECK, BOT, HW_MAX, HW_NECK);
    std::printf("tick rate %d Hz, duration %d s (%d ticks), seed %u, reach %d, roll %d%%, attractor %d%%, "
                "grains drawn %s\n\n",
                TICKS_PER_SEC, seconds, seconds * TICKS_PER_SEC, seed, reach, rollPct, attractPct,
                dither ? "dithered (shape comparison)" : "solid");

    writeShapedReference(dir);

    const int grains[3] = {2, 3, 4};
    std::vector<RunStats> stats;
    std::vector<Throughput> tputs;

    for (int gi = 0; gi < 3; ++gi) {
        const int g = grains[gi];
        if (grain != 0 && g != grain) continue;
        std::printf("\n================ grain %d px ================\n", g);
        const RunStats st = runDrain(g, seconds, reach, rollPct, attractPct, seed, dir, true, dither);
        const Throughput tp = measureThroughput(g, seed, attractPct, reach, rollPct);
        stats.push_back(st);
        tputs.push_back(tp);

        std::printf("\n  grid                    %d x %d cells (%lld cells/tick scanned)\n", st.cols,
                    st.rows, st.cellsPerTick);
        std::printf("  open cells              upper %d, lower %d\n", st.upperOpen, st.lowerOpen);
        std::printf("  charge N                %d grains  (%.1f%% of upper bulb)\n", st.charge,
                    100.0 * st.charge / (st.upperOpen ? st.upperOpen : 1));
        std::printf("  mass conservation       %s (min %d, max %d, expected %d)\n",
                    st.massOk ? "EXACT at every dump point" : "*** DRIFT ***", st.massMin,
                    st.massMax, st.charge);
        std::printf("  schedule kept           %s -- %d grains still up top at t=T\n",
                    st.upperAtDeadline == 0 ? "yes, bulb empty on the tick" : "*** NO, TIMER RAN LATE ***",
                    st.upperAtDeadline);
        std::printf("  stranded upper, q=%2d%%   %d grains (%.1f%%)\n", attractPct, st.strandedWith,
                    100.0 * st.strandedWith / (st.charge ? st.charge : 1));
        std::printf("  stranded upper, q= 0%%   %d grains (%.1f%%)\n", st.strandedWithout,
                    100.0 * st.strandedWithout / (st.charge ? st.charge : 1));
        std::printf("  relaxation              %s after %d extra ticks\n",
                    st.relaxConverged ? "converged" : "DID NOT CONVERGE", st.relaxTicks);
        if (st.midRepose.ok) {
            std::printf("  repose, half drained    %.1f deg (left %.1f / right %.1f, %d+%d pts, "
                        "relaxed %d ticks)\n",
                        st.midRepose.meanDeg, st.midRepose.leftDeg, st.midRepose.rightDeg,
                        st.midRepose.nLeft, st.midRepose.nRight, st.midRelaxTicks);
        } else {
            std::printf("  repose, half drained    not measurable (%d+%d free pts)\n",
                        st.midRepose.nLeft, st.midRepose.nRight);
        }
        if (st.finalRepose.ok) {
            std::printf("  repose, final pile      %.1f deg (left %.1f / right %.1f)\n",
                        st.finalRepose.meanDeg, st.finalRepose.leftDeg, st.finalRepose.rightDeg);
        } else {
            std::printf("  repose, final pile      no free surface -- the lower bulb ends packed "
                        "to the glass (%d+%d free pts)\n",
                        st.finalRepose.nLeft, st.finalRepose.nRight);
        }
        std::printf("  throughput ceiling      %.2f grains/tick sustained, peak %d\n", tp.sustained,
                    tp.peak);
        std::printf("  throat below the plug   %d open cells -- the hard bound the ceiling sits "
                    "against\n",
                    tp.throatCells);
        std::printf("  wide-open drain         %d of %d grains in %d ticks%s\n", tp.drained,
                    tp.charge, tp.ticks, tp.stalled ? "  <-- STALLED" : "");
        std::printf("  cost                    %.4f ms/tick host, %.1f ns/cell\n", st.msPerTick,
                    st.cellsPerTick ? st.msPerTick * 1e6 / static_cast<double>(st.cellsPerTick) : 0.0);
        const double demand = static_cast<double>(st.charge) / (seconds * TICKS_PER_SEC);
        std::printf("  demand at %d s           %.3f grains/tick -> %s\n", seconds, demand,
                    (tp.sustained > 0.0 && demand < tp.sustained) ? "within ceiling"
                                                                  : "OVER THE CEILING");
    }

    // Does the lateral-roll knob actually buy a realistic angle? Measured on a
    // free-standing pile so no wall can pin the surface.
    std::printf("\n================ angle of repose vs lateral reach ================\n");
    std::printf("  (free-standing pile, 12000 grains, fully relaxed; dry sand is ~34 deg)\n");
    for (int r = 0; r <= 4; ++r) {
        const Repose rr = freePileRepose(r, (r == 0) ? 0 : rollPct, seed);
        if (rr.ok) {
            std::printf("  reach %d, p=%3d%%   %.1f deg   (left %.1f / right %.1f)\n", r,
                        (r == 0) ? 0 : rollPct, rr.meanDeg, rr.leftDeg, rr.rightDeg);
        } else {
            std::printf("  reach %d, p=%3d%%   not measurable\n", r, (r == 0) ? 0 : rollPct);
        }
    }

    return 0;
}
