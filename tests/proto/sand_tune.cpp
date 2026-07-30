// Throwaway measuring instrument: what constants does the falling-sand
// hourglass need in order to hit real timer durations?
//
// The vessel here is the one the face actually uses -- a border plus a single
// horizontal floor across the middle with a hole in it. No taper, no funnel,
// full-width chambers. That matters: an earlier prototype measured a TAPERED
// glass and concluded that no aperture width has a usable operating point. A
// flat floor is a different problem (the sand does not creep down a wall toward
// the orifice) and the earlier numbers are not assumed to transfer -- every one
// of them is re-measured here.
//
// Nothing in firmware/src is modified. The centreline attractor is implemented
// in this file by poking the public sand grid between ticks, so the shipped
// simulation stays exactly as it is while the question of whether an attractor
// is needed at all is still open.
//
//   sand_tune [--only a|b|c|d|e|f|g|h|i|j] [--seed S]
//
// Sections, in the order they run:
//   a  free-flow rate versus hole half-width
//   b  residual grains after full settling (does the upper chamber empty?)
//   c  centreline attractor sweep, and the attractor's own throughput
//   d  angle of repose, measured after FULL relaxation
//   e  drain time versus fill depth and hole width
//   h  heaped charge instead of a flat slab
//   f  grain budget for 30 s / 2 min / 10 min, with a metered gate
//   g  cost per tick, moving versus settled
//   i  flip repeatability
//   j  surface profiles, printed rather than fitted
//
// Section j is the check on the other nine. Every angle reported anywhere here
// comes from a least-squares fit, and a fit is only worth the profile under it;
// j prints the profile so a surprising angle can be judged rather than trusted.

#include "sand/sand_sim.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using h0::Gravity;
using h0::SandGrid;
using h0::SandSim;

// ---------------------------------------------------------------- geometry --

constexpr int W = SandGrid::W; // 104
constexpr int H = SandGrid::H; // 124

constexpr int FLOOR_Y = H / 2; // 62, the dividing floor
constexpr int HOLE_X = W / 2;  // 52, centre of the hole

constexpr int UPPER_TOP = 1;
constexpr int UPPER_BOT = FLOOR_Y - 1; // 61
constexpr int LOWER_TOP = FLOOR_Y + 1; // 63
constexpr int LOWER_BOT = H - 2;       // 122

constexpr int INNER_W = W - 2;                          // 102 usable columns
constexpr int UPPER_ROWS = UPPER_BOT - UPPER_TOP + 1;   // 61
constexpr int LOWER_ROWS = LOWER_BOT - LOWER_TOP + 1;   // 60
constexpr int LOWER_CAP = INNER_W * LOWER_ROWS;         // 6120 grains

/// Border, plus the floor. `holed` false gives the same vessel with the hole
/// bricked up, which is how the metered gate is shut.
SandGrid makeWalls(int hw, bool holed) {
    SandGrid g;
    for (int x = 0; x < W; ++x) {
        g.set(x, 0, true);
        g.set(x, H - 1, true);
        const bool inHole = holed && (x >= HOLE_X - hw) && (x <= HOLE_X + hw);
        if (!inHole) g.set(x, FLOOR_Y, true);
    }
    for (int y = 0; y < H; ++y) {
        g.set(0, y, true);
        g.set(W - 1, y, true);
    }
    return g;
}

// ------------------------------------------------------------------- prng --

/// Independent of the simulation's own generator, so the attractor's coin flips
/// never correlate with the per-row handedness inside step().
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    /// `perMille` is probability x1000, so q=0.02 is 20.
    bool chance(int perMille) {
        return perMille > 0 && static_cast<int>(next() % 1000u) < perMille;
    }
};

// -------------------------------------------------------------- the trial --

/// One vessel, one charge, one run. Owns the mass check: grain count is
/// verified here on every tick, independently of anything the library's own
/// tests assert, because a measurement taken on a leaking simulation is worse
/// than no measurement.
class Trial {
public:
    Trial(int hw, uint32_t seed) : rng_(seed ^ 0x5BD1u) {
        open_ = makeWalls(hw, true);
        shut_ = makeWalls(hw, false);
        sim_.setWalls(open_);
        sim_.seed(seed ? seed : 1u);
    }

    /// Gravity, and with it which chamber is upstream. Flipping the device is
    /// exactly this: the walls do not move, the direction of fall does.
    void setGravity(Gravity g) { g_ = g; }
    bool down() const { return g_ == Gravity::S; }
    int srcTop() const { return down() ? UPPER_TOP : LOWER_TOP; }
    int srcBot() const { return down() ? UPPER_BOT : LOWER_BOT; }

    /// Grains still in the upstream chamber, and grains that have crossed.
    int source() const {
        return down() ? sim_.sand().countRows(0, FLOOR_Y - 1)
                      : sim_.sand().countRows(FLOOR_Y + 1, H - 1);
    }
    int dest() const {
        return down() ? sim_.sand().countRows(FLOOR_Y + 1, H - 1)
                      : sim_.sand().countRows(0, FLOOR_Y - 1);
    }

    /// Fill `depth` rows of the upper chamber, packed against the floor: a flat
    /// full-width slab, which is what "the hourglass is full" naively means.
    int charge(int depth) {
        int n = 0;
        for (int y = UPPER_BOT; y > UPPER_BOT - depth && y >= UPPER_TOP; --y) {
            for (int x = 1; x <= W - 2; ++x) {
                if (sim_.walls().get(x, y)) continue;
                sim_.sand().set(x, y, true);
                ++n;
            }
        }
        expect_ = n;
        return n;
    }

