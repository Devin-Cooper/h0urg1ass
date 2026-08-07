#include "doctest.h"
#include <cstdlib>
#include <cstring>

#include "golden.hpp"

#include <1bit/core/allocator.hpp>
#include <1bit/core/framebuffer.hpp>
#include <1bit/fonts/term_8x12.hpp>

#include <cstdio>

#include "app/settings_ui.hpp"
#include "faces/boot_face.hpp"
#include "faces/layout.hpp"
#include "faces/power_face.hpp"
#include "faces/settings_face.hpp"
#include "faces/timer_face.hpp"
#include "power/battery_model.hpp"
#include "render/raster_ops.hpp"
#include "sand/agitation.hpp"
#include "sand/sand_sim.hpp"
#include "sand/sand_render.hpp"
#include "sand/sand_vessel.hpp"
#include "timer/timer_model.hpp"

using onebit::BLACK;
using onebit::WHITE;

namespace {

constexpr uint64_t SEC = 1'000'000ull;
using Panel = onebit::Framebuffer<240, 280>;

/// The panel INTERIOR -- inside the border rail, which is the region the readout
/// has to itself.
///
/// It replaces the old CARD_* block, which named the lintel interior. That rect
/// meant something when the lintel was a wall: it was the region sand could not
/// reach. It means nothing now -- it is a sub-rect of the panel with no edge and
/// no owner -- so the tests that measured against it measure against this.
///
/// The border is excluded deliberately. A scan window that includes the rail
/// finds ink on all four sides no matter what, so an ink bounding box taken over
/// it is the window itself and could never notice anything.
constexpr int16_t CARD_X = h0::sandgeom::PANEL_X + 1;
constexpr int16_t CARD_Y = h0::sandgeom::PANEL_Y + 1;
constexpr int16_t CARD_W = h0::sandgeom::PANEL_W - 2;
constexpr int16_t CARD_H = h0::sandgeom::PANEL_H - 2;

/// Count ink pixels. The cheap invariant behind most of these tests: sand that
/// appears or vanishes is a conservation bug, and a blank face is a silent
/// failure that a golden baseline would happily enshrine.
int inkCount(const onebit::IFramebuffer& fb) {
    int n = 0;
    for (int16_t y = 0; y < fb.height(); ++y)
        for (int16_t x = 0; x < fb.width(); ++x)
            if (fb.getPixel(x, y) == BLACK) ++n;
    return n;
}

int inkInRect(const onebit::IFramebuffer& fb, int16_t x0, int16_t y0, int16_t w, int16_t h) {
    int n = 0;
    for (int16_t y = y0; y < y0 + h; ++y)
        for (int16_t x = x0; x < x0 + w; ++x)
            if (fb.getPixel(x, y) == BLACK) ++n;
    return n;
}

bool inRect(int16_t x, int16_t y, int16_t x0, int16_t y0, int16_t w, int16_t h) {
    return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
}

/// Differing pixels, restricted to (or excluded from) a rectangle.
int diff(const onebit::IFramebuffer& a, const onebit::IFramebuffer& b, int16_t x0, int16_t y0,
         int16_t w, int16_t h, bool inside) {
    int n = 0;
    for (int16_t y = 0; y < a.height(); ++y)
        for (int16_t x = 0; x < a.width(); ++x)
            if (inRect(x, y, x0, y0, w, h) == inside && a.getPixel(x, y) != b.getPixel(x, y)) ++n;
    return n;
}

/// Occupied grid cells in an inclusive grid rect.
///
/// Grid space is the only place "is there sand behind the readout" can be
/// asked. A framebuffer measurement cannot separate a grain from housing ink --
/// both are just black -- so every claim about what is behind the panel has to
/// be made here, against the simulation rather than against the picture.
int cellsInRect(const h0::SandGrid& g, int cx0, int cy0, int cx1, int cy1) {
    int n = 0;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
            if (g.get(cx, cy)) ++n;
    return n;
}

/// Cells that differ between two grids, over an inclusive grid rect.
int cellDiff(const h0::SandGrid& a, const h0::SandGrid& b, int cx0, int cy0, int cx1, int cy1) {
    int n = 0;
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
            if (a.get(cx, cy) != b.get(cx, cy)) ++n;
    return n;
}

/// What fraction of a PIXEL rect has sand behind it, as a percentage.
///
/// A second, independent derivation of the number `TimerFace` latches its cell
/// polarity on. The face is asked what it decided; this says what the sand
/// actually is, so the two can be compared instead of the test simply believing
/// whatever the face reports.
int coveragePct(const h0::SandGrid& g, const h0::Rect16& r) {
    const int cx0 = (r.x - h0::sandgeom::ORIGIN_X) / h0::sandgeom::SCALE;
    const int cy0 = (r.y - h0::sandgeom::ORIGIN_Y) / h0::sandgeom::SCALE;
    const int cx1 = (r.x + r.w - 1 - h0::sandgeom::ORIGIN_X) / h0::sandgeom::SCALE;
    const int cy1 = (r.y + r.h - 1 - h0::sandgeom::ORIGIN_Y) / h0::sandgeom::SCALE;
    const int total = (cx1 - cx0 + 1) * (cy1 - cy0 + 1);
    return total ? 100 * cellsInRect(g, cx0, cy0, cx1, cy1) / total : 0;
}

/// Pixels in the rect that are NOT the complement of each other. Zero means
/// every single pixel flipped -- which is what a polarity inversion is.
int complementIn(const onebit::IFramebuffer& a, const onebit::IFramebuffer& b, int16_t x0,
                 int16_t y0, int16_t w, int16_t h) {
    int n = 0;
    for (int16_t y = y0; y < y0 + h; ++y)
        for (int16_t x = x0; x < x0 + w; ++x)
            if (a.getPixel(x, y) == b.getPixel(x, y)) ++n;
    return n;
}

/// Nothing may be drawn under the rounded corners. The panel physically clips a
/// ~44 px radius, so ink there is invisible at best and clipped text at worst.
///
/// The clip is a rounded rectangle: within a corner quadrant the visible region
/// is a disc of radius R centred at (R, R) inwards from that corner, NOT the
/// complement of a disc centred on the corner point itself. The difference is
/// not academic -- the corner-point form rejects (16, 16), which `safe::INSET`
/// records as measured fully visible on hardware, and it went unnoticed until
/// the sand vessel became the first face to actually fill the safe box.
int inkInCorners(const onebit::IFramebuffer& fb) {
    constexpr int R = h0::safe::CORNER_R;
    const int16_t w = fb.width(), h = fb.height();
    int n = 0;
    for (int16_t y = 0; y < h; ++y) {
        for (int16_t x = 0; x < w; ++x) {
            // Distance from the arc centre of whichever corner this pixel is in.
            const int dx = (x < R) ? (R - x) : (x >= w - R ? x - (w - 1 - R) : 0);
            const int dy = (y < R) ? (R - y) : (y >= h - R ? y - (h - 1 - R) : 0);
            if (dx == 0 || dy == 0) continue; // not in a corner quadrant
            if (dx * dx + dy * dy > R * R && fb.getPixel(x, y) == BLACK) ++n;
        }
    }
    return n;
}

/// Run a face forward to a chosen instant at the simulation's own rate.
void runTo(h0::TimerFace& face, const h0::TimerModel& t, uint64_t& now, uint64_t to) {
    while (now < to) { now += 33'333ull; face.tick(t, now); }
}

} // namespace

// ------------------------------------------------------------- the helpers --

TEST_CASE("test helpers: complementIn, cellsInRect and cellDiff mean what they say") {
    // These three are about to carry the readout's legibility guarantee, so pin
    // what they actually count before anything relies on it. A helper that
    // silently returns zero would turn every assertion built on it green -- but
    // the more dangerous failure is a helper that quietly IGNORES an argument,
    // because that one still returns plausible numbers. A `cellsInRect` blind
    // to its rect counts the whole grid, which clears Task 2's `> 100` guard
    // even when the panel region is empty, rebuilding exactly the vacuous
    // guarantee this design exists to replace. So every argument is exercised
    // at a value where ignoring it, or transposing it, changes the answer.

    // -- complementIn --------------------------------------------------------
    Panel a, b;
    a.clear(WHITE);
    b.clear(BLACK);
    CHECK(complementIn(a, b, 0, 0, 40, 40) == 0);       // total inversion
    b.clear(WHITE);
    CHECK(complementIn(a, b, 0, 0, 40, 40) == 40 * 40); // identical, so nothing flipped

    // A non-square window at a non-zero origin, holding an off-centre patch
    // that the transposed window (50, 60, 40, 20) would miss entirely. Both the
    // origin and the w/h order are load-bearing at the real call sites: the
    // panel sits at (29, 18) and the flap cells inside it are asked about one
    // at a time, so this helper is essentially never called at (0, 0).
    constexpr int16_t WX = 50, WY = 60, WW = 20, WH = 40; // x 50..69, y 60..99
    constexpr int16_t PX = 52, PY = 90, PW = 3, PH = 7;   // in the window, below its transpose
    auto patch = [&](onebit::IFramebuffer& fb, onebit::Color c) {
        for (int16_t y = PY; y < PY + PH; ++y)
            for (int16_t x = PX; x < PX + PW; ++x) fb.setPixel(x, y, c);
    };

    a.clear(WHITE);
    b.clear(WHITE);
    patch(b, BLACK);
    // Only the patch flipped, so every other pixel in the window is counted.
    CHECK(complementIn(a, b, WX, WY, WW, WH) == WW * WH - PW * PH);

    b.clear(BLACK);
    patch(b, WHITE);
    // The mirror image: everything flipped except the patch, white in both.
    CHECK(complementIn(a, b, WX, WY, WW, WH) == PW * PH);

    // A window that excludes the patch entirely sees a clean inversion again,
    // which pins the origin from the other side.
    CHECK(complementIn(a, b, WX, WY, WW, PY - WY) == 0);

    // -- cellsInRect ---------------------------------------------------------
    // Hand-built and asymmetric in both axes, so a bound that is ignored,
    // off by one, or swapped with its partner lands on a different answer.
    constexpr int GW = h0::SandGrid::W - 1, GH = h0::SandGrid::H - 1;
    h0::SandGrid g;
    g.set(3, 1, true);
    g.set(4, 1, true);
    g.set(3, 2, true);
    g.set(60, 90, true);

    CHECK(cellsInRect(g, 0, 0, GW, GH) == 4);
    CHECK(cellsInRect(g, 3, 1, 4, 2) == 3); // the far cell is outside
    CHECK(cellsInRect(g, 4, 1, 4, 2) == 1); // cx0 drops column 3
    CHECK(cellsInRect(g, 3, 1, 3, 2) == 2); // cx1 drops column 4
    CHECK(cellsInRect(g, 3, 2, 4, 2) == 1); // cy0 drops row 1
    CHECK(cellsInRect(g, 3, 1, 4, 1) == 2); // cy1 drops row 2
    CHECK(cellsInRect(g, 3, 1, 3, 1) == 1); // one cell: the bounds are inclusive
    CHECK(cellsInRect(g, 0, 0, 2, GH) == 0); // a full-height strip left of everything
    CHECK(cellsInRect(g, 1, 3, 2, 4) == 0);  // the transposed rect: x and y do not commute

    // -- cellDiff ------------------------------------------------------------
    // Two genuinely different grids. Passing the same grid twice is a tautology
    // for any implementation, including one that never reads its second
    // argument -- and "the sand behind the panel changed" is exactly the
    // question a b-blind cellDiff would answer "no" to forever.
    h0::SandGrid h = g;
    h.set(3, 1, false); // set in g, clear in h
    h.set(70, 5, true); // clear in g, set in h

    CHECK(cellDiff(g, h, 0, 0, GW, GH) == 2);
    CHECK(cellDiff(h, g, 0, 0, GW, GH) == 2);    // and it is symmetric
    CHECK(cellDiff(g, h, 3, 1, 4, 2) == 1);      // only the cleared cell is in this rect
    CHECK(cellDiff(g, h, 60, 80, 80, 100) == 0); // a rect the two agree on
    CHECK(cellDiff(g, g, 0, 0, GW, GH) == 0);    // a grid never differs from itself

    // -- and against real sand -----------------------------------------------
    // The distribution is not known ahead of time, but the partition identity
    // is: any split of the grid must sum to the whole. A rect-blind helper
    // returns the full count for both halves and doubles the total.
    h0::SandVessel v;
    v.begin();
    v.reset(3u, 400);
    const h0::SandGrid& s = v.sand();
    const int all = cellsInRect(s, 0, 0, GW, GH);
    CHECK(all == 400);
    CHECK(cellsInRect(s, 0, 0, GW, h0::sandgeom::FLOOR_ROW)
              + cellsInRect(s, 0, h0::sandgeom::FLOOR_ROW + 1, GW, GH)
          == all);
    CHECK(cellsInRect(s, 0, 0, GW / 2, GH) + cellsInRect(s, GW / 2 + 1, 0, GW, GH) == all);
}

// ------------------------------------------------- the legibility mechanism --

TEST_CASE("the readout panel is opaque: sand behind it changes nothing inside it") {
    // The invariant that replaces "sand cannot enter the lintel". It is no
    // longer physically impossible for sand to be behind the readout -- it is
    // expected -- so the guarantee moves to compositing: whatever is behind,
    // the panel's pixels are the same as they would be with no sand at all.
    onebit::init();
    h0::TimerModel t;
    t.setDuration(300 * SEC);        // > 180 s -> the 2000-grain tier
    t.start(0);

    h0::TimerFace charged;
    charged.restart(t, 1u);
    h0::TimerFace bare;              // vessel never begun: no sand, no walls

    // LOAD-BEARING, and it has to be measured over the region that used to be
    // WALL -- not over the panel rect, which is mostly ordinary chamber and
    // always was.
    //
    // The panel is grid x 6..97, y 1..46. The lintel was only x 23..81, y 1..26,
    // so the panel rect contains a large slab that sand could always occupy.
    // Measured on this fixture: 1745 occupied cells in the panel rect, of which
    // just 531 lie in the ex-lintel rect. A `panel > 100` guard was therefore
    // satisfied BEFORE the wall came out, and would stay satisfied if the wall
    // were put back -- it rules out nothing. The ex-lintel count is the one that
    // was exactly zero by construction until this change, so that is the one
    // that has to carry the guard.
    //
    // Both counts are seed-independent at frame 0: reset() places grains by
    // geometry and the seed only perturbs the tick. Verified 531 across seeds
    // 1, 5, 11, 1234 and 0xC0FFEE.
    const int behindPanel = cellsInRect(charged.sand(),
                                        h0::sandgeom::PANEL_CX0, h0::sandgeom::PANEL_CY0,
                                        h0::sandgeom::PANEL_CX1, h0::sandgeom::PANEL_CY1);
    const int behindReadout = cellsInRect(charged.sand(),
                                          h0::sandgeom::LINTEL_CX0, h0::sandgeom::LINTEL_CY0,
                                          h0::sandgeom::LINTEL_CX1, h0::sandgeom::LINTEL_CY1);
    CAPTURE(behindPanel);
    CAPTURE(behindReadout);
    REQUIRE(behindReadout > 300);

    Panel a, b;
    charged.render(a, t, 0);
    bare.render(b, t, 0);
    CHECK(diff(a, b, h0::sandgeom::PANEL_X, h0::sandgeom::PANEL_Y,
               h0::sandgeom::PANEL_W, h0::sandgeom::PANEL_H, true) == 0);
}

TEST_CASE("the panel stays opaque through every flap phase and when expired") {
    // One frame proves nothing about the two moments the panel is most likely
    // to leak: mid-flap, when the widget writes WHITE to occlude a falling
    // card, and Expired, when invertSafeBox blackens everything under it.
    onebit::init();
    for (int expired = 0; expired < 2; ++expired) {
        h0::TimerModel t;
        t.setDuration(300 * SEC);
        t.start(0);
        h0::TimerFace charged;
        charged.restart(t, 5u);
        h0::TimerFace bare;
        if (expired) t.tick(301 * SEC);

        // Same guard as above, for the same reason: the face is never ticked
        // here, so the sand stays at its charged frame-0 layout and there really
        // is sand in the region that used to be solid wall.
        const int behindReadout = cellsInRect(charged.sand(),
                                              h0::sandgeom::LINTEL_CX0, h0::sandgeom::LINTEL_CY0,
                                              h0::sandgeom::LINTEL_CX1, h0::sandgeom::LINTEL_CY1);
        CAPTURE(behindReadout);
        REQUIRE(behindReadout > 300);

        for (int i = 1; i <= 60; ++i) {
            const uint64_t now = static_cast<uint64_t>(i) * 16'666ull;
            Panel a, b;
            charged.render(a, t, now);
            bare.render(b, t, now);
            CAPTURE(expired); CAPTURE(i);
            REQUIRE(diff(a, b, h0::sandgeom::PANEL_X, h0::sandgeom::PANEL_Y,
                         h0::sandgeom::PANEL_W, h0::sandgeom::PANEL_H, true) == 0);
        }
    }
}

TEST_CASE("the panel is dark ink on white paper, in every state, with a border") {
    // ABSOLUTE, and that is the entire point of it.
    //
    // The two tests above compare a charged face against a bare one, so any
    // transform applied to BOTH faces cancels in the diff and they cannot see
    // it. Moving `invertSafeBox` from before the panel draw to after it does
    // exactly that: the whole readout comes out black with a white border, the
    // separator's colon and the "DONE" label are drawn black-on-black and
    // vanish -- and both opacity tests stay green, because `bare` was inverted
    // too. Measured: before this test existed that mutation left the whole suite
    // byte-identical -- 185 cases / 179 passed, 94180 assertions / 12 failed,
    // the same six names -- while the render really had changed.
    //
    // Nothing relative can catch it. So this test does not compare two renders
    // at all; it asserts what the panel IS.
    //
    // It also pins the border, which nothing else does. The border is required
    // -- `renderSand` no longer draws the housing, so without it the readout has
    // no edge at all -- but three tests that still describe the old, narrower
    // lintel geometry fail *because* of it, and deleting it takes the suite from
    // six failures to three. Making the tree greener by deleting a required
    // feature is a trap, and this is the tripwire on it. Those three are
    // retargeted at PANEL_* when the readout is enlarged; the border stays.
    onebit::init();
    for (int st = 0; st < 4; ++st) {
        h0::TimerModel t;
        t.setDuration(300 * SEC); // > 180 s -> the 2000-grain tier
        h0::TimerFace face;
        face.restart(t, 1u);
        uint64_t base = 0;
        std::string what = "idle";
        switch (st) {
            case 0: break;                                   // Idle
            case 1: t.start(0); what = "running"; break;     // Running
            case 2: t.start(0); t.pause(0); what = "paused"; break;
            default:                                          // Expired
                t.start(0);
                t.tick(301 * SEC);
                base = 301 * SEC;
                what = "expired";
                REQUIRE(t.isExpired());
                break;
        }
        CAPTURE(what);

        // The face is never ticked, so the sand stays at its charged layout and
        // there is genuinely sand behind the readout in all four states. Without
        // this the assertions below are about a panel floating over nothing.
        const int behindReadout = cellsInRect(face.sand(),
                                              h0::sandgeom::LINTEL_CX0, h0::sandgeom::LINTEL_CY0,
                                              h0::sandgeom::LINTEL_CX1, h0::sandgeom::LINTEL_CY1);
        CAPTURE(behindReadout);
        REQUIRE(behindReadout > 300);

        // A few frames apart, so a mid-flap phase is covered as well as a
        // settled one -- the widget writes WHITE to occlude a falling card, and
        // that write lands inside the panel.
        for (int i = 0; i < 4; ++i) {
            const uint64_t now = base + static_cast<uint64_t>(i) * 8u * 16'666ull;
            CAPTURE(i);
            Panel fb;
            face.render(fb, t, now);

            constexpr int16_t PX = h0::sandgeom::PANEL_X, PY = h0::sandgeom::PANEL_Y;
            constexpr int16_t PW = h0::sandgeom::PANEL_W, PH = h0::sandgeom::PANEL_H;

            // 1. The border exists, all the way round. Deleting `drawRect`
            //    makes this zero; inverting after the panel draw does too.
            int borderInk = 0;
            for (int16_t x = PX; x < PX + PW; ++x) {
                if (fb.getPixel(x, PY) == BLACK) ++borderInk;
                if (fb.getPixel(x, static_cast<int16_t>(PY + PH - 1)) == BLACK) ++borderInk;
            }
            for (int16_t y = PY; y < PY + PH; ++y) {
                if (fb.getPixel(PX, y) == BLACK) ++borderInk;
                if (fb.getPixel(static_cast<int16_t>(PX + PW - 1), y) == BLACK) ++borderInk;
            }
            const int borderPx = 2 * PW + 2 * PH;
            CAPTURE(borderInk);
            CHECK(borderInk == borderPx);

            // 2. The ring immediately inside the border is paper. This is the
            //    weakest possible statement of "the panel is white", so it
            //    survives the board growing to fill the panel, and it is still
            //    enough to catch a panel that came out black.
            int ringPaper = 0;
            for (int16_t x = static_cast<int16_t>(PX + 1); x < PX + PW - 1; ++x) {
                if (fb.getPixel(x, static_cast<int16_t>(PY + 1)) == WHITE) ++ringPaper;
                if (fb.getPixel(x, static_cast<int16_t>(PY + PH - 2)) == WHITE) ++ringPaper;
            }
            for (int16_t y = static_cast<int16_t>(PY + 1); y < PY + PH - 1; ++y) {
                if (fb.getPixel(static_cast<int16_t>(PX + 1), y) == WHITE) ++ringPaper;
                if (fb.getPixel(static_cast<int16_t>(PX + PW - 2), y) == WHITE) ++ringPaper;
            }
            const int ringPx = 2 * (PW - 2) + 2 * (PH - 2);
            CAPTURE(ringPaper);
            CHECK(ringPaper == ringPx);

            // 3. The interior is a readout, not a slab: some ink, nowhere near
            //    all of it. Measured on this fixture the panel interior carries
            //    1,038-1,147 ink px of 16,200 (~7%); with the invert moved after
            //    the panel it carries all 16,200. Both bounds are far from the
            //    real figure, so this moves for defects, not for glyph choices.
            const int interior = inkInRect(fb, static_cast<int16_t>(PX + 1),
                                           static_cast<int16_t>(PY + 1),
                                           static_cast<int16_t>(PW - 2),
                                           static_cast<int16_t>(PH - 2));
            const int area = (PW - 2) * (PH - 2);
            CAPTURE(interior);
            CAPTURE(area);
            CHECK(interior > 200);        // the readout drew something
            CHECK(interior < area / 4);   // and it is ink on paper, not paper on ink
        }
    }
}

TEST_CASE("a flap cell inverts exactly when sand is behind it") {
    // Each cell is solid: a covered cell is the exact COMPLEMENT of the
    // uncovered rendering, never a blend, so the contrast is total at every sand
    // density.
    //
    // Note what the legs below have to do to produce a covered cell -- they tilt
    // the device. That is not an artifact of the fixture: upright, coverage
    // peaks at 46% of a cell and no cell ever inverts. This mechanism is for
    // handling, not for reading the fill level. See the header.
    //
    // Three legs, because one is not enough to say anything:
    //
    //   S   the sand is nowhere near the board -- the resting case, and the one
    //       the goldens capture. Nothing may invert.
    //   NW  the sand is driven against the ceiling and to the LEFT, so the left
    //       of the board is covered and the right is not.
    //   NE  the mirror image.
    //
    // The last two are what make this a test of PER-CELL polarity rather than
    // of one global flag: the covered set is different in each, and in both it
    // is a strict subset. A single leg with every cell covered would pass
    // against an implementation that inverted the whole panel.
    onebit::init();

    const h0::Gravity dirs[3] = {h0::Gravity::S, h0::Gravity::NW, h0::Gravity::NE};
    const char* names[3] = {"S", "NW", "NE"};
    bool coveredSet[3][5] = {};
    int nCovered[3] = {0, 0, 0};

    for (int leg = 0; leg < 3; ++leg) {
        h0::TimerModel t;
        t.setDuration(300 * SEC); // > 180 s -> the 2000-grain tier
        t.start(0);

        h0::TimerFace charged;
        charged.restart(t, 11u);
        charged.setGravity(dirs[leg]);
        h0::TimerFace bare; // vessel never begun: no sand, no walls

        // Long enough for the tilt to settle the sand where it is going; the
        // configurations above are stable from about 200 ticks to 3000.
        uint64_t now = 0;
        runTo(charged, t, now, 7 * SEC);

        // Both faces render for the FIRST time here, so both snap rather than
        // cascade and the board content is identical. The only thing that can
        // differ inside a cell is its polarity.
        Panel a, b;
        charged.render(a, t, now);
        bare.render(b, t, now);

        for (int c = 0; c < 5; ++c) {
            const h0::Rect16 r = charged.cellRect(c);
            const int pct = coveragePct(charged.sand(), r);
            coveredSet[leg][c] = charged.cellCovered(c);
            CAPTURE(names[leg]);
            CAPTURE(c);
            CAPTURE(pct);
            if (charged.cellCovered(c)) {
                ++nCovered[leg];
                // The latch starts false and this is the first frame, so
                // "covered" means exactly "crossed the upper threshold".
                CHECK(pct > 60);
                CHECK(complementIn(a, b, r.x, r.y, r.w, r.h) == 0); // every pixel flipped
            } else {
                CHECK(pct <= 60);
                CHECK(diff(a, b, r.x, r.y, r.w, r.h, true) == 0); // every pixel equal
            }
        }
    }

    // Both branches were exercised, or the loop above proved only one of them.
    CAPTURE(nCovered[0]);
    CAPTURE(nCovered[1]);
    CAPTURE(nCovered[2]);
    CHECK(nCovered[0] == 0);              // upright, the board is above the sand
    CHECK(nCovered[1] > 0);
    CHECK(nCovered[1] < 5);
    CHECK(nCovered[2] > 0);
    CHECK(nCovered[2] < 5);

    // ...and the two tilts covered DIFFERENT cells. This is the assertion that
    // a single global polarity flag cannot pass.
    bool differ = false;
    for (int c = 0; c < 5; ++c) differ = differ || (coveredSet[1][c] != coveredSet[2][c]);
    CHECK(differ);
}

TEST_CASE("every flap cell carries both colours, whatever is behind it") {
    // The black-on-black failure this whole design risks, stated directly: a
    // cell that is uniformly one colour is unreadable, regardless of why.
    //
    // Both legs matter. Upright, no cell inverts and this is the old guarantee
    // restated against the composited panel. Packed against the ceiling, cells
    // invert and the same bound has to hold of the complement -- which is where
    // an inversion that flipped the glyph but not its background, or the
    // background but not the glyph, would show up as a solid block.
    //
    // MOST of the board inverts on the second leg, not all of it, and that is
    // a measured property of the sand rather than a hedge. Sand driven at the
    // ceiling settles into a MOUND, not a slab: at the 2000-grain top tier the
    // chamber is a little over half full, so the heap spans the full width at
    // the ceiling and tapers away below it. The 2x board is wide enough to
    // reach past the mound's shoulders -- the three middle cells sit under
    // 82-100% coverage and the two outer ones under 28-31%. At 1x the board was
    // 105 px and sat entirely inside the cone, which is why this leg used to
    // bury all five.
    constexpr int kBuried = 3;
    onebit::init();
    for (int leg = 0; leg < 2; ++leg) {
        h0::TimerModel t;
        t.setDuration(300 * SEC);
        t.start(0);
        h0::TimerFace face;
        face.restart(t, 9u);
        face.setGravity(leg ? h0::Gravity::N : h0::Gravity::S);

        uint64_t now = 0;
        bool sawInverted = false, sawUpright = false;
        for (int step = 0; step < 12; ++step) {
            runTo(face, t, now, now + 20 * SEC);
            Panel fb;
            face.render(fb, t, now);
            for (int c = 0; c < 5; ++c) {
                const h0::Rect16 r = face.cellRect(c);
                const int ink = inkInRect(fb, r.x, r.y, r.w, r.h);
                const int total = r.w * r.h;
                if (face.cellCovered(c)) sawInverted = true; else sawUpright = true;
                CAPTURE(leg);
                CAPTURE(step);
                CAPTURE(c);
                CAPTURE(ink);
                CAPTURE(total);
                CHECK(ink > total / 20);         // not blank
                CHECK(ink < total - total / 20); // not solid
            }
        }
        // LOAD-BEARING. Without it the inverted leg could quietly stop
        // inverting -- the sand configuration is the test fixture here, and a
        // fixture that stops producing the state under test takes the assertion
        // with it while staying green.
        //
        // Counted rather than merely observed, so "most of the board inverts"
        // is a number the fixture has to keep hitting and not a single stray
        // cell somewhere in 60 samples.
        int buried = 0;
        for (int c = 0; c < 5; ++c) if (face.cellCovered(c)) ++buried;
        CAPTURE(leg);
        CAPTURE(buried);
        CHECK(sawInverted == (leg == 1));
        CHECK(sawUpright); // both legs still have upright cells to bound
        CHECK(buried == (leg ? kBuried : 0));
    }
}

TEST_CASE("a cell's polarity is latched, and holds inside the hysteresis band") {
    // The band is the difference between a gauge and a fault. A grain jitters
    // at the sand's surface, so a cell sitting there gains and loses a few
    // every tick; under a bare threshold that jitter is a polarity flip on
    // consecutive frames, and the readout strobes. Two thresholds turn the
    // noisy measurement into a latched state.
    //
    // The invariant, checked at every single frame: A CELL ONLY EVER CHANGES
    // POLARITY WITH ITS COVERAGE OUTSIDE THE BAND -- above 60% to invert,
    // below 40% to revert. Inside 40..60 the latch holds whatever it already
    // was, which is exactly what a bare threshold cannot do.
    //
    // This is airtight rather than sampled, because `render()` is the only
    // thing that moves the latch and this renders every tick: if the latch
    // moved between two observations, it moved on the frame being observed.
    // That is why `runTo` is not used here.
    //
    // The fixture: tip NW to bury the board, then tip W, which drains it
    // slowly enough that cells cross the band on the way down and linger
    // there. Constructing a cell that sits at 40..60 was the open question --
    // it needs a retreating surface rather than a settled pile, and no static
    // tilt produces one.
    onebit::init();

    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);
    h0::TimerFace face;
    face.restart(t, 9u);
    face.setGravity(h0::Gravity::NW);

