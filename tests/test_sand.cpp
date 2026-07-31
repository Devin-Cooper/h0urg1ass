#include "doctest.h"

#include "faces/timer_face.hpp"
#include "sand/sand_sim.hpp"
#include "timer/timer_model.hpp"

using h0::Gravity;
using h0::SandGrid;
using h0::SandSim;

namespace {

/// A vessel: a border, plus a horizontal floor across the middle with a gap.
/// This is the shape the face actually uses -- no bulbs, no taper.
SandGrid vessel(int holeHalfWidth = 3) {
    SandGrid w;
    const int mid = SandGrid::H / 2;
    for (int x = 0; x < SandGrid::W; ++x) {
        w.set(x, 0, true);
        w.set(x, SandGrid::H - 1, true);
        const bool inHole = (x >= SandGrid::W / 2 - holeHalfWidth) &&
                            (x <= SandGrid::W / 2 + holeHalfWidth);
        if (!inHole) w.set(x, mid, true);
    }
    for (int y = 0; y < SandGrid::H; ++y) {
        w.set(0, y, true);
        w.set(SandGrid::W - 1, y, true);
    }
    return w;
}

/// Fill the upper chamber from the floor upward.
void charge(SandSim& s, int rows) {
    const int mid = SandGrid::H / 2;
    for (int y = mid - 1; y > mid - 1 - rows && y > 0; --y)
        for (int x = 1; x < SandGrid::W - 1; ++x)
            if (!s.walls().get(x, y)) s.sand().set(x, y, true);
}

int settle(SandSim& s, Gravity g, int maxTicks = 4000) {
    int t = 0;
    for (; t < maxTicks; ++t)
        if (s.step(g) == 0) break;
    return t;
}

} // namespace

TEST_CASE("sand is conserved, tick after tick") {
    // The property everything else rests on. Double buffering fails this -- a
    // faller and a diagonal slider claim the same cell and one is destroyed --
    // which is why the simulation updates in place.
    SandSim s;
    s.setWalls(vessel());
    s.seed(12345);
    charge(s, 20);

    const int start = s.sand().count();
    REQUIRE(start > 500);
    for (int i = 0; i < 600; ++i) {
        s.step(Gravity::S);
        REQUIRE(s.sand().count() == start);
    }
}

TEST_CASE("sand is conserved under every gravity direction") {
    const Gravity all[] = {Gravity::S,  Gravity::SW, Gravity::W,  Gravity::NW,
                           Gravity::N,  Gravity::NE, Gravity::E,  Gravity::SE};
    for (Gravity g : all) {
        SandSim s;
        s.setWalls(vessel());
        s.seed(999);
        charge(s, 15);
        const int start = s.sand().count();
        for (int i = 0; i < 200; ++i) s.step(g);
        CHECK(s.sand().count() == start);
    }
}

TEST_CASE("sand never enters a wall") {
    SandSim s;
    s.setWalls(vessel());
    s.seed(7);
    charge(s, 25);
    for (int i = 0; i < 800; ++i) s.step(Gravity::S);

    for (int y = 0; y < SandGrid::H; ++y)
        for (int x = 0; x < SandGrid::W; ++x)
            if (s.walls().get(x, y)) REQUIRE_FALSE(s.sand().get(x, y));
}

TEST_CASE("sand falls through the hole into the lower chamber") {
    SandSim s;
    s.setWalls(vessel());
    s.seed(42);
    charge(s, 20);
    const int mid = SandGrid::H / 2;

    const int upperBefore = s.sand().countRows(0, mid - 1);
    REQUIRE(upperBefore > 0);
    CHECK(s.sand().countRows(mid, SandGrid::H - 1) == 0);

    settle(s, Gravity::S);

    CHECK(s.sand().countRows(mid, SandGrid::H - 1) > 0);
    CHECK(s.sand().countRows(0, mid - 1) < upperBefore);
}

TEST_CASE("a grain never moves more than one cell per tick") {
    // The teleport bug. A scan running WITH gravity lets a grain move, then be
    // visited again in the same tick and move again -- a column collapses
    // instantly instead of falling.
    SandSim s;
    SandGrid w = vessel();
    s.setWalls(w);
    s.seed(3);

    // One grain, high up, in clear air.
    s.sand().set(SandGrid::W / 2 - 20, 5, true);
    for (int t = 0; t < 30; ++t) {
        int found = -1;
        for (int y = 0; y < SandGrid::H; ++y)
            if (s.sand().get(SandGrid::W / 2 - 20, y)) { found = y; break; }
        REQUIRE(found >= 0);
        s.step(Gravity::S);
        int after = -1;
        for (int y = 0; y < SandGrid::H; ++y)
            if (s.sand().get(SandGrid::W / 2 - 20, y)) { after = y; break; }
        if (after >= 0) CHECK(after - found <= 1);
    }
}