    /// Fill the upper chamber as a heap of height `h` centred on the hole, with
    /// faces at 45 degrees -- the shape sand poured onto a flat floor actually
    /// takes, and the shape a drained lower chamber presents when the device is
    /// turned over. Clipped by the side walls, so h may exceed the half-width.
    int chargeCone(int h) {
        int n = 0;
        for (int y = UPPER_BOT; y >= UPPER_TOP; --y) {
            const int up = FLOOR_Y - y; // 1 at the floor
            for (int x = 1; x <= W - 2; ++x) {
                if (sim_.walls().get(x, y)) continue;
                if (std::abs(x - HOLE_X) + up > h) continue;
                sim_.sand().set(x, y, true);
                ++n;
            }
        }
        expect_ = n;
        return n;
    }

    /// Gate control for the metered runs: shutting bricks up EVERY hole cell.
    ///
    /// The tempting refinement -- wall only the hole cells that are empty right
    /// now, so no grain is ever caught inside a wall -- does not close the gate
    /// at all. The scan runs bottom-up, so within a single tick the grain in the
    /// hole falls out, the cell it vacated is still unwalled, and the grain
    /// above drops straight into it. The hole passes its full width every tick
    /// while reporting itself shut, and a ten-minute timer empties in three
    /// seconds. Walling the lot leaves at most one row of grains momentarily
    /// inside the wall; they fall clear on the next tick, because a wall blocks
    /// what tries to ENTER a cell, not what is already in it.
    void setGate(bool wantOpen) {
        if (wantOpen == gateOpen_) return;
        sim_.setWalls(wantOpen ? open_ : shut_);
        gateOpen_ = wantOpen;
    }

    /// Attractor, then gravity. Returns total moves; `gravOut` receives the
    /// gravity moves alone, which is the half that decides "settled".
    int tick(int qPerMille, int* gravOut) {
        const int a = attractor(qPerMille);
        const int g = sim_.step(g_);
        if (gravOut) *gravOut = g;
        if (checkMass_) {
            const int now = sim_.sand().count();
            if (now != expect_) {
                if (massOk_) {
                    std::printf("\n*** MASS DRIFT: expected %d grains, found %d ***\n", expect_,
                                now);
                }
                massOk_ = false;
                expect_ = now;
            }
        }
        return a + g;
    }

    /// True settlement test, not a tick budget. Gravity has stopped AND no
    /// grain anywhere has a legal attractor target, so nothing can move however
    /// long the run continues. Sampling at a fixed iteration count instead is
    /// exactly the mistake that puts a wrong angle of repose in print.
    bool atRest(int gravMoves, int qPerMille) const {
        if (gravMoves != 0) return false;
        if (qPerMille <= 0) return true;
        return candidates() == 0;
    }

    int candidates() const {
        int n = 0;
        for (int y = srcTop(); y <= srcBot(); ++y) {
            for (int x = 1; x <= W - 2; ++x) {
                if (x == HOLE_X) continue;
                if (!sim_.sand().get(x, y)) continue;
                if (!resting(x, y)) continue;
                const int nx = (x < HOLE_X) ? x + 1 : x - 1;
                if (!solid(nx, y)) ++n;
            }
        }
        return n;
    }

    int upper() const { return sim_.sand().countRows(0, FLOOR_Y - 1); }
    int lower() const { return sim_.sand().countRows(FLOOR_Y + 1, H - 1); }

    /// Signed left/right mass imbalance in the upstream chamber, from -1 (all
    /// left) to +1 (all right). A fitted surface angle is noisy mid-avalanche;
    /// this is not, so it is the honest test of whether the attractor treats the
    /// two flanks alike.
    double imbalance() const {
        int l = 0, r = 0;
        for (int y = srcTop(); y <= srcBot(); ++y) {
            for (int x = 1; x < HOLE_X; ++x)
                if (sim_.sand().get(x, y)) ++l;
            for (int x = HOLE_X + 1; x <= W - 2; ++x)
                if (sim_.sand().get(x, y)) ++r;
        }
        const int n = l + r;
        return n ? static_cast<double>(r - l) / n : 0.0;
    }
    int total() const { return sim_.sand().count(); }

    bool massOk() const { return massOk_; }
    void setMassCheck(bool on) { checkMass_ = on; }
    int expected() const { return expect_; }

    const SandGrid& sand() const { return sim_.sand(); }
    const SandGrid& walls() const { return sim_.walls(); }
    SandSim& sim() { return sim_; }

private:
    bool solid(int x, int y) const { return sim_.walls().get(x, y) || sim_.sand().get(x, y); }

    /// A grain that cannot fall this tick. Only these are nudged, so the
    /// attractor never fights a grain that is already in free fall.
    bool resting(int x, int y) const {
        const int dy = down() ? 1 : -1;
        return solid(x, y + dy) && solid(x - 1, y + dy) && solid(x + 1, y + dy);
    }

    /// Centreline attractor, upstream chamber only.
    ///
    /// Each half of a row is walked from the centre OUTWARDS and grains are
    /// nudged inwards, i.e. always into a cell already visited this pass. That
    /// ordering is what stops a nudged grain from being picked up again and
    /// skating several cells in a single tick.
    ///
    /// Which half goes first is a coin flip PER ROW. The two halves are not
    /// independent -- both write into the hole's own column -- so a fixed order
    /// hands the same side first claim on that column on every tick of a run
    /// thousands of ticks long. Measured, that compounds into one flank being
    /// scraped bare while the other still stands at full height. Per-row
    /// randomness is the same remedy the simulation already applies to its
    /// choice of diagonal, and for the same reason.
    int attractor(int qPerMille) {
        if (qPerMille <= 0) return 0;
        int moves = 0;
        SandGrid& s = sim_.sand();
        auto inwardHalf = [&](int y, int dir) {
            // dir -1 walks the left half outward, nudging right; +1 mirrors it.
            const int step = -dir;
            for (int x = HOLE_X + dir; x >= 1 && x <= W - 2; x += dir) {
                if (!s.get(x, y)) continue;
                if (!resting(x, y)) continue;
                if (solid(x + step, y)) continue;
                if (!rng_.chance(qPerMille)) continue;
                s.set(x, y, false);
                s.set(x + step, y, true);
                ++moves;
            }
        };
        for (int y = srcBot(); y >= srcTop(); --y) {
            const bool leftFirst = (rng_.next() & 1u) != 0;
            inwardHalf(y, leftFirst ? -1 : 1);
            inwardHalf(y, leftFirst ? 1 : -1);
        }
        return moves;
    }