    Panel fb;
    uint64_t now = 0;
    bool was[5] = {}; // the latch starts false, which is the state before frame 0
    int heldOn = 0, heldOff = 0, changes = 0;

    for (int tick = 0; tick < 700; ++tick) {
        now += 33'333ull;
        face.tick(t, now);
        if (tick == 209) face.setGravity(h0::Gravity::W); // let it fall away again
        face.render(fb, t, now);

        for (int c = 0; c < 5; ++c) {
            // Measured from the grid, independently of what the face reports.
            const int pct = coveragePct(face.sand(), face.cellRect(c));
            const bool covered = face.cellCovered(c);
            if (covered != was[c]) {
                ++changes;
                CAPTURE(tick);
                CAPTURE(c);
                CAPTURE(pct);
                CAPTURE(covered);
                if (covered) CHECK(pct > 60);
                else CHECK(pct < 40);
            } else if (pct >= 40 && pct <= 60) {
                if (covered) ++heldOn;  // stayed inverted below the ON threshold
                else ++heldOff;         // stayed upright above the OFF threshold
            }
            was[c] = covered;
        }
    }

    // FIXTURE INTEGRITY, and it is the whole test. The invariant above is
    // vacuous unless the run actually spends frames inside the band in BOTH
    // latch states -- a cell holding ON below 60 is what a bare threshold
    // would have reverted, and a cell holding OFF above 40 is what a bare
    // threshold would have inverted. Measured: 171 and 21.
    CAPTURE(heldOn);
    CAPTURE(heldOff);
    CAPTURE(changes);
    CHECK(heldOn > 50);
    CHECK(heldOff > 5);
    CHECK(changes >= 4);
}