TEST_CASE("the teleport bug does not reappear when gravity points sideways") {
    // Bottom-to-top is only correct while gravity points down. Tilt left and the
    // same fault returns rotated 90 degrees unless the scan order rotates too.
    SandSim s;
    s.setWalls(vessel());
    s.seed(11);

    const int y = 20;
    for (int x = 60; x < 70; ++x) s.sand().set(x, y, true);
    const int start = s.sand().count();

    // Leftmost grain must advance at most one cell per tick.
    int prevLeft = 60;
    for (int t = 0; t < 20; ++t) {
        s.step(Gravity::W);
        int left = SandGrid::W;
        for (int x = 0; x < SandGrid::W; ++x)
            if (s.sand().get(x, y)) { left = x; break; }
        CHECK(prevLeft - left <= 1);
        prevLeft = left;
    }
    CHECK(s.sand().count() == start);
}

TEST_CASE("no grain moves more than one cell, under any gravity, with the diagonals forced") {
    // The test the suite was missing. The two teleport tests launch a grain into
    // EMPTY space, so the straight-ahead branch always succeeds and the diagonal
    // branch never runs -- and it is the diagonals that move on the axis the
    // outer loop iterates. Under E/W gravity a grain slid into a row not yet
    // visited, was visited again, and slid again: measured at up to 51 cells in
    // one tick.
    //
    // So this packs the vessel densely enough that grains are constantly
    // blocked ahead and forced onto their diagonals, then checks every grain's
    // displacement between consecutive ticks.
    const Gravity all[] = {Gravity::S,  Gravity::SW, Gravity::W,  Gravity::NW,
                           Gravity::N,  Gravity::NE, Gravity::E,  Gravity::SE};
    for (Gravity g : all) {
        SandSim s;
        s.setWalls(vessel());
        s.seed(31337);
        charge(s, 30);

        // Bounding box, not per-cell provenance. Grains CHAIN -- A vacates a
        // cell and B moves into it in the same tick -- so "the source cell must
        // now be empty" is not a real invariant and produces false failures.
        // The extent of the sand, however, can only grow by one cell per tick
        // in any direction, and a 51-cell jump breaks that unmissably.
        auto bounds = [](const SandGrid& gr, int& x0, int& x1, int& y0, int& y1) {
            x0 = SandGrid::W; x1 = -1; y0 = SandGrid::H; y1 = -1;
            for (int y = 0; y < SandGrid::H; ++y)
                for (int x = 0; x < SandGrid::W; ++x)
                    if (gr.get(x, y)) {
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
        };

        for (int t = 0; t < 60; ++t) {
            int ax0, ax1, ay0, ay1;
            bounds(s.sand(), ax0, ax1, ay0, ay1);
            s.step(g);
            int bx0, bx1, by0, by1;
            bounds(s.sand(), bx0, bx1, by0, by1);

            CAPTURE(static_cast<int>(g));
            CAPTURE(t);
            CHECK(ax0 - bx0 <= 1);
            CHECK(bx1 - ax1 <= 1);
            CHECK(ay0 - by0 <= 1);
            CHECK(by1 - ay1 <= 1);
        }
    }
}

TEST_CASE("a settled pile is centred, not sheared to one side") {
    // A fixed scan direction is a systematic shear -- whichever end of a row is
    // visited first gets to move into the space, every row, every tick. Measured
    // at a 0.53-0.63 left/right mass split before the scan direction was
    // randomised: a squeegee rather than a hopper.
    SandSim s;
    s.setWalls(vessel());
    s.seed(4242);

    // Rain a centred column onto the lower floor and let it heap.
    const int cx = SandGrid::W / 2;
    for (int y = SandGrid::H / 2 + 2; y < SandGrid::H / 2 + 40; ++y)
        for (int x = cx - 3; x <= cx + 3; ++x) s.sand().set(x, y, true);
    settle(s, Gravity::S);

    // Centre of mass must sit near the axis it was poured onto.
    long sumX = 0, n = 0;
    for (int y = SandGrid::H / 2; y < SandGrid::H; ++y)
        for (int x = 0; x < SandGrid::W; ++x)
            if (s.sand().get(x, y)) { sumX += x; ++n; }
    REQUIRE(n > 100);
    const double centroid = static_cast<double>(sumX) / static_cast<double>(n);
    CHECK(centroid == doctest::Approx(cx).epsilon(0.05));
}

TEST_CASE("a pile settles rather than churning forever") {
    // If it never reaches zero movement, the caller can never skip work and the
    // battery pays for a picture that is not changing.
    SandSim s;
    s.setWalls(vessel());
    s.seed(5150);
    charge(s, 10);
    const int ticks = settle(s, Gravity::S, 6000);
    CHECK(ticks < 6000); // reached a genuine fixpoint
}

TEST_CASE("the pile makes a slope, not a level") {
    // Sand is granular: it must heap. A flat top would read as liquid.
    SandSim s;
    s.setWalls(vessel());
    s.seed(808);

    // Drop a narrow column onto the lower floor.
    const int cx = SandGrid::W / 2;
    for (int y = SandGrid::H / 2 + 2; y < SandGrid::H / 2 + 30; ++y)
        for (int x = cx - 2; x <= cx + 2; ++x) s.sand().set(x, y, true);
    settle(s, Gravity::S);

    // Measure the pile width at two heights above the floor.
    auto widthAt = [&](int y) {
        int lo = SandGrid::W, hi = -1;
        for (int x = 0; x < SandGrid::W; ++x)
            if (s.sand().get(x, y)) { if (x < lo) lo = x; hi = x; }
        return (hi < 0) ? 0 : hi - lo + 1;
    };
    const int floorY = SandGrid::H - 2;
    CHECK(widthAt(floorY) > widthAt(floorY - 8)); // wider at the base
}

TEST_CASE("the same seed produces the same drain") {
    // Determinism is what makes a stochastic system testable at all, and it is
    // what lets a golden image of a sand frame mean anything.
    SandSim a, b;
    a.setWalls(vessel());
    b.setWalls(vessel());
    a.seed(2024);
    b.seed(2024);
    charge(a, 12);
    charge(b, 12);

    for (int i = 0; i < 300; ++i) { a.step(Gravity::S); b.step(Gravity::S); }

    for (int y = 0; y < SandGrid::H; ++y)
        for (int x = 0; x < SandGrid::W; ++x)
            REQUIRE(a.sand().get(x, y) == b.sand().get(x, y));
}

TEST_CASE("different seeds diverge") {
    // Otherwise the seed is decorative and the per-row randomness is not
    // actually reaching the rule.
    SandSim a, b;
    a.setWalls(vessel());
    b.setWalls(vessel());
    a.seed(1);
    b.seed(2);
    charge(a, 12);
    charge(b, 12);
    for (int i = 0; i < 200; ++i) { a.step(Gravity::S); b.step(Gravity::S); }

    int diff = 0;
    for (int y = 0; y < SandGrid::H; ++y)
        for (int x = 0; x < SandGrid::W; ++x)
            if (a.sand().get(x, y) != b.sand().get(x, y)) ++diff;
    CHECK(diff > 0);
    CHECK(a.sand().count() == b.sand().count()); // both still conserving
}

TEST_CASE("out of bounds reads as solid, so nothing escapes the array") {
    SandGrid g;
    CHECK(g.get(-1, 5));
    CHECK(g.get(SandGrid::W, 5));
    CHECK(g.get(5, -1));
    CHECK(g.get(5, SandGrid::H));
}

TEST_CASE("grid set and get round-trip across word boundaries") {
    // 104 columns is not a multiple of 32, so the last word is partial -- the
    // classic place for an off-by-one in a packed grid.
    SandGrid g;
    const int xs[] = {0, 31, 32, 63, 64, 95, 96, SandGrid::W - 1};
    for (int x : xs) g.set(x, 7, true);
    for (int x : xs) CHECK(g.get(x, 7));
    CHECK(g.count() == 8);
    for (int x : xs) g.set(x, 7, false);
    CHECK(g.count() == 0);
}

TEST_CASE("the drain gate absorbs a tick-rate change") {
    // The whole justification for ticking at 8 Hz while blanked. The gate is a
    // proportional controller on a cumulative count, so a slower rate must reach
    // the same place, not lag and then lurch on the way back.
    constexpr uint64_t SEC = 1'000'000ull;
    h0::TimerFace face;
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);

    uint64_t now = 0;
    face.tick(t, now);                    // seeds the charge
    const int charge = face.charge();
    REQUIRE(charge > 0);

    // 60 s at the full rate, then 180 s blanked at 8 Hz.
    face.setTickHz(30);
    for (; now < 60 * SEC; now += 33'333) face.tick(t, now);
    face.setTickHz(8);
    for (; now < 240 * SEC; now += 125'000) face.tick(t, now);

    // 240 of 300 s elapsed, so 80% should have fallen.
    const int wantAt240 = static_cast<int>(charge * 0.8f);
    CHECK(face.lowerCount() > wantAt240 - charge / 10);
    CHECK(face.lowerCount() < wantAt240 + charge / 10);

    // Back to full rate for a second: no lurch, no stall.
    const int before = face.lowerCount();
    face.setTickHz(30);
    for (; now < 241 * SEC; now += 33'333) face.tick(t, now);
    CHECK(face.lowerCount() >= before);
    CHECK(face.lowerCount() < before + charge / 10);
}