    SandSim sim_;
    SandGrid open_;
    SandGrid shut_;
    Gravity g_ = Gravity::S;
    Rng rng_;
    bool gateOpen_ = true;
    bool checkMass_ = true;
    bool massOk_ = true;
    int expect_ = 0;
};

// ------------------------------------------------------------ surface fits --

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

constexpr double kDeg = 180.0 / 3.14159265358979323846;

/// Highest occupied row per column inside [y0, y1]; -1 for an empty column.
std::vector<int> surface(const SandGrid& sand, int y0, int y1) {
    std::vector<int> top(static_cast<size_t>(W), -1);
    for (int x = 1; x <= W - 2; ++x) {
        for (int y = y0; y <= y1; ++y) {
            if (sand.get(x, y)) {
                top[static_cast<size_t>(x)] = y;
                break;
            }
        }
    }
    return top;
}

struct Repose {
    bool ok = false;
    double leftDeg = 0, rightDeg = 0, meanDeg = 0;
    int nl = 0, nr = 0;
    int apexHeight = 0; ///< rows from the floor of the chamber up to the peak
};

/// Angle of repose of a free-standing heap. Columns whose surface cell sits
/// against a side wall are dropped: there the surface is pinned by the vessel,
/// not by the material, and including them drags the fit toward the wall.
Repose pileRepose(const SandGrid& sand, int y0, int y1) {
    Repose out;
    const std::vector<int> top = surface(sand, y0, y1);
    int apex = -1;
    for (int x = 1; x <= W - 2; ++x) {
        const int t = top[static_cast<size_t>(x)];
        if (t < 0) continue;
        if (apex < 0 || t < top[static_cast<size_t>(apex)]) apex = x;
    }
    if (apex < 0) return out;
    out.apexHeight = y1 - top[static_cast<size_t>(apex)] + 1;

    auto flank = [&](int dir, std::vector<double>& xs, std::vector<double>& ys) {
        for (int x = apex + dir; x >= 1 && x <= W - 2; x += dir) {
            const int t = top[static_cast<size_t>(x)];
            if (t < 0) break;
            const int outward = x + dir;
            if (outward < 1 || outward > W - 2) break;
            xs.push_back(static_cast<double>(std::abs(x - apex)));
            ys.push_back(static_cast<double>(t));
        }
    };

    std::vector<double> lx, ly, rx, ry;
    flank(-1, lx, ly);
    flank(+1, rx, ry);
    out.nl = static_cast<int>(lx.size());
    out.nr = static_cast<int>(rx.size());
    if (out.nl < 4 || out.nr < 4) return out;
    out.leftDeg = std::atan(std::fabs(fitSlope(lx, ly))) * kDeg;
    out.rightDeg = std::atan(std::fabs(fitSlope(rx, ry))) * kDeg;
    out.meanDeg = 0.5 * (out.leftDeg + out.rightDeg);
    out.ok = true;
    return out;
}

/// Wall angle of the drainage crater in the upper chamber: the free surface
/// walked outward from the hole, up to the point where it reaches the highest
/// surface in the chamber (the level of the undisturbed lake). This is the
/// number that says whether the remaining sand still reads as granular material
/// standing at its own angle, or as a puddle with a shaft punched through it.
///
/// Columns that have drained clean are skipped rather than treated as the end
/// of the flank: near a wide hole the first few columns are empty and stopping
/// there measures nothing.
struct Crater {
    bool ok = false;
    double leftDeg = -1, rightDeg = -1, meanDeg = -1;
    /// How lopsided the two flanks are. A centreline attractor that favours one
    /// side shows up here long before it is obvious on screen.
    double skewDeg = 0;
};

Crater craterAngle(const SandGrid& sand) {
    const std::vector<int> top = surface(sand, UPPER_TOP, UPPER_BOT);
    Crater out;
    double got[2] = {-1, -1};
    int side = 0;
    for (int dir = -1; dir <= 1; dir += 2, ++side) {
        std::vector<double> xs, ys;
        int ridge = H; // highest surface seen so far on this flank
        for (int d = 0; d <= 51; ++d) {
            const int x = HOLE_X + dir * d;
            if (x < 1 || x > W - 2) break;
            const int t = top[static_cast<size_t>(x)];
            if (t < 0) continue; // drained clean, keep walking outward
            // Stop at the ridge. Walking past it picks up the far side of the
            // original heap, which descends, and averaging an ascent with a
            // descent reports a shallow angle for a surface that is nowhere
            // shallow. One cell of slack absorbs single-column roughness.
            if (t > ridge + 1) break;
            if (t < ridge) ridge = t;
            xs.push_back(static_cast<double>(d));
            ys.push_back(static_cast<double>(t));
        }
        if (xs.size() >= 4) got[side] = std::atan(std::fabs(fitSlope(xs, ys))) * kDeg;
    }
    if (got[0] < 0 || got[1] < 0) return out;
    out.leftDeg = got[0];
    out.rightDeg = got[1];
    out.meanDeg = 0.5 * (got[0] + got[1]);
    out.skewDeg = std::fabs(got[0] - got[1]);
    out.ok = true;
    return out;
}