namespace {

/// An allocator that refuses one exact allocation size and passes everything
/// else through to malloc, so a single named buffer can be made to fail while
/// the rest of the process allocates normally.
size_t g_denySize = 0;
int g_denied = 0;

void* denyingAlloc(size_t n, void*) {
    if (g_denySize != 0 && n == g_denySize) {
        ++g_denied;
        return nullptr;
    }
    return std::malloc(n);
}
void denyingFree(void* p, void*) { std::free(p); }

/// Installs the denying allocator for a scope and restores the default one on
/// the way out. A scope guard rather than two statements because the allocator
/// is process-global: leaking it out of this test would let it deny an
/// allocation in some later, unrelated one, and doctest's fatal assertions
/// leave by throwing.
struct DenyAlloc {
    explicit DenyAlloc(size_t size) {
        g_denySize = size;
        g_denied = 0;
        onebit::init(onebit::Allocator{denyingAlloc, denyingFree, nullptr});
    }
    ~DenyAlloc() {
        g_denySize = 0;
        onebit::init();
    }
};

} // namespace

TEST_CASE("a readout whose scratch buffer could not be allocated goes blank, not black") {
    // The black-on-black failure, arrived at from the side the sand cannot
    // reach. `onebit::Framebuffer` takes its storage from the heap and the
    // constructor has no error path: a failed allocation leaves an object that
    // looks fine, reports it only through `isValid()`, and silently discards
    // every draw. A covered cell is stamped in two steps -- fill the rect
    // BLACK, then Xor the board over it -- and only the second step reads the
    // scratch. Swallow that blit and the cell is a solid black slab.
    //
    // Unreachable on the host by accident, since malloc does not fail here, so
    // the failure is injected: deny allocations of exactly the scratch's size
    // and nothing else. 2,116 bytes on a device with a few hundred KB is a
    // small ask that will nearly always succeed, and "nearly always" is not
    // what the rest of this file spends its assertions establishing.
    using Scratch = onebit::Framebuffer<h0::sandgeom::PANEL_W, h0::sandgeom::PANEL_H>;

    onebit::init();
    int covered = 0, denied = 0;
    {
        DenyAlloc deny(Scratch::BUFFER_SIZE);

        h0::TimerModel t;
        t.setDuration(300 * SEC);
        t.start(0);
        h0::TimerFace face; // its scratch is the allocation being refused
        face.restart(t, 11u);
        face.setGravity(h0::Gravity::N); // bury the board: the middle cells want inverting

        Panel fb; // 8,400 bytes -- a different size, so this one succeeds
        uint64_t now = 0;
        runTo(face, t, now, 7 * SEC);
        face.render(fb, t, now);

        for (int c = 0; c < 5; ++c) {
            const h0::Rect16 r = face.cellRect(c);
            const int ink = inkInRect(fb, r.x, r.y, r.w, r.h);
            const int total = r.w * r.h;
            if (face.cellCovered(c)) ++covered;
            CAPTURE(c);
            CAPTURE(ink);
            CAPTURE(total);
            CHECK(ink < total); // NOT a solid black slab -- the whole point
            CHECK(ink == 0);    // ...and specifically blank paper inside the panel
        }
        denied = g_denied;
    }

    // FIXTURE INTEGRITY, both halves, because either one failing quietly makes
    // the assertions above pass against anything. The scratch has to have been
    // refused exactly once -- not zero times, and not more, which would mean
    // the size collided with some other buffer -- and cells have to be on the
    // COVERED branch, since the uncovered branch never fills black and could
    // not produce a slab in the first place.
    //
    // THREE cells, not five. Sand driven at the ceiling settles into a mound
    // rather than a slab, and the 2x board is wide enough to reach past its
    // shoulders -- see "every flap cell carries both colours" for the measured
    // profile. Three covered cells is three chances to paint a black slab, and
    // the two uncovered ones are the Copy branch, which is checked here too:
    // every one of the five is asserted blank above.
    CAPTURE(denied);
    CAPTURE(covered);
    CHECK(denied == 1);
    CHECK(covered == 3);
}

