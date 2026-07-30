#include "doctest.h"

#include "golden.hpp"

#include <1bit/core/framebuffer.hpp>

#include <cstdio>

#include "faces/timer_face.hpp"
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

/// The lintel interior -- the readout's home, and the region the whole design
/// exists to keep clear of sand.
constexpr int16_t CARD_X = h0::sandgeom::LINTEL_IN_X;
constexpr int16_t CARD_Y = h0::sandgeom::LINTEL_IN_Y;
constexpr int16_t CARD_W = h0::sandgeom::LINTEL_IN_W;
constexpr int16_t CARD_H = h0::sandgeom::LINTEL_IN_H;

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

// ------------------------------------------------- the legibility mechanism --

TEST_CASE("the lintel interior is empty in the wall grid, so sand cannot enter it") {
    // This is the whole design. Not a statistic about where grains happen to
    // land -- the interior is simply not a place a grain can be, because the
    // housing is solid to the physics. If this fails, the lintel failed to
    // reach `open_` (or `shut_`) and the readout is drawing on black sand.
    const h0::Gravity dirs[8] = {h0::Gravity::S,  h0::Gravity::SW, h0::Gravity::W,
                                 h0::Gravity::NW, h0::Gravity::N,  h0::Gravity::NE,
                                 h0::Gravity::E,  h0::Gravity::SE};
    for (int grains : {400, 900, 2000}) {
        for (int d = 0; d < 8; ++d) {
            h0::SandVessel v;
            v.begin();
            v.reset(0xC0FFEEu, grains);
            v.setGravity(dirs[d]);
            for (int i = 0; i < 400; ++i) {
                v.tick(1.0f - static_cast<float>(i) / 400.0f);
                int inside = 0;
                for (int cy = h0::sandgeom::LINTEL_CY0; cy <= h0::sandgeom::LINTEL_CY1; ++cy)
                    for (int cx = h0::sandgeom::LINTEL_CX0; cx <= h0::sandgeom::LINTEL_CX1; ++cx)
                        if (v.sand().get(cx, cy)) ++inside;
                if (inside != 0) { CAPTURE(grains); CAPTURE(d); CAPTURE(i); REQUIRE(inside == 0); }
            }
        }
    }
}

TEST_CASE("a sand-only render leaves the card white, with no help from the face") {
    // The corollary of the wall trick, and the one that catches `walls()` being
    // switched back to the physics grid: that would paint the housing solid
    // black -- 5,700 px of it -- which is exactly the black-on-black failure the
    // design exists to prevent, arriving as a one-line change.
    for (int grains : {400, 900, 2000}) {
        h0::SandVessel v;
        v.begin();
        v.reset(7u, grains);
        for (int i = 0; i < 600; ++i) {
            v.tick(1.0f - static_cast<float>(i) / 600.0f);
            if (i % 60) continue;
            Panel fb;
            fb.clear(WHITE);
            h0::renderSand(fb, v.sand(), v.walls());
            CAPTURE(grains);
            CAPTURE(i);
            REQUIRE(inkInRect(fb, CARD_X, CARD_Y, CARD_W, CARD_H) == 0);
        }
    }
}

TEST_CASE("what the card shows does not depend on where the sand is") {
    // The property stated directly: two runs of the same timer, showing the same
    // time, but with the sand driven into completely different configurations.
    // Inside the card they must be pixel-identical.
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

    CHECK(diff(fa, fb2, CARD_X, CARD_Y, CARD_W, CARD_H, true) == 0);
    // ...and the sand really was somewhere different, or the test proves nothing.
    CHECK(diff(fa, fb2, CARD_X, CARD_Y, CARD_W, CARD_H, false) > 500);
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

    // Compare outside the lintel entirely: inside it the composite has the
    // board and the label, and the sand-only frame does not.
    Panel bare;
    bare.clear(WHITE);
    h0::SandGrid drawn = h0::makeVessel(h0::SandVessel::kHoleHalf);
    h0::drawLintelOutline(drawn);
    h0::renderSand(bare, face.sand(), drawn);

    CHECK(diff(composed, bare, h0::sandgeom::LINTEL_X, h0::sandgeom::LINTEL_Y,
               h0::sandgeom::LINTEL_W, h0::sandgeom::LINTEL_H, false) == 0);
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
        h0::SandGrid drawn = h0::makeVessel(h0::SandVessel::kHoleHalf);
        h0::drawLintelOutline(drawn);
        h0::renderSand(bare, face.sand(), drawn);

        CAPTURE(i);
        REQUIRE(diff(composed, bare, h0::sandgeom::LINTEL_X, h0::sandgeom::LINTEL_Y,
                     h0::sandgeom::LINTEL_W, h0::sandgeom::LINTEL_H, false) == 0);
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

TEST_CASE("short timers never reach the lintel at all") {
    // The lintel costs the sand nothing below three minutes: the charge is not
    // tall enough to reach it, so the volume it occupies was empty anyway. This
    // is why every timer up to three minutes behaves exactly as it did before
    // the readout moved into the chamber.
    for (int grains : {400, 900}) {
        h0::SandVessel v;
        v.begin();
        v.reset(42u, grains);
        CAPTURE(grains);
        CHECK(v.charge() == grains); // nothing truncated by the obstacle
        CHECK(v.sand().countRows(h0::sandgeom::LINTEL_CY0, h0::sandgeom::LINTEL_CY1) == 0);
    }

    // The top tier does reach it, and must still fit with margin to spare.
    h0::SandVessel big;
    big.begin();
    big.reset(42u, 2000);
    CHECK(big.charge() >= 2000);
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
    // Pins the board's ink box, which is what makes the knockout exactly sized.
    h0::TimerModel t;
    t.setDuration(300 * SEC);
    t.start(0);
    h0::TimerFace face;
    face.restart(t, 1u);

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
    // The bounding box is scanned over the card, so comparing it back against the
    // card would be true by construction -- and would pass on a blank card too.
    // Assert the measured box instead, and require it to exist at all.
    REQUIRE(x1 >= 0); // a readout that drew nothing must fail, not pass vacuously
    CHECK(x0 == 68);  // kBoardX
    CHECK(y0 == 20);  // kBoardY
    CHECK(x1 == 172); // kBoardX + 5 * kCellW - 1, cell borders included
    CHECK(y1 == 53);  // kBoardY + kCellH - 1; Running, so no label band
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