double craterDeg(const SandGrid& sand) {
    const Crater c = craterAngle(sand);
    return c.ok ? c.meanDeg : -1.0;
}

// ------------------------------------------------------------- run helpers --

struct Drain {
    int charge = 0;
    int delivered = 0;
    int residual = 0;
    long settleTicks = 0;
    bool converged = false;
    bool massOk = true;
    double sustained = 0; ///< grains/tick while the chamber is still full
    int peak = 0;
    double best100 = 0;
    long ticksTo50 = -1, ticksTo90 = -1, ticksTo99 = -1;
    double craterFlowing = -1; ///< upper free surface while sand is moving
    double craterSettled = -1; ///< same surface once everything has stopped
    double maxImbalance = 0;   ///< worst left/right mass split during the drain
    Repose lowerPile;
};

/// Charge, open the hole, and let it go until absolutely nothing can move.
/// `cone` charges a 45-degree heap centred on the hole instead of a flat slab.
Drain drainToRest(int hw, int depth, int qPerMille, uint32_t seed, long maxTicks,
                  bool cone = false) {
    Trial t(hw, seed);
    Drain d;
    d.charge = cone ? t.chargeCone(depth) : t.charge(depth);

    std::vector<int> perTick;
    perTick.reserve(4096);
    double craterSum = 0;
    int craterN = 0;
    int nextProbe = 0;
    const double probes[3] = {0.25, 0.50, 0.75};
    int prevLower = 0;
    long tick = 0;
    for (; tick < maxTicks; ++tick) {
        int grav = 0;
        t.tick(qPerMille, &grav);
        const int lo = t.lower();
        const int gained = lo - prevLower;
        prevLower = lo;
        perTick.push_back(gained);
        if (gained > d.peak) d.peak = gained;
        if (d.ticksTo50 < 0 && lo * 2 >= d.charge) d.ticksTo50 = tick + 1;
        if (d.ticksTo90 < 0 && lo * 10 >= d.charge * 9) d.ticksTo90 = tick + 1;
        if (d.ticksTo99 < 0 && lo * 100 >= d.charge * 99) d.ticksTo99 = tick + 1;
        // The crater angle is sampled at three points through the drain and
        // averaged: a single snapshot catches whatever avalanche happens to be
        // in flight and swings by tens of degrees.
        // Sampled sparsely: the count is a full sweep of the chamber and doing
        // it every tick would dominate the run.
        // Only while the bulk is still in the chamber. In the last few percent
        // a handful of grains is left and they are trivially all on one side,
        // which says nothing about how the flow looked.
        if ((tick & 63) == 0 && lo * 2 < d.charge) {
            const double im = std::fabs(t.imbalance());
            if (im > d.maxImbalance) d.maxImbalance = im;
        }
        while (nextProbe < 3 && lo >= probes[nextProbe] * d.charge) {
            const double c = craterDeg(t.sand());
            if (c >= 0) {
                craterSum += c;
                ++craterN;
            }
            ++nextProbe;
        }
        if (t.atRest(grav, qPerMille)) {
            d.converged = true;
            ++tick;
            break;
        }
    }
    d.settleTicks = tick;
    d.delivered = t.lower();
    d.residual = t.upper();
    d.massOk = t.massOk();
    d.craterFlowing = craterN ? craterSum / craterN : -1.0;
    d.craterSettled = craterDeg(t.sand());
    d.lowerPile = pileRepose(t.sand(), LOWER_TOP, LOWER_BOT);

    // Sustained rate: measured only while the chamber is still at least 80%
    // charged, because on a flat floor the rate decays as the crater eats
    // outward and an average over the whole run would understate the ceiling.
    long from = 20, to = 0;
    int run = 0;
    for (size_t i = 0; i < perTick.size(); ++i) {
        run += perTick[i];
        if (run * 5 > d.charge) break; // 20% delivered
        to = static_cast<long>(i) + 1;
    }
    if (to > from) {
        int sum = 0;
        for (long i = from; i < to; ++i) sum += perTick[static_cast<size_t>(i)];
        d.sustained = static_cast<double>(sum) / static_cast<double>(to - from);
    } else if (!perTick.empty()) {
        int sum = 0;
        for (int v : perTick) sum += v;
        d.sustained = static_cast<double>(sum) / static_cast<double>(perTick.size());
    }
    // Best 100-tick window, as an upper bound that no averaging choice can hide.
    if (perTick.size() >= 100) {
        int win = 0;
        for (size_t i = 0; i < 100; ++i) win += perTick[i];
        int best = win;
        for (size_t i = 100; i < perTick.size(); ++i) {
            win += perTick[i] - perTick[i - 100];
            if (win > best) best = win;
        }
        d.best100 = best / 100.0;
    }
    return d;
}

/// Formats into a caller-supplied buffer. A shared static buffer looks tidier
/// and is wrong: two calls in one printf argument list both read back the value
/// written by whichever ran last, and the 30 Hz and 60 Hz columns print the
/// same number.
struct Secs {
    char buf[16];
    Secs(double ticks, int hz) { std::snprintf(buf, sizeof(buf), "%.1f", ticks / hz); }
    const char* c() const { return buf; }
};

// ------------------------------------------------------------ experiment a --

void sectionA(uint32_t seed) {
    std::printf("\n=== A. free-flow rate versus hole half-width ===\n");
    std::printf("full upper chamber (depth 60, %d grains), hole wide open, no attractor\n",
                60 * INNER_W);
    std::printf("  hw  hole   grains/tick   best100   peak   delivered   residual  mass\n");
    for (int hw = 1; hw <= 10; ++hw) {
        const Drain d = drainToRest(hw, 60, 0, seed, 200000);
        std::printf("  %2d  %2d px  %10.2f  %8.2f  %5d  %9d  %9d  %s\n", hw, 2 * hw + 1,
                    d.sustained, d.best100, d.peak, d.delivered, d.residual,
                    d.massOk ? "ok" : "DRIFT");
        std::fflush(stdout);
    }
}