// The two guarantees that used to live here are gone, and both were deleted
// deliberately rather than allowed to rot:
//
//   * "the lintel interior is empty in the wall grid, so sand cannot enter it"
//     is now ACTIVELY FALSE. The lintel is not a wall; sand filling that region
//     is the change, not a defect. Its successor is the pair of opacity tests
//     above, which assert the panel's pixels are what they would be with no
//     sand at all -- the same legibility claim, bought with compositing rather
//     than with physics.
//
//   * "a sand-only render leaves the card white, with no help from the face"
//     is false at 2000 grains for exactly the same reason. Worth recording that
//     its other two legs were ALREADY VACUOUS before this change: the 900-grain
//     heap tops out around row 32 and the lintel was rows 1-26, so no grain ever
//     came within a cell of it at 400 or 900. It was one live assertion and two
//     that could not fail.

TEST_CASE("what the card shows does not depend on where the sand is") {
    // RESTATED for per-cell polarity, and the restatement is the point.
    //
    // The old form asserted the card was pixel-identical whatever the sand was
    // doing. That is false on purpose now: a cell whose sand is behind it is
    // stamped inverted, so two runs showing the same time at different sand
    // levels differ by thousands of pixels inside the readout. Measured on this
    // fixture at the moment the old form was retired: 4,692.
    //
    // What survives is weaker in form and stronger in content. The sand cannot
    // change WHAT the readout says, or WHERE anything on it sits -- only which
    // way up a cell is drawn. So: equal characters, and every cell either
    // pixel-identical or the exact complement. Never a blend, which is what a
    // leak of sand into the panel would actually look like.
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);

    h0::TimerFace a, b;
    a.restart(t, 11u);
    b.restart(t, 11u);
    a.setGravity(h0::Gravity::S); // sand banks in the shoulders and drains
    b.setGravity(h0::Gravity::N); // sand packs up against the soffit

    uint64_t na = 0, nb = 0;
    runTo(a, t, na, 60 * SEC);
    runTo(b, t, nb, 60 * SEC);

    Panel fa, fb2;
    a.render(fa, t, 60 * SEC);
    b.render(fb2, t, 60 * SEC);

    // The content, which is sand-independent.
    for (int c = 0; c < h0::TimerFace::kCells; ++c) {
        CAPTURE(c);
        CHECK(a.boardChar(static_cast<int16_t>(c)) == b.boardChar(static_cast<int16_t>(c)));
    }

    // The presentation, which is sand-dependent but only in one specific way.
    int identical = 0, complementary = 0;
    for (int c = 0; c < h0::TimerFace::kCells; ++c) {
        const h0::Rect16 r = a.cellRect(c);
        const int same = diff(fa, fb2, r.x, r.y, r.w, r.h, true);
        const int flipped = complementIn(fa, fb2, r.x, r.y, r.w, r.h);
        CAPTURE(c);
        CAPTURE(same);
        CAPTURE(flipped);
        CHECK((same == 0 || flipped == 0));
        if (same == 0) ++identical;
        if (flipped == 0) ++complementary;
    }

    // FIXTURE INTEGRITY. `same == 0` for all five cells satisfies the loop above
    // perfectly well, and that is what a dead polarity latch looks like -- so the
    // disjunction has to be shown taking BOTH branches or it is only asserting
    // the half that was going to hold anyway. Measured on this fixture: cells 0
    // and 4 identical, cells 1-3 exactly complementary, 2,040 px each (34 x 60).
    CAPTURE(identical);
    CAPTURE(complementary);
    CHECK(identical == 2);
    CHECK(complementary == 3);

    // FIXTURE INTEGRITY, and without it the loop above is a statement about
    // nothing: two identical frames satisfy `same == 0` for every cell. The sand
    // has to actually have differed BEHIND the panel.
    const int behind = cellDiff(a.sand(), b.sand(), h0::sandgeom::PANEL_CX0,
                                h0::sandgeom::PANEL_CY0, h0::sandgeom::PANEL_CX1,
                                h0::sandgeom::PANEL_CY1);
    CAPTURE(behind);
    CHECK(behind > 100);

    // ...and visibly so outside it too, which is the leg the old form already
    // had and which still says the two fixtures were driven apart at all.
    CHECK(diff(fa, fb2, h0::sandgeom::PANEL_X, h0::sandgeom::PANEL_Y, h0::sandgeom::PANEL_W,
               h0::sandgeom::PANEL_H, false) > 500);
}

TEST_CASE("the composite is sand everywhere outside the housing") {
    // Catches a stray fb.clear() inside the readout path, which would blank the
    // sand and leave a board floating on white.
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);

    h0::TimerFace face;
    face.restart(t, 5u);
    uint64_t now = 0;
    runTo(face, t, now, 40 * SEC);

    Panel composed;
    face.render(composed, t, 40 * SEC);

    // The reference frame is the vessel and nothing else. It used to have the
    // lintel outline drawn into it, because the face's own wall grid carried it;
    // that grid no longer does, so drawing it here would be reproducing a
    // structure nothing renders any more.
    Panel bare;
    bare.clear(WHITE);
    const h0::SandGrid drawn = h0::makeVessel(h0::SandVessel::kHoleHalf);
    h0::renderSand(bare, face.sand(), drawn);

    // Compare outside the PANEL entirely -- the housing is the panel now. Inside
    // it the composite has an opaque readout and the sand-only frame has sand,
    // and that difference is the whole design rather than a defect.
    CHECK(diff(composed, bare, h0::sandgeom::PANEL_X, h0::sandgeom::PANEL_Y,
               h0::sandgeom::PANEL_W, h0::sandgeom::PANEL_H, false) == 0);
}

// ------------------------------------------------------------- the drain --

TEST_CASE("a flip in progress never erases sand outside the housing") {
    // The library's split-flap models a hinged card that falls through 180
    // degrees, and it CLEARS ITS DESTINATION to WHITE to occlude what it passes
    // over -- the one place the widget writes paper rather than ink. This face
    // composites that widget over a sand simulation, so an erase that reached
    // past the card's own cell would punch a white hole in the sand once per
    // flap. It is clipped today; this pins it, because it is a property of code
    // this repo does not own and cannot see change.
    //
    // The board is drawn into a panel-sized scratch now, so that white clear
    // cannot physically reach the frame -- but the stamp that follows it can,
    // and this is what says the stamp stays inside the housing. Retargeted from
    // the lintel to the panel with the rest of them.
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);
    h0::TimerFace face;
    face.restart(t, 1234u);

    uint64_t now = 0;
    runTo(face, t, now, 100 * SEC);

    // Sweep a whole second at 60 Hz so every phase of a flap is covered, not
    // just the settled frames the goldens happen to capture.
    for (int i = 0; i < 60; ++i) {
        const uint64_t nw = now + static_cast<uint64_t>(i) * 16'666ull;
        Panel composed;
        face.render(composed, t, nw);

        Panel bare;
        bare.clear(WHITE);
        const h0::SandGrid drawn = h0::makeVessel(h0::SandVessel::kHoleHalf);
        h0::renderSand(bare, face.sand(), drawn);

        CAPTURE(i);
        REQUIRE(diff(composed, bare, h0::sandgeom::PANEL_X, h0::sandgeom::PANEL_Y,
                     h0::sandgeom::PANEL_W, h0::sandgeom::PANEL_H, false) == 0);
    }
}

TEST_CASE("sand is conserved for the whole run") {
    for (int grains : {400, 900, 2000}) {
        h0::SandVessel v;
        v.begin();
        v.reset(3u, grains);
        const int charged = v.charge();
        CHECK(charged > 0);
        for (int i = 0; i < 800; ++i) {
            v.tick(1.0f - static_cast<float>(i) / 800.0f);
            CAPTURE(grains);
            CAPTURE(i);
            REQUIRE(v.sand().count() == charged);
        }
    }
}

TEST_CASE("the upper chamber actually empties") {
    // On a FLAT floor sand is stable at zero slope, so grains far from the hole
    // never slide in on their own -- measured, 33-86% strand permanently. The
    // centreline attractor is what makes the vessel drainable at all, and the
    // lintel is welded to the ceiling so that it cannot reintroduce the problem
    // by giving grains somewhere to stack that is not reachable.
    //
    // Measured in GRID space, not pixels: the lintel outline adds a fixed 436 px
    // to the upper band, so a pixel count can no longer tell stranded sand from
    // structure, and the old "< 40 stray pixels" would degrade silently.
    for (int grains : {400, 900, 2000}) {
        h0::SandVessel v;
        v.begin();
        v.reset(99u, grains);
        for (int i = 0; i < 4000; ++i) v.tick(0.0f); // drain flat out
        for (int i = 0; i < 120; ++i) v.tick(0.0f);  // and settle
        CAPTURE(grains);
        CHECK(v.sand().countRows(0, h0::sandgeom::FLOOR_ROW - 1) == 0);
    }
}