// ------------------------------------------------------------ experiment b --

void sectionB(uint32_t seed) {
    std::printf("\n=== B. does the upper chamber empty? residual after FULL settling ===\n");
    std::printf("no attractor. 'forced' is the part geometry alone makes impossible\n");
    std::printf("(charge above the %d-grain capacity of the lower chamber).\n", LOWER_CAP);
    std::printf("  depth  charge   hw  residual    %%   forced  settleTicks  conv  mass\n");
    const int depths[] = {10, 20, 40, 60};
    const int hws[] = {1, 2, 3, 4, 5, 6, 8, 10};
    for (int depth : depths) {
        for (int hw : hws) {
            const Drain d = drainToRest(hw, depth, 0, seed, 200000);
            const int forced = (d.charge > LOWER_CAP) ? d.charge - LOWER_CAP : 0;
            std::printf("  %5d  %6d  %3d  %8d  %5.1f  %6d  %11ld  %4s  %s\n", depth, d.charge, hw,
                        d.residual, 100.0 * d.residual / d.charge, forced, d.settleTicks,
                        d.converged ? "yes" : "NO", d.massOk ? "ok" : "DRIFT");
            std::fflush(stdout);
        }
    }
}

// ------------------------------------------------------------ experiment c --

void sectionC(uint32_t seed) {
    std::printf("\n=== C. centreline attractor sweep ===\n");
    std::printf("hole half-width 3. 'flowing' is the upper free surface averaged over three\n");
    std::printf("points through the drain; 'settled' is the same surface once everything has\n");
    std::printf("stopped; 'pile' is the lower heap after full relaxation.\n");
    std::printf("'skew' is the worst left/right mass split seen during the drain: 0.00 is\n");
    std::printf("even, 1.00 is everything on one side.\n");
    std::printf("  depth  charge     q  residual    %%  flowing settled   pile  skew  settleTicks  "
                "mass\n");
    const int depths[] = {20, 40, 60};
    const int qs[] = {0, 20, 50, 100, 250, 500, 1000};
    for (int depth : depths) {
        for (int q : qs) {
            const Drain d = drainToRest(3, depth, q, seed, 400000);
            char fl[16], se[16], pile[16];
            if (d.craterFlowing >= 0) std::snprintf(fl, sizeof(fl), "%6.1f", d.craterFlowing);
            else std::snprintf(fl, sizeof(fl), "     -");
            if (d.craterSettled >= 0) std::snprintf(se, sizeof(se), "%6.1f", d.craterSettled);
            else std::snprintf(se, sizeof(se), "     -");
            if (d.lowerPile.ok) std::snprintf(pile, sizeof(pile), "%5.1f", d.lowerPile.meanDeg);
            else std::snprintf(pile, sizeof(pile), "    -");
            std::printf("  %5d  %6d  %.2f  %8d  %5.1f  %s  %s  %s  %.2f  %11ld  %s\n", depth,
                        d.charge, q / 1000.0, d.residual, 100.0 * d.residual / d.charge, fl, se,
                        pile, d.maxImbalance, d.settleTicks, d.massOk ? "ok" : "DRIFT");
            std::fflush(stdout);
        }
    }

    std::printf("\n  same sweep at hole half-width 6, depth 40:\n");
    std::printf("     q  residual    %%  flowing settled  settleTicks\n");
    for (int q : {0, 20, 50, 100, 250, 500, 1000}) {
        const Drain d = drainToRest(6, 40, q, seed, 400000);
        std::printf("  %.2f  %8d  %5.1f  %7.1f %7.1f  %11ld\n", q / 1000.0, d.residual,
                    100.0 * d.residual / d.charge, d.craterFlowing, d.craterSettled,
                    d.settleTicks);
        std::fflush(stdout);
    }

    // The attractor's own throughput, isolated from the free-fall phase. Once
    // gravity alone has done everything it can, whatever is left can only reach
    // the hole by creeping, and that creep rate is what sets the shortest
    // duration the timer can offer.
    std::printf("\n  attractor-limited throughput (depth 40, hw 3): the phase AFTER\n");
    std::printf("  gravity alone has stalled, which is the rate that bounds the timer.\n");
    std::printf("     q   free ticks  free drained   creep ticks  creep drained  creep/tick\n");
    const Drain base = drainToRest(3, 40, 0, seed, 400000);
    for (int q : {20, 50, 100, 250, 500, 1000}) {
        const Drain d = drainToRest(3, 40, q, seed, 400000);
        const long creepTicks = d.settleTicks - base.settleTicks;
        const int creepGrains = d.delivered - base.delivered;
        std::printf("  %.2f  %11ld  %12d  %12ld  %13d  %10.3f\n", q / 1000.0, base.settleTicks,
                    base.delivered, creepTicks, creepGrains,
                    creepTicks > 0 ? static_cast<double>(creepGrains) / creepTicks : 0.0);
        std::fflush(stdout);
    }
}

// ------------------------------------------------------------ experiment h --

/// A flat full-width slab is the naive reading of "the hourglass is full", and
/// section B shows it strands catastrophically. The alternative charge shape is
/// the one sand actually takes when poured onto a flat floor: a heap with
/// 45-degree faces, centred on the hole. Crucially it is also the shape the
/// LOWER pile ends up in, so turning the device over regenerates it for free.
void sectionH(uint32_t seed) {
    std::printf("\n=== H. heaped charge instead of a flat slab ===\n");
    std::printf("charge is a 45-degree heap of height h centred on the hole, clipped by the\n");
    std::printf("side walls. Compare residuals against section B at equal grain counts.\n");
    std::printf("   h  hw     q  charge  residual    %%   t99   settle  flowing  skew  s@30  s@60\n");
    const int heights[] = {20, 30, 40, 50, 60};
    for (int h : heights) {
        for (int hw : {1, 2, 3, 6}) {
            for (int q : {0, 100, 250, 500, 1000}) {
                const Drain d = drainToRest(hw, h, q, seed, 400000, true);
                const Secs s30(static_cast<double>(d.settleTicks), 30);
                const Secs s60(static_cast<double>(d.settleTicks), 60);
                std::printf("  %3d  %2d  %.2f  %6d  %8d  %5.1f  %5ld  %7ld  %7.1f  %.2f  %5s  "
                            "%5s\n",
                            h, hw, q / 1000.0, d.charge, d.residual,
                            100.0 * d.residual / d.charge, d.ticksTo99, d.settleTicks,
                            d.craterFlowing, d.maxImbalance, s30.c(), s60.c());
                std::fflush(stdout);
            }
        }
    }
}

// ------------------------------------------------------------ experiment i --

/// Turning the device over is the whole interaction, so the second drain has to
/// behave like the first. Gravity flips to N; the walls do not move. Whatever
/// the first drain piled up in the lower chamber becomes the charge, with no
/// help from any re-fill routine.
void sectionI(uint32_t seed) {
    std::printf("\n=== I. flip repeatability ===\n");
    std::printf("drain down, then flip gravity and drain up, on the same grains.\n");
    std::printf("  charge  hw     q   pass  delivered  residual    %%  settleTicks  mass\n");
    struct Case {
        int h, hw, q;
        bool cone;
    };
    const Case cases[] = {
        {40, 3, 250, true}, {60, 3, 250, true}, {40, 3, 250, false}, {60, 2, 250, true},
    };
    for (const Case& c : cases) {
        Trial t(c.hw, seed);
        const int charge = c.cone ? t.chargeCone(c.h) : t.charge(c.h);
        for (int pass = 1; pass <= 3; ++pass) {
            t.setGravity((pass % 2) ? Gravity::S : Gravity::N);
            long tick = 0;
            for (; tick < 400000; ++tick) {
                int grav = 0;
                t.tick(c.q, &grav);
                if (t.atRest(grav, c.q)) break;
            }
            std::printf("  %6d  %2d  %.2f  %5d  %9d  %8d  %5.1f  %11ld  %s\n", charge, c.hw,
                        c.q / 1000.0, pass, t.dest(), t.source(), 100.0 * t.source() / charge, tick,
                        t.massOk() ? "ok" : "DRIFT");
            std::fflush(stdout);
        }
    }
}

// ------------------------------------------------------------ experiment d --

void sectionD(uint32_t seed) {
    std::printf("\n=== D. angle of repose, after FULL relaxation ===\n");
    std::printf("a heap rained through a narrow hole, small enough not to reach the side\n");
    std::printf("walls. Run to zero moves, not to a tick budget.\n");
    std::printf("  charge  hw    q   height  mean deg  left   right   pts   ticks  conv\n");
    struct Case {
        int depth, hw, q;
    };
    const Case cases[] = {
        {6, 1, 250}, {10, 1, 250}, {10, 2, 250}, {14, 2, 250}, {10, 3, 250}, {10, 3, 100},
    };
    for (const Case& c : cases) {
        Trial t(c.hw, seed);
        const int charge = t.charge(c.depth);
        long tick = 0;
        bool conv = false;
        // Phase 1: drain with the attractor on, so the heap is built by rain
        // through the hole rather than left half in the upper chamber.
        for (; tick < 400000; ++tick) {
            int grav = 0;
            t.tick(c.q, &grav);
            if (t.upper() == 0) break;
        }
        // Phase 2: attractor off, relax to a true standstill.
        long relax = 0;
        for (; relax < 400000; ++relax) {
            int grav = 0;
            t.tick(0, &grav);
            if (grav == 0) {
                conv = true;
                break;
            }
        }
        const Repose r = pileRepose(t.sand(), LOWER_TOP, LOWER_BOT);
        if (r.ok) {
            std::printf("  %6d  %2d  %.2f  %6d  %8.1f  %5.1f  %6.1f  %2d+%2d  %6ld  %s\n", charge,
                        c.hw, c.q / 1000.0, r.apexHeight, r.meanDeg, r.leftDeg, r.rightDeg, r.nl,
                        r.nr, tick + relax, conv ? "yes" : "NO");
        } else {
            std::printf("  %6d  %2d  %.2f  %6d   not measurable (%d+%d free points)\n", charge,
                        c.hw, c.q / 1000.0, r.apexHeight, r.nl, r.nr);
        }
        std::fflush(stdout);
    }
}

// ------------------------------------------------------------ experiment e --

void sectionE(uint32_t seed) {
    std::printf("\n=== E. drain time versus fill depth and hole width ===\n");
    std::printf("unmetered: the hole is simply open. seconds at 30 Hz / 60 Hz.\n");
    std::printf("  q     depth  charge  hw   t90     t99    settle    s@30   s@60   residual\n");
    const int depths[] = {10, 20, 40, 60};
    const int hws[] = {1, 2, 3, 4, 6, 10};
    for (int q : {0, 100}) {
        for (int depth : depths) {
            for (int hw : hws) {
                const Drain d = drainToRest(hw, depth, q, seed, 400000);
                const Secs s30(static_cast<double>(d.settleTicks), 30);
                const Secs s60(static_cast<double>(d.settleTicks), 60);
                std::printf("  %.2f  %5d  %6d  %2d  %6ld  %6ld  %7ld  %6s %6s  %8d\n", q / 1000.0,
                            depth, d.charge, hw, d.ticksTo90, d.ticksTo99, d.settleTicks, s30.c(),
                            s60.c(), d.residual);
                std::fflush(stdout);
            }
        }
    }
}