TEST_CASE("short timers never reach the rows the lintel used to wall off") {
    // Same measurement as before this change, opposite rationale -- and the old
    // one would now mislead, so it is restated rather than left to rot. It read
    // "the lintel costs the sand nothing below three minutes", an argument about
    // an obstacle. There is no obstacle: those rows are ordinary chamber now and
    // a grain is free to sit in any of them.
    //
    // What the same numbers say instead is why the timer goldens moved only in
    // the readout. The golden fixture is 120 s -> 900 grains, and at 900 the
    // heap is not tall enough to reach these rows -- so taking the wall out
    // changed the geometry of a region no grain was going to occupy anyway, and
    // the sand pixels came out bit-identical. That is a fact about the CHARGE,
    // not about the housing, which is exactly why it survives the housing
    // ceasing to exist.
    for (int grains : {400, 900}) {
        h0::SandVessel v;
        v.begin();
        v.reset(42u, grains);
        CAPTURE(grains);
        CHECK(v.charge() == grains);
        CHECK(v.sand().countRows(h0::sandgeom::LINTEL_CY0, h0::sandgeom::LINTEL_CY1) == 0);
    }

    // The top tier does reach them -- and now that they are open, it has 1,722
    // grains of headroom rather than 188. See the capacity test below.
    h0::SandVessel big;
    big.begin();
    big.reset(42u, 2000);
    CHECK(big.charge() >= 2000);
}

TEST_CASE("sand fills behind the panel and can still leave") {
    // THE REASON THE WHOLE CHANGE EXISTS, and until now nothing in the suite
    // pinned it.
    //
    // A 2x readout kept as a wall places 1,502 grains against a 2,000-grain top
    // tier -- BELOW the tier, so the longest timers would have been silently
    // truncated and the only symptom would have been an hourglass that ran out
    // of sand early. Taking the wall out moves capacity from 2,188 to 3,722.
    //
    // Two halves, and both have to hold. Capacity that cannot drain is worse
    // than capacity that was never there: the timer would finish with a heap
    // still hanging under the ceiling, in the one region the user cannot see
    // into because the readout is over it.
    onebit::init();
    for (int ask : {2000, 3000}) {
        // 2000 is the live top tier. 3000 is headroom the tier does not use
        // yet -- asserted anyway, because it is the margin the whole change was
        // bought for, and a wall creeping back into that region would take it
        // away while leaving the tier itself passing.
        CAPTURE(ask);
        h0::SandVessel v;
        v.begin();
        v.reset(4u, ask);
        CHECK(v.charge() >= ask);

        // LOAD-BEARING, and measured over the EX-LINTEL rect rather than the
        // panel rect. The panel is mostly ordinary chamber and always was, so a
        // `panel > 100` guard is satisfied with the wall in place and rules out
        // nothing; these rows are the ones that were exactly zero by
        // construction until this change. Measured at 2000 grains: 1,745 cells
        // under the panel, of which 531 are here.
        const int behind =
            cellsInRect(v.sand(), h0::sandgeom::LINTEL_CX0, h0::sandgeom::LINTEL_CY0,
                        h0::sandgeom::LINTEL_CX1, h0::sandgeom::LINTEL_CY1);
        CAPTURE(behind);
        REQUIRE(behind > 300);

        for (int i = 0; i < 4000; ++i) v.tick(0.0f); // gate wide open: drain flat out

        // Nothing stranded, counted two ways on purpose. Every charged grain
        // reaching the lower chamber would also be satisfied by a grain that
        // simply vanished; the upper chamber being empty is the leg that fails
        // if one hangs where the lintel used to be.
        CAPTURE(v.charge());
        CHECK(v.lowerCount() == v.charge());
        CHECK(v.sand().countRows(0, h0::sandgeom::FLOOR_ROW - 1) == 0);
    }
}

TEST_CASE("the drain is visible: the frame changes as the sand falls") {
    // Catches the tick call being lost, which yields a still life that every
    // static golden would happily pass.
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);

    h0::TimerFace face;
    face.restart(t, 1234u);
    uint64_t now = 0;

    runTo(face, t, now, 30 * SEC);
    Panel early;
    face.render(early, t, 30 * SEC);

    runTo(face, t, now, 95 * SEC);
    Panel late;
    face.render(late, t, 95 * SEC);

    CHECK(diff(early, late, CARD_X, CARD_Y, CARD_W, CARD_H, false) > 500);
}

TEST_CASE("a flip recharges the sand, not just the clock") {
    // TimerModel::reset() does not touch the duration, so watching the duration
    // missed a flip entirely: the clock ran from full while the sand stayed
    // where it had drained to, and the gate stayed shut for the whole next run.
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    t.start(0);

    h0::TimerFace face;
    face.restart(t, 8u);
    uint64_t now = 0;
    runTo(face, t, now, 60 * SEC);
    REQUIRE(face.sand().countRows(h0::sandgeom::FLOOR_ROW, h0::SandGrid::H - 1) > 0);

    t.reset(now); // the flip
    face.tick(t, now);

    CHECK(face.sand().countRows(h0::sandgeom::FLOOR_ROW, h0::SandGrid::H - 1) == 0);
    CHECK(face.sand().countRows(0, h0::sandgeom::FLOOR_ROW - 1) == face.charge());
}

// ------------------------------------------------------------ the readout --

TEST_CASE("the board settles within one second of every tick") {
    // The defect this guards: SplitFlapDisplay steps one character at a time,
    // forward only. With the library's default alphanumeric sequence a digit
    // DECREMENT -- what a countdown does every second -- costs 39 flaps, 5.19 s
    // at the default cadence. The board never lands, and what it shows instead
    // of digits is letters.
    h0::TimerFace face;
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);

    Panel fb;
    for (int sec = 0; sec < 12; ++sec) {
        for (int f = 0; f < 20; ++f) {
            const uint64_t now =
                static_cast<uint64_t>(sec) * SEC + static_cast<uint64_t>(f) * 50'000ull;
            face.render(fb, t, now);
        }
        char want[8];
        const uint32_t rem = t.remainingSeconds(static_cast<uint64_t>(sec) * SEC);
        std::snprintf(want, sizeof(want), "%02u:%02u", rem / 60, rem % 60);
        CAPTURE(sec);
        CAPTURE(want);
        for (int16_t c = 0; c < 5; ++c) {
            CAPTURE(c);
            CHECK(face.boardChar(c) == want[c]);
        }
    }
}

TEST_CASE("one real second drives exactly one flap on the seconds-units cell, "
          "then the board holds") {
    // The cadence property the fix is FOR: with every transition costing one
    // flap and ms_per_flap = 500, a real second can never need more than one
    // flap's animation on any cell. This pins that directly, rather than only
    // pinning the end state the way the test above does -- it is the
    // difference between "the board is right a second later" (already
    // covered) and "the board did not sit on the wrong digit, or bounce, or
    // keep moving after it landed" (not covered by a once-a-second sample).
    //
    // Duration picked so only the seconds-units cell moves at the first tick:
    // "00:15" -> "00:14" leaves minutes and the seconds-tens digit untouched,
    // so a stray flap anywhere else in that window would show up here too.
    h0::TimerModel t;
    t.setDuration(15 * SEC);
    t.start(0);
    h0::TimerFace face;

    Panel fb;
    face.render(fb, t, 0); // first-ever render snaps rather than cascading
    REQUIRE(face.boardChar(4) == '5');

    // Sweep at a real render cadence (~30 Hz, matching firmware/src/main.cpp)
    // rather than jumping, so this is the same calling pattern the board
    // actually sees -- a coarse jump exercises the separate one-second
    // catch-up cap in TimerFace::render, not the per-flap cadence this test
    // is about.
    constexpr uint64_t kFrame = 33'333ull;
    bool sawOld = false, changed = false;
    uint64_t changedAt = 0;
    for (uint64_t now = kFrame; now <= 1'600'000ull; now += kFrame) {
        face.render(fb, t, now);
        const char c = face.boardChar(4);
        CAPTURE(now);
        if (!changed) {
            if (c == '5') { sawOld = true; continue; }
            REQUIRE(c == '4'); // one flap, forward -- nothing else is reachable
            changed = true;
            changedAt = now;
        } else {
            CHECK(c == '4'); // landed, and it does not bounce afterwards
        }
        // The rest of the board never moves: this second's tick is entirely
        // the seconds-units cell's business.
        CHECK(face.boardChar(0) == '0');
        CHECK(face.boardChar(1) == '0');
        CHECK(face.boardChar(3) == '1');
    }
    CHECK(sawOld);
    CHECK(changed);

    // The card lands one whole flap after the tick that released it -- the
    // sequence is 500 ms falling, then 500 ms standing, which is the cadence
    // that was asked for. The window is loose by a frame either side because
    // the flap advances on an accumulator crossing, which lands wherever the
    // render cadence divides it.
    CHECK(changedAt >= 1'450'000ull);
    CHECK(changedAt <= 1'600'000ull);

    // One flap is at most ms_per_flap of animation, so the cell is settled
    // before the second is up. Confirm the RENDERED FRAME, not just the
    // character, is pixel-static across the rest of THIS second -- that is
    // "the board holds", not merely "the glyph index stopped changing".
    // Sampled short of the 2.0 s tick, which legitimately starts the next flap.
    Panel a, b;
    face.render(a, t, 1'700'000ull);
    face.render(b, t, 1'950'000ull);
    CHECK(diff(a, b, CARD_X, CARD_Y, CARD_W, CARD_H, true) == 0);
}

TEST_CASE("the flap stays in motion across consecutive frames, not just one") {
    // The gap every other flap test left open. `boardChar` reports the cell's
    // CURRENT index, which reaches its target on the frame the flip STARTS --
    // so a board that completes its whole 500 ms flap inside one frame reads
    // identically to one that animates properly: right glyph, right end state,
    // matching goldens. Only the frames BETWEEN the ticks distinguish them, and
    // nothing sampled them. On hardware that shows up as no animation at all.
    //
    // 500 ms per flap at ~30 Hz is about 15 frames of motion; require at least
    // 10 consecutive, which is loose enough to survive cadence tuning and tight
    // enough that a one-frame snap cannot pass.
    h0::TimerModel t;
    t.setDuration(15 * SEC);
    t.start(0);
    h0::TimerFace face;

    Panel fb;
    face.render(fb, t, 0); // first-ever render snaps rather than cascading
    REQUIRE(face.boardChar(4) == '5');
    REQUIRE_FALSE(face.boardFlipping()); // settled, so the count below starts clean

    // The window is the BOARD's own box, taken from the face, not the old
    // lintel interior. The board is 170 px wide now and the lintel interior was
    // 114, so CARD_* clips the seconds cells -- the only ones that flap once a
    // second -- down to a 7 px strip, and the motion count collapsed from 15
    // frames to 5 while the animation itself was untouched. A window that
    // measures somewhere other than where the thing moves is not a looser test,
    // it is a different one.
    const h0::Rect16 first = face.cellRect(0);
    const h0::Rect16 last = face.cellRect(4);
    const int16_t BOARD_X = first.x;
    const int16_t BOARD_Y = first.y;
    const int16_t BOARD_W = static_cast<int16_t>(last.x + last.w - first.x);
    const int16_t BOARD_H = first.h;

    constexpr uint64_t kFrame = 33'333ull;
    int flipFrames = 0, best = 0, movedFrames = 0;
    Panel prev, cur;
    face.render(prev, t, kFrame);
    for (uint64_t now = 2 * kFrame; now <= 1'900'000ull; now += kFrame) {
        face.render(cur, t, now);
        if (face.boardFlipping()) {
            ++flipFrames;
            if (flipFrames > best) best = flipFrames;
            // Mid-flip frames must also LOOK different from their predecessor.
            // `isFlipping` is the board's own bookkeeping; this is the pixels a
            // person would actually see move.
            if (diff(prev, cur, BOARD_X, BOARD_Y, BOARD_W, BOARD_H, true) > 0) ++movedFrames;
        } else {
            flipFrames = 0;
        }
        std::memcpy(prev.buffer(), cur.buffer(), cur.bufferSize());
    }
    CAPTURE(best);
    CAPTURE(movedFrames);
    CHECK(best >= 10);
    CHECK(movedFrames >= 10);
}

TEST_CASE("a discontinuous jump snaps immediately, not mid-cascade") {
    // Companion to the test above: that one pins the TICK path (animate).
    // This one pins the JUMP path (snap) -- a reset, a resume, an expiry, or
    // a skipped second must never be handed to the animator, because the
    // target it would be chasing keeps moving as the clock counts down
    // underneath a still-cascading board. Before the tick-vs-jump fix,
    // render() could not tell "one second passed" from "the model jumped",
    // so it fed BOTH the same capped delta -- fine at the old 110 ms/flap
    // (nine flaps of budget), but at 500 ms/flap a jump needing more than two
    // flaps landed mid-cascade on the very next frame. Checked by hand: with
    // the tick-vs-jump fix reverted (ordinaryTick always true, i.e. every
    // frame uses the animate path) this test fails -- the board still reads
    // "00:5X" a frame after the reset rather than "01:00".
    h0::TimerModel t;
    t.setDuration(60 * SEC); // "01:00"
    t.start(0);
    h0::TimerFace face;
    Panel fb;

    uint64_t now = 0;
    face.render(fb, t, now); // first-ever render snaps rather than cascading
    // Run it down at a real cadence so the board is genuinely settled
    // mid-countdown, not merely freshly snapped.
    for (int i = 0; i < 90; ++i) { now += 33'333ull; face.render(fb, t, now); }

    char before[8];
    const uint32_t remBefore = t.remainingSeconds(now);
    std::snprintf(before, sizeof(before), "%02u:%02u", remBefore / 60, remBefore % 60);
    for (int16_t c = 0; c < 5; ++c) REQUIRE(face.boardChar(c) == before[c]);

    // The flip: back to full, discontinuously. "00:5X" -> "01:00" needs up to
    // nine flaps on the minutes-units cell alone (0 -> 1 is the long way
    // around a descending sequence) -- far more than the two flaps a single
    // capped render() call can afford at 500 ms/flap, so a cascade would be
    // impossible to miss here.
    t.reset(now);
    now += 33'333ull; // the very next frame, same as the real render loop
    face.render(fb, t, now);

    char want[8];
    const uint32_t rem = t.remainingSeconds(now);
    std::snprintf(want, sizeof(want), "%02u:%02u", rem / 60, rem % 60);
    REQUIRE(std::strcmp(want, "01:00") == 0); // the reset landed where expected
    for (int16_t c = 0; c < 5; ++c) {
        CAPTURE(c);
        CHECK(face.boardChar(c) == want[c]); // right there -- not mid-cascade
    }
}

TEST_CASE("the board spells the time across the whole representable range") {
    // The dial is capped at 99:59, so this is exact everywhere -- the clamp in
    // formatMMSS is unreachable rather than merely unlikely. This is the whole
    // of what the deleted digits face was silently providing.
    Panel fb;
    for (uint32_t s = 0; s <= 5999; ++s) {
        h0::TimerModel t;
        t.setDuration(static_cast<uint64_t>(s) * SEC);
        t.start(0);
        // A fresh face each time: the first render snaps the board to its
        // target rather than cascading, which is exactly the behaviour being
        // asserted. Reusing one face with a frozen clock gives it no time to
        // flap and tests nothing but the previous iteration's leftovers.
        h0::TimerFace face;
        face.render(fb, t, 0);
        char want[8];
        std::snprintf(want, sizeof(want), "%02u:%02u", s / 60, s % 60);
        bool ok = true;
        for (int16_t c = 0; c < 5; ++c) ok = ok && face.boardChar(c) == want[c];
        if (!ok) { CAPTURE(s); CAPTURE(want); REQUIRE(ok); }
    }
}

TEST_CASE("the readout sits inside its housing") {
    // Pins the board's ink box -- and, less obviously, catches sand leaking into
    // the panel. That second job is the reason this test is scanned the way it
    // is, and it survived the rewrite for the bigger board on purpose.
    //
    // The window is the panel INTERIOR, which is a good deal bigger than the
    // board: 180 x 90 against 170 x 60, so there are 30 rows below the board and
    // 5 columns each side of it that must contain no ink at all in the Running
    // state. The bounding box of whatever ink IS there therefore has to come out
    // exactly the board's rect. Anything that puts a stray black pixel anywhere
    // else in the panel -- and a grain showing through is exactly that -- pushes
    // an edge of the box out and one of the four equalities below fails.
    //
    // Two ways to lose that, both of which look like tidying up:
    //
    //   * narrowing the window to the board's own rect. The box would then be
    //     the window by construction and could not move.
    //   * relaxing the equalities to a containment check (box inside panel).
    //     Sand fills the panel region from the top down, so leaked grains land
    //     INSIDE the window and containment stays true while the readout goes
    //     black-on-black.
    //
    // The fixture is chosen so the catcher is armed rather than theoretical:
    // 300 s is the 2000-grain tier and the face is rendered at charge, when
    // there really is sand in the rows the board covers. The guard below says
    // so rather than assuming it.
    onebit::init();
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);
    h0::TimerFace face;
    face.restart(t, 1u);

    const int behind = cellsInRect(face.sand(), h0::sandgeom::PANEL_CX0, h0::sandgeom::PANEL_CY0,
                                   h0::sandgeom::PANEL_CX1, h0::sandgeom::PANEL_CY1);
    CAPTURE(behind);
    REQUIRE(behind > 100); // there is something for a leak to leak

    Panel fb;
    face.render(fb, t, 0);

    int16_t x0 = 240, y0 = 280, x1 = -1, y1 = -1;
    for (int16_t y = CARD_Y; y < CARD_Y + CARD_H; ++y)
        for (int16_t x = CARD_X; x < CARD_X + CARD_W; ++x)
            if (fb.getPixel(x, y) == BLACK) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    // Assert the MEASURED box, not a relation between the box and the window --
    // see above. `x1 >= 0` is separate because a readout that drew nothing at all
    // leaves the box unset, and an unset box must fail rather than pass vacuously.
    REQUIRE(x1 >= 0);
    CHECK(x0 == 35);  // kBoardX
    CHECK(y0 == 22);  // kBoardY
    CHECK(x1 == 204); // kBoardX + 5 * kCellW - 1, cell borders included
    CHECK(y1 == 81);  // kBoardY + kCellH - 1; Running, so no label band
}

// ------------------------------------------------------------ the frame --