// ------------------------------------------------------------ experiment f --

struct Metered {
    int charge = 0;
    int delivered = 0;
    int residual = 0;
    long ticks = 0;
    double maxLagTicks = 0; ///< worst schedule error, expressed in ticks
    double rmsLagTicks = 0;
    int gateOpenTicks = 0;
    double maxImbalance = 0; ///< worst left/right mass split in the draining chamber
    bool massOk = true;
    bool finished = false;
};

/// Deadbeat position control on the gate: the schedule says how many grains
/// SHOULD have crossed by now, and the gate is opened whenever fewer have. A
/// position controller rather than a rate controller, so a tick the throat could
/// not supply is made up later instead of being lost.
///
/// `preFlip` runs a full drain first and then turns the device over, so the
/// timer starts from the state a user actually hands it: not a tidy heap placed
/// by a fill routine, but whatever the previous run left in the other chamber,
/// now upside down. That state drains appreciably more slowly, and a budget
/// measured from the tidy heap flatters the design by a third.
Metered meteredRun(int hw, int depth, int qPerMille, long targetTicks, uint32_t seed,
                   bool cone = false, bool preFlip = false) {
    Trial t(hw, seed);
    Metered m;
    m.charge = cone ? t.chargeCone(depth) : t.charge(depth);

    if (preFlip) {
        for (long i = 0; i < 400000; ++i) {
            int grav = 0;
            t.tick(qPerMille, &grav);
            if (t.atRest(grav, qPerMille)) break;
        }
        t.setGravity(Gravity::N);
    }

    const double perTick = static_cast<double>(m.charge) / static_cast<double>(targetTicks);
    const int start = t.dest();

    double sumSq = 0;
    long tick = 0;
    for (; tick < targetTicks; ++tick) {
        const int want = static_cast<int>(std::llround(perTick * (tick + 1)));
        const int have = t.dest() - start;
        t.setGate(have < want);
        if (have < want) ++m.gateOpenTicks;
        int grav = 0;
        t.tick(qPerMille, &grav);
        if ((tick & 63) == 0 && t.source() * 2 > m.charge) {
            const double im = std::fabs(t.imbalance());
            if (im > m.maxImbalance) m.maxImbalance = im;
        }
        const double lag = (want - (t.dest() - start)) / (perTick > 0 ? perTick : 1.0);
        if (std::fabs(lag) > std::fabs(m.maxLagTicks)) m.maxLagTicks = lag;
        sumSq += lag * lag;
    }
    m.ticks = tick;
    m.rmsLagTicks = std::sqrt(sumSq / static_cast<double>(targetTicks));
    m.delivered = t.dest() - start;
    m.residual = t.source();
    m.massOk = t.massOk();
    m.finished = (m.residual == 0);
    return m;
}

void sectionF(uint32_t seed) {
    std::printf("\n=== F. grain budget for real durations ===\n");
    std::printf("target durations 30 s, 2 min, 10 min. Gate under deadbeat position control.\n");
    std::printf("every run starts from a FLIPPED state -- charge, drain it completely, turn the\n");
    std::printf("device over -- because that is the state a running timer is actually handed.\n");
    std::printf("lag is reported in SECONDS of visual error against the ideal linear ramp.\n");

    struct Target {
        const char* name;
        int seconds;
    };
    const Target targets[] = {{"30 s", 30}, {"2 min", 120}, {"10 min", 600}};

    struct Charge {
        const char* shape;
        int size;
        bool cone;
    };
    const Charge charges[] = {
        {"heap h=20", 20, true},  {"heap h=30", 30, true},  {"heap h=40", 40, true},
        {"heap h=60", 60, true},  {"slab d=20", 20, false},
    };

    // The simulation rate is a free parameter -- it does not have to equal the
    // frame rate. Ticking twice per drawn frame doubles the grain budget for the
    // shortest duration at no visual cost, and 30 s is the duration that needs
    // it.
    for (int hz : {30, 60, 120}) {
        std::printf("\n  --- %d Hz simulation ---\n", hz);
        std::printf("  target  charge shape  grains  hw     q     rate  gate%%  residual  maxLag "
                    " rmsLag  skew  mass\n");
        for (const Target& tg : targets) {
            const long ticks = static_cast<long>(tg.seconds) * hz;
            for (const Charge& c : charges) {
                for (int q : {250, 400}) {
                    const int hw = 2;
                    const Metered m = meteredRun(hw, c.size, q, ticks, seed, c.cone, true);
                    std::printf("  %6s  %11s  %6d  %2d  %.2f  %7.3f  %5.1f  %8d  %6.2fs %6.2fs  "
                                "%.2f  %s\n",
                                tg.name, c.shape, m.charge, hw, q / 1000.0,
                                static_cast<double>(m.charge) / static_cast<double>(ticks),
                                100.0 * m.gateOpenTicks / static_cast<double>(ticks), m.residual,
                                m.maxLagTicks / hz, m.rmsLagTicks / hz, m.maxImbalance,
                                m.massOk ? "ok" : "DRIFT");
                    std::fflush(stdout);
                }
            }
        }
    }
}

// ------------------------------------------------------------ experiment g --