TEST_CASE("the corner helper flags clipped ink and clears the safe box") {
    // This helper gates several faces, so a version that always returns zero
    // would silently disarm all of them. Pin both directions.
    Panel fb;
    fb.clear(WHITE);
    CHECK(inkInCorners(fb) == 0);

    fb.setPixel(2, 2, BLACK); // deep under the top-left arc
    CHECK(inkInCorners(fb) == 1);

    fb.clear(WHITE);
    fb.setPixel(h0::safe::X, h0::safe::Y, BLACK); // measured visible on hardware
    fb.setPixel(239 - h0::safe::INSET, 279 - h0::safe::INSET, BLACK);
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("nothing is drawn under the rounded corners, in any state") {
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    h0::TimerFace face;
    face.restart(t, 3u);

    Panel fb;
    face.render(fb, t, 0); // Idle
    CHECK(inkInCorners(fb) == 0);

    t.start(0);
    uint64_t now = 0;
    runTo(face, t, now, 40 * SEC);
    face.render(fb, t, 40 * SEC);
    CHECK(inkInCorners(fb) == 0);

    t.pause(40 * SEC);
    face.render(fb, t, 40 * SEC);
    CHECK(inkInCorners(fb) == 0);

    t.resume(40 * SEC);
    t.tick(200 * SEC);
    REQUIRE(t.isExpired());
    face.render(fb, t, 200 * SEC);
    // The expiry invert is scoped to the safe box precisely so this holds: a
    // whole-panel invert measures ~1,800 ink pixels under the clip.
    CHECK(inkInCorners(fb) == 0);
}

TEST_CASE("expiry is unmistakable") {
    // Absolutes rather than a ratio. The running frame's ink varies with the
    // charge and where the sand happens to be, so a ratio moves for reasons that
    // are not defects; the two populations are far enough apart to separate with
    // fixed bounds. Measured on this fixture: running 5,300, expired 42,800.
    h0::TimerModel t;
    t.setDuration(60 * SEC);
    t.start(0);
    h0::TimerFace face;
    face.restart(t, 4u);
    uint64_t now = 0;
    runTo(face, t, now, 30 * SEC);

    Panel running;
    face.render(running, t, 30 * SEC);

    t.tick(120 * SEC);
    REQUIRE(t.isExpired());
    Panel expired;
    face.render(expired, t, 120 * SEC);

    CHECK(inkCount(running) < 15000);
    CHECK(inkCount(expired) > 30000);
}

TEST_CASE("rotate180 is exact and is its own inverse") {
    // The failure mode is a mirrored-but-plausible readout: "05:12" reversed is
    // still five plausible glyphs. Only a per-pixel reference catches a bit-order
    // error, and only the involution catches an off-by-one in the byte walk.
    // The fixture MUST be asymmetric under 180 degrees, or the test cannot fail.
    // The obvious-looking ((x*7 + y*13) & 5) == 1 is not: it is invariant under
    // the rotation, so a no-op rotate180 passes it. Verified by construction
    // below rather than by inspection, because that is exactly the mistake this
    // comment exists to stop someone repeating.
    auto pattern = [](int16_t x, int16_t y) {
        return (x < 40 && y < 60) || ((x * 3 + y) % 7 == 0 && x > y / 2);
    };

    Panel fb, reference, original;
    fb.clear(WHITE);
    reference.clear(WHITE);
    original.clear(WHITE);
    for (int16_t y = 0; y < 280; ++y) {
        for (int16_t x = 0; x < 240; ++x) {
            if (pattern(x, y)) { fb.setPixel(x, y, BLACK); original.setPixel(x, y, BLACK); }
            if (pattern(static_cast<int16_t>(239 - x), static_cast<int16_t>(279 - y)))
                reference.setPixel(x, y, BLACK);
        }
    }

    // Guard the guard: if the fixture were symmetric, the two CHECKs below would
    // both pass against a rotate180 that did nothing at all.
    REQUIRE(diff(reference, original, 0, 0, 0, 0, false) > 1000);

    h0::rotate180(fb);
    CHECK(diff(fb, reference, 0, 0, 0, 0, false) == 0);

    h0::rotate180(fb);
    CHECK(diff(fb, original, 0, 0, 0, 0, false) == 0);
}

TEST_CASE("the timer face golden across states") {
    h0::TimerModel t;
    t.setDuration(120 * SEC);
    h0::TimerFace face;
    face.restart(t, 0xC0FFEEu);

    Panel fb;
    checkGolden((face.render(fb, t, 0), fb), "timer@idle");

    t.start(0);
    uint64_t now = 0;
    runTo(face, t, now, 30 * SEC);
    checkGolden((face.render(fb, t, 30 * SEC), fb), "timer@running");

    runTo(face, t, now, 60 * SEC);
    checkGolden((face.render(fb, t, 60 * SEC), fb), "timer@half");

    t.pause(60 * SEC);
    checkGolden((face.render(fb, t, 60 * SEC), fb), "timer@paused");

    t.resume(60 * SEC);
    t.tick(200 * SEC);
    REQUIRE(t.isExpired());
    checkGolden((face.render(fb, t, 200 * SEC), fb), "timer@expired");
}

// ------------------------------------------------------ the drain, metered --

TEST_CASE("the sand leaves one grain at a time, not in bars") {
    // The aperture is five cells wide, and the old gate opened all five together
    // so the whole width discharged in a single tick: measured, 381 of 429 flow
    // events were exactly five grains, and at 30 minutes that was one bar every
    // 4.2 seconds with nothing in between.
    //
    // The cause was not that the gate was binary -- it was that the five cells
    // were perfectly CORRELATED. A per-cell accumulator started in phase
    // reproduces the bar exactly; staggering the phases is the whole fix.
    for (int seconds : {300, 1800}) {
        h0::SandVessel v;
        v.begin();
        v.reset(1234u, 2000);
        const int total = seconds * 30;
        int prev = 0, events = 0, fullWidth = 0, dry = 0, dryMax = 0;
        for (int i = 0; i < total; ++i) {
            v.tick(1.0f - static_cast<float>(i) / static_cast<float>(total));
            const int low = v.lowerCount();
            const int d = low - prev;
            prev = low;
            if (d > 0) {
                ++events;
                if (d >= 2 * h0::SandVessel::kHoleHalf + 1) ++fullWidth;
                dry = 0;
            } else if (++dry > dryMax) {
                dryMax = dry;
            }
        }
        CAPTURE(seconds);
        CHECK(events > 1000);
        CHECK(fullWidth == 0);      // was 89% of events
        CHECK(dryMax < 60);         // under two seconds; was 134 ticks at 1800 s
    }
}

TEST_CASE("metering the drain one grain at a time still keeps time") {
    // A prettier stream that no longer tracks the clock would be a failure. The
    // gate is proportional on a cumulative count, so the error is bounded by how
    // far behind schedule the sand is allowed to get.
    h0::SandVessel v;
    v.begin();
    v.reset(99u, 2000);
    const int total = 300 * 30;
    float worst = 0.0f;
    for (int i = 0; i < total; ++i) {
        const float frac = 1.0f - static_cast<float>(i) / static_cast<float>(total);
        v.tick(frac);
        const float want = (1.0f - frac) * static_cast<float>(v.charge());
        const float err = (static_cast<float>(v.lowerCount()) - want) /
                          static_cast<float>(v.charge());
        const float mag = err < 0 ? -err : err;
        if (mag > worst) worst = mag;
    }
    CHECK(worst < 0.02f); // measured 0.34%
}

TEST_CASE("a falling grain accelerates instead of crawling") {
    // Every grain used to move exactly one cell per tick -- terminal velocity
    // from the instant it was released, which is why the stream read like syrup.
    auto dropTicks = [](int vmax) {
        h0::SandSim sim;
        sim.setWalls(h0::makeVessel(h0::SandVessel::kHoleHalf));
        sim.seed(1u);
        sim.setMaxFallSpeed(vmax);
        const int x = h0::sandgeom::HOLE_CX;
        sim.sand().set(x, h0::sandgeom::FLOOR_ROW + 1, true);
        int ticks = 0;
        while (ticks < 400 && sim.step(h0::Gravity::S) != 0) ++ticks;
        return ticks;
    };
    const int slow = dropTicks(0);
    const int fast = dropTicks(h0::SandVessel::kMaxFallSpeed);
    CHECK(slow > 50);          // one cell per tick over the lower chamber
    CHECK(fast * 3 < slow);    // measured 59 -> 17 ticks, 1.97 s -> 0.57 s
}

TEST_CASE("acceleration never costs or creates a grain") {
    // The ballistic pass moves grains outside the cellular scan, so it is the
    // one place that could break the conservation the whole simulation rests on.
    const h0::Gravity dirs[8] = {h0::Gravity::S,  h0::Gravity::SW, h0::Gravity::W,
                                 h0::Gravity::NW, h0::Gravity::N,  h0::Gravity::NE,
                                 h0::Gravity::E,  h0::Gravity::SE};
    for (int d = 0; d < 8; ++d) {
        h0::SandVessel v;
        v.begin();
        v.reset(21u, 900);
        v.setGravity(dirs[d]);
        const int charged = v.charge();
        for (int i = 0; i < 500; ++i) {
            v.tick(1.0f - static_cast<float>(i) / 500.0f);
            CAPTURE(d);
            CAPTURE(i);
            REQUIRE(v.sand().count() == charged);
        }
    }
}

// ---------------------------------------------------------- the agitation --

TEST_CASE("the sand goes completely dead when the device is put down") {
    // The whole point of gating drift on jerk rather than tilt angle. A device
    // resting at an angle reads the same as one being tipped, so an angle-gated
    // drift would creep forever on a desk.
    h0::Agitation a;
    const h0::Vec3 tilted{0.17f, 0.98f, 0.0f}; // about 10 degrees, held still
    for (int i = 0; i < 200; ++i) a.update(tilted);
    // Exactly zero, not merely small: an asymptote leaves a drift probability
    // that never quite reaches zero, and the sand shimmers indefinitely.
    CHECK(a.value() == 0.0f);
}

TEST_CASE("a shake settles within about a second") {
    h0::Agitation a;
    for (int i = 0; i < 60; ++i) {
        a.update(h0::Vec3{(i & 1) ? 0.6f : -0.6f, 0.8f, 0.0f});
    }
    CHECK(a.value() > 0.9f);

    const h0::Vec3 still{0.0f, 1.0f, 0.0f};
    int ticks = 0;
    while (a.value() > 0.0f && ticks < 300) { a.update(still); ++ticks; }
    CHECK(ticks > 20);  // not instant -- it should read as settling
    CHECK(ticks < 45);  // measured 33 ticks, 1.10 s
}

TEST_CASE("desk vibration does not wake the sand, but a deliberate tilt does") {
    // These two are close in raw jerk magnitude -- measured 0.016 against 0.0175
    // g per sample -- so amplitude alone cannot separate them. What does is
    // DIRECTION: a vibration's jerk alternates and cancels when the vector is
    // low-passed, while a tilt's points one way. Filtering |jerk| keeps both.
    h0::Agitation buzz;
    for (int i = 0; i < 300; ++i) {
        buzz.update(h0::Vec3{(i & 1) ? 0.008f : -0.008f, 1.0f, 0.0f});
    }
    CHECK(buzz.value() == 0.0f);

    h0::Agitation tilt;
    for (int i = 0; i < 60; ++i) {
        const float r = static_cast<float>(i) * 0.5f * 3.14159265f / 180.0f;
        tilt.update(h0::Vec3{std::sin(r), std::cos(r), 0.0f}); // 15 deg/s
    }
    CHECK(tilt.value() > 0.1f);
}

TEST_CASE("a small tilt moves sand only while the device is being handled") {
    // The dead zone this exists to kill: gravity is quantised to eight
    // directions, so anywhere below 22.5 degrees the simulation was handed an
    // identical vector and could not respond at all, however far it was tilted.
    //
    // Measures how far the settled pile shifts downhill. Drift only moves
    // surface grains -- a buried one is under load -- so the shift is a change
    // in the surface, not a relocation of the bulk.
    auto shift = [](float agitation, float gx, float gy) {
        h0::SandVessel v;
        v.begin();
        v.reset(4u, 900);
        v.setGravity(h0::Gravity::S);
        for (int i = 0; i < 200; ++i) v.tick(1.0f); // let the charge collapse

        auto bias = [&v]() {
            int left = 0, right = 0;
            for (int y = 1; y < h0::sandgeom::FLOOR_ROW; ++y)
                for (int x = 1; x < h0::SandGrid::W - 1; ++x)
                    if (v.sand().get(x, y)) (x < h0::sandgeom::HOLE_CX ? left : right) += 1;
            return right - left;
        };
        const int before = bias();
        for (int i = 0; i < 200; ++i) {
            v.setTilt(gx, gy, agitation);
            v.tick(1.0f);
        }
        return bias() - before;
    };

    // Sitting on a desk at 15 degrees: not one grain moves sideways. This is the
    // property the whole agitation gate exists for -- an angle-gated drift would
    // creep for as long as the device sat there.
    CHECK(shift(0.0f, 0.2588f, 0.9659f) == 0);

    // In the hand, a continuous ramp with angle where there used to be nothing
    // at all below 22.5 degrees. Measured: +4 at 5 degrees, +26 at 15, +34 at 20.
    const int at5 = shift(1.0f, 0.0872f, 0.9962f);
    const int at15 = shift(1.0f, 0.2588f, 0.9659f);
    const int at20 = shift(1.0f, 0.3420f, 0.9397f);
    CHECK(at5 > 0);
    CHECK(at15 > at5);
    CHECK(at20 > at15);
    CHECK(at15 > 15);
}

// ---------------------------------------------------------- the settings face --

TEST_CASE("settings face: the theme row") {
    Panel fb;
    h0::SettingsUi ui;
    ui.open(h0::kDefaults);
    h0::BatteryReading bat{};
    h0::SettingsFace::renderAt(fb, ui, bat);
    checkGolden(fb, "settings@theme");
}

namespace {

/// Advance the row wheel by one row. The row wheel never accelerates, so a
/// single slow drag of exactly one pitch moves exactly one row.
void nextRow(h0::SettingsUi& ui) {
    constexpr int16_t PPU = h0::DragColumn::kPixelsPerUnit;
    int16_t y = 200;
    ui.onDrag(1, true, y);
    for (int i = 0; i < PPU; ++i) ui.onDrag(1, true, static_cast<int16_t>(--y));
    ui.onDrag(1, false, y);
}

} // namespace

TEST_CASE("settings face: the battery row, calibrated") {
    Panel fb;
    h0::SettingsUi ui;
    h0::Settings s = h0::kDefaults;
    s.batCalPermille = 1032;
    ui.open(s);
    // Bounded: the wheel wraps, so a full lap is the worst case. An unbounded
    // "while not Battery" loop would hang forever on any indexing bug.
    for (uint8_t i = 0; i < h0::rowCount() && ui.currentRow() != h0::RowId::Battery; ++i) {
        nextRow(ui);
    }
    REQUIRE(ui.currentRow() == h0::RowId::Battery);
    h0::BatteryReading bat{3820, 3940, false, true, true};
    h0::SettingsFace::renderAt(fb, ui, bat);
    checkGolden(fb, "settings@battery");
}

TEST_CASE("settings face: the CAL row, automatic") {
    // The AUTO/MAN marker is the only warning anywhere on screen that
    // hand-setting CAL has switched automatic calibration off, so it needs
    // its own render-level regression coverage, not just formatValue() called
    // directly. This is also the near-full-width case: raw 4190 mV at the
    // default 1000 permille prints "AUTO 4.19v", 89 of the value column's
    // 90 px, the tightest string this row can produce.
    Panel fb;
    h0::SettingsUi ui;
    h0::Settings s = h0::kDefaults; // batCalAuto == 1
    ui.open(s);
    for (uint8_t i = 0; i < h0::rowCount() && ui.currentRow() != h0::RowId::Cal; ++i) {
        nextRow(ui);
    }
    REQUIRE(ui.currentRow() == h0::RowId::Cal);
    h0::BatteryReading bat{4190, 4190, false, false, true};
    h0::SettingsFace::renderAt(fb, ui, bat);
    checkGolden(fb, "settings@cal-auto");
}

TEST_CASE("settings face: the CAL row, hand-set") {
    Panel fb;
    h0::SettingsUi ui;
    h0::Settings s = h0::kDefaults;
    s.batCalAuto = 0;
    s.batCalPermille = 1052; // an arbitrary learned-then-hand-tuned gain
    ui.open(s);
    for (uint8_t i = 0; i < h0::rowCount() && ui.currentRow() != h0::RowId::Cal; ++i) {
        nextRow(ui);
    }
    REQUIRE(ui.currentRow() == h0::RowId::Cal);
    h0::BatteryReading bat{3700, 3700, false, false, true};
    h0::SettingsFace::renderAt(fb, ui, bat);
    checkGolden(fb, "settings@cal-man");
}

TEST_CASE("settings face: mid-drag on the value column") {
    Panel fb;
    h0::SettingsUi ui;
    ui.open(h0::kDefaults);
    ui.onDrag(2, true, 200);
    ui.onDrag(2, true, 190); // 10 px of sub-unit offset
    h0::BatteryReading bat{};
    h0::SettingsFace::renderAt(fb, ui, bat);
    checkGolden(fb, "settings@dragging");
}

TEST_CASE("every settings row's value fits its column and clears the clip") {
    // The rounded corners and h0::safe both have to hold for EVERY value any
    // row can display, not just the ones a golden happens to capture.
    // Both calibration states, because they format differently and only one of
    // them is exercised by the goldens.
    for (int cal = 0; cal < 2; ++cal) {
    h0::BatteryReading bat{3820, 3940, false, cal == 1, true};
    for (uint8_t r = 0; r < h0::rowCount(); ++r) {
        const h0::RowId id = static_cast<h0::RowId>(r);
        const uint8_t n = h0::ladderSize(id);
        for (uint8_t i = 0; i < (n == 0 ? 1 : n); ++i) {
            h0::Settings s = h0::kDefaults;
            if (n > 0) h0::applyLadder(id, i, s);
            char buf[12];
            h0::SettingsFace::formatValue(id, i, s, bat, buf, sizeof(buf));
            const int16_t w =
                onebit::getBitmapTextWidth(onebit::fonts::TERM_8X12, buf);
            const int16_t x0 = static_cast<int16_t>(164 - w / 2);
            const int16_t x1 = static_cast<int16_t>(x0 + w);
            CHECK(x0 >= 24);   // inside the selection rules
            CHECK(x1 <= 216);
            CHECK(x0 >= h0::safe::X);
            CHECK(x1 <= h0::safe::X + h0::safe::W);
            // Section 8 caps the value column at 9 characters against the 13 px
            // gutter that "BLANK AT" leaves. Assert the cap, not just the rules.
            // CAL's AUTO position (Task 5) is a deliberate, single exception:
            // "AUTO 4.19v" is 10 characters, 89 px -- and the x0/x1 checks above
            // already prove it clears the selection window and the safe box with
            // margin to spare, so the cap widens only for the one row that needs
            // it rather than loosening the invariant for everybody.
            // 89 px against COL_HALF*2 = 90: the real clipping bounds checked
            // above still have margin to spare, but the NOMINAL column budget
            // is down to its last pixel -- there is no room left for this
            // string to grow.
            const int16_t cap = (id == h0::RowId::Cal && i == 0) ? 89 : 80;
            CHECK(w <= cap);
        }
    }
    }
}

TEST_CASE("the mute glyph appears only when muted, and changes nothing else") {
    // MUTE silences the only acknowledgement channel an invisible gesture
    // vocabulary has. The glyph is what stops that state being discoverable
    // solely by the absence of a sound.
    Panel plain, muted;
    h0::TimerFace f1, f2;
    h0::TimerModel t;
    t.setDuration(120 * SEC);

    f1.setMuted(false);
    f1.render(plain, t, 0);
    f2.setMuted(true);
    f2.render(muted, t, 0);

    const int extra = inkCount(muted) - inkCount(plain);
    CHECK(extra > 0);   // it is drawn
    CHECK(extra < 120); // and it is small -- this is a marker, not a banner

    // And it is where it claims to be: nothing outside x 18..26 / y 18..26 moved.
    for (int16_t y = 0; y < 280; ++y) {
        for (int16_t x = 0; x < 240; ++x) {
            if (x >= 18 && x <= 26 && y >= 18 && y <= 26) continue;
            CHECK(plain.getPixel(x, y) == muted.getPixel(x, y));
        }
    }
}

TEST_CASE("power face: the hold prompt, half filled") {
    Panel fb;
    h0::PowerFace::renderAt(fb, h0::PowerAction::PromptHold, 128);
    checkGolden(fb, "power@hold");
}

TEST_CASE("power face: the release prompt") {
    Panel fb;
    h0::PowerFace::renderAt(fb, h0::PowerAction::PromptRelease, 255);
    checkGolden(fb, "power@release");
}

TEST_CASE("the CAL row says whether the gauge is calibrating itself") {
    // The marker IS the warning: hand-setting CAL disables automatic
    // calibration, and nothing else on screen would say so.
    h0::Settings s;
    s.batCalPermille = 1000;

    h0::BatteryReading bat;
    bat.valid = true;
    bat.rawMilliVolts = 3700;

    char buf[24];

    s.batCalAuto = 1;
    h0::SettingsFace::formatValue(h0::RowId::Cal, 0, s, bat, buf, sizeof(buf));
    CHECK(std::string(buf) == "AUTO 3.70v");

    s.batCalAuto = 0;
    h0::SettingsFace::formatValue(h0::RowId::Cal, 0, s, bat, buf, sizeof(buf));
    CHECK(std::string(buf) == "3.70v MAN");
}

TEST_CASE("the battery row shows CHARGING before it shows UNCAL") {
    // The UNCAL gate used to return first, so no device could show CHARGING
    // until its gain had been anchored -- which is every device for the whole
    // of the first hardware verification pass. The old ordering's reason was
    // that `charging` was a 4220 mV threshold an uncalibrated divider could
    // trip on its own; it is now AutoCal's rise-then-plateau on the RAW
    // reading, gain-independent by construction, so the reason is gone.
    h0::Settings s = h0::kDefaults;
    h0::BatteryReading bat;
    bat.valid = true;
    bat.rawMilliVolts = 4000;
    bat.milliVolts = 4000;
    bat.calibrated = false;
    bat.charging = true;

    char buf[24];
    h0::SettingsFace::formatValue(h0::RowId::Battery, 0, s, bat, buf, sizeof(buf));
    CHECK(std::string(buf) == "CHARGING");

    // The gate itself still holds when nothing is charging: a bucket read off
    // a +/-9% gain is uninformative, not merely imprecise.
    bat.charging = false;
    h0::SettingsFace::formatValue(h0::RowId::Battery, 0, s, bat, buf, sizeof(buf));
    CHECK(std::string(buf) == "UNCAL");
}

TEST_CASE("CAL shows AT LIMIT at both ends of the range, not just past them") {
    // <= and >=, not < and >: the boundary values themselves already mean
    // "no headroom left", not one step short of it. This had no assertion at
    // all before Task 5 -- and the round-trip sweep that used to reach
    // kCalMin by accident (its old i=0 iteration landed exactly on it) no
    // longer does, now that every CAL iteration starts from kDefaults'
    // mid-range permille. So it is asserted directly instead of incidentally.
    h0::Settings s = h0::kDefaults;
    h0::BatteryReading bat;
    bat.valid = true;
    bat.rawMilliVolts = 3700;
    char buf[24];

    s.batCalPermille = h0::kCalMin;
    h0::SettingsFace::formatValue(h0::RowId::Cal, 0, s, bat, buf, sizeof(buf));
    CHECK(std::string(buf) == "AT LIMIT");

    s.batCalPermille = h0::kCalMax;
    h0::SettingsFace::formatValue(h0::RowId::Cal, 0, s, bat, buf, sizeof(buf));
    CHECK(std::string(buf) == "AT LIMIT");
}

// ---------------------------------------------------------------- the boot --

TEST_CASE("the boot splash is inside the safe box and inside the corners") {
    Panel fb;
    h0::BootFace::renderAt(fb);

    // Something is actually drawn -- a face that clears to WHITE and returns
    // would otherwise pass every geometric check below.
    int ink = 0;
    for (int16_t y = 0; y < 280; ++y)
        for (int16_t x = 0; x < 240; ++x)
            if (fb.getPixel(x, y) == BLACK) ++ink;
    CHECK(ink > 100);

    // Every inked pixel clears the safe box. h0::safe's own corner (16, 16)
    // sits inside the r=44 clip disc (39.6 px from the disc centre at
    // (44, 44)), so the safe box is wholly contained in the rounded rect --
    // outsideSafe == 0 already implies inkInCorners(fb) == 0 for THIS face,
    // and cannot fail independently of it. The corner check stays anyway for
    // uniformity with the other eight face tests that call inkInCorners, and
    // because it stops being redundant the moment any face is allowed to draw
    // outside h0::safe -- do not mistake it for a live guard on this one.
    int outsideSafe = 0;
    for (int16_t y = 0; y < 280; ++y) {
        for (int16_t x = 0; x < 240; ++x) {
            if (fb.getPixel(x, y) != BLACK) continue;
            if (x < h0::safe::X || x >= h0::safe::X + h0::safe::W ||
                y < h0::safe::Y || y >= h0::safe::Y + h0::safe::H) ++outsideSafe;
        }
    }
    CHECK(outsideSafe == 0);
    CHECK(inkInCorners(fb) == 0);

    checkGolden(fb, "boot@splash");
}