void sectionG(uint32_t seed) {
    std::printf("\n=== G. cost per tick ===\n");
    std::printf("same grain count in both columns (the simulation conserves mass), so the\n");
    std::printf("moving/settled ratio is a like-for-like comparison.\n");
    std::printf("  depth  grains   moving us/tick   settled us/tick   ratio   moves/tick\n");

    for (int depth : {0, 10, 20, 40, 60}) {
        Trial t(4, seed);
        const int charge = t.charge(depth);
        t.setMassCheck(false); // the mass check is a full popcount; keep it out of the timing

        // Moving phase.
        const int kMoving = 400;
        long long moves = 0;
        const auto m0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kMoving; ++i) {
            int grav = 0;
            t.tick(0, &grav);
            moves += grav;
        }
        const auto m1 = std::chrono::steady_clock::now();
        const double movingUs =
            std::chrono::duration<double, std::micro>(m1 - m0).count() / kMoving;

        // Settle completely, then time the standstill.
        for (long i = 0; i < 400000; ++i) {
            int grav = 0;
            t.tick(0, &grav);
            if (grav == 0) break;
        }
        const int kSettled = 2000;
        const auto s0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kSettled; ++i) {
            int grav = 0;
            t.tick(0, &grav);
        }
        const auto s1 = std::chrono::steady_clock::now();
        const double settledUs =
            std::chrono::duration<double, std::micro>(s1 - s0).count() / kSettled;

        std::printf("  %5d  %6d  %14.2f  %16.2f  %6.2f  %11.1f\n", depth, charge, movingUs,
                    settledUs, settledUs > 0 ? movingUs / settledUs : 0.0,
                    static_cast<double>(moves) / kMoving);
        std::fflush(stdout);
    }

    std::printf("\n  grid is %d x %d = %d cells; the scan visits every one of them every\n", W, H,
                W * H);
    std::printf("  tick regardless of how much is moving.\n");
}

// ------------------------------------------------------------ experiment j --

/// Prints the surface profile itself. A fitted angle is only worth as much as
/// the profile it was fitted to, and a slope that reads 12 degrees where 45 was
/// expected is either a real result or a broken instrument -- this is how the
/// difference gets settled.
void sectionJ(uint32_t seed) {
    std::printf("\n=== J. surface profiles (instrument check) ===\n");
    std::printf("height above the floor, sampled every 4 columns, hole at column %d.\n", HOLE_X);

    struct Shot {
        const char* label;
        int hw, size, q;
        bool cone;
        double stopAt; ///< dump when this fraction has crossed; 1.0 = fully settled
    };
    const Shot shots[] = {
        {"slab d=40 q=0, settled", 3, 40, 0, false, 1.0},
        {"slab d=40 q=0, mid-drain", 3, 40, 0, false, 0.5},
        {"heap h=60 q=0, settled", 3, 60, 0, true, 1.0},
        {"heap h=60 q=0, mid-drain", 3, 60, 0, true, 0.5},
        {"heap h=60 q=0.25, mid-drain", 3, 60, 250, true, 0.5},
        {"heap h=60 q=1.00, mid-drain", 3, 60, 1000, true, 0.5},
    };

    for (const Shot& sh : shots) {
        Trial t(sh.hw, seed);
        const int charge = sh.cone ? t.chargeCone(sh.size) : t.charge(sh.size);
        for (long i = 0; i < 400000; ++i) {
            int grav = 0;
            t.tick(sh.q, &grav);
            if (sh.stopAt < 1.0 && t.lower() >= sh.stopAt * charge) break;
            if (t.atRest(grav, sh.q)) break;
        }
        const std::vector<int> top = surface(t.sand(), UPPER_TOP, UPPER_BOT);
        std::printf("\n  %s  (upper %d, lower %d)\n", sh.label, t.upper(), t.lower());
        std::printf("   col:");
        for (int x = 2; x <= W - 2; x += 4) std::printf(" %3d", x);
        std::printf("\n   hgt:");
        for (int x = 2; x <= W - 2; x += 4) {
            const int tp = top[static_cast<size_t>(x)];
            if (tp < 0) std::printf("   .");
            else std::printf(" %3d", FLOOR_Y - tp);
        }
        const Crater c = craterAngle(t.sand());
        std::printf("\n   upper surface  left %.1f deg / right %.1f deg (skew %.1f), lower pile ",
                    c.leftDeg, c.rightDeg, c.skewDeg);
        const Repose r = pileRepose(t.sand(), LOWER_TOP, LOWER_BOT);
        if (r.ok) std::printf("%.1f deg\n", r.meanDeg);
        else std::printf("not measurable\n");
        std::fflush(stdout);
    }
}

int parseInt(const char* v, int fallback) {
    if (!v) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return (end == v) ? fallback : static_cast<int>(n);
}

} // namespace

int main(int argc, char** argv) {
    uint32_t seed = 20250731u;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : nullptr;
        if (a == "--seed") {
            seed = static_cast<uint32_t>(parseInt(v, 0));
            ++i;
        } else if (a == "--only") {
            only = v ? v : "";
            ++i;
        } else {
            std::printf("usage: sand_tune [--only a|b|c|d|e|f|g|h|i|j] [--seed S]\n");
            return 2;
        }
    }

    std::printf("flat-floor hourglass tuning run\n");
    std::printf("grid %dx%d cells at 2 px, floor row %d, hole centre column %d\n", W, H, FLOOR_Y,
                HOLE_X);
    std::printf("upper chamber %d rows x %d cols (%d cells), lower %d x %d (%d cells)\n",
                UPPER_ROWS, INNER_W, UPPER_ROWS * INNER_W, LOWER_ROWS, INNER_W, LOWER_CAP);
    std::printf("seed %u\n", seed);

    auto want = [&](const char* s) { return only.empty() || only == s; };
    if (want("a")) sectionA(seed);
    if (want("b")) sectionB(seed);
    if (want("c")) sectionC(seed);
    if (want("d")) sectionD(seed);
    if (want("e")) sectionE(seed);
    if (want("h")) sectionH(seed);
    if (want("f")) sectionF(seed);
    if (want("g")) sectionG(seed);
    if (want("i")) sectionI(seed);
    if (want("j")) sectionJ(seed);
    return 0;
}
