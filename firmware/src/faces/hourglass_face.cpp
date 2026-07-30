#include "faces/hourglass_face.hpp"

#include <1bit/render/pattern.hpp>
#include <1bit/render/primitives.hpp>

namespace h0 {

namespace {

using onebit::BLACK;
using onebit::WHITE;

// --------------------------------------------------------------- geometry --
//
// A bowtie: two trapezoids meeting at the neck. Real hourglasses have curved
// shoulders, but at this size a curve costs pixels and reads as noise, whereas
// straight facets read as deliberate. Everything sits inside the measured 16 px
// safe rectangle, so nothing lands under the ~44 px corner radius.

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

/// Interior area of the upper bulb, in pixels.
int32_t upperCapacity() {
    int32_t a = 0;
    for (int16_t y = TOP; y <= NECK; ++y) a += 2 * halfWidthAt(y);
    return a;
}

/// Largest 45-degree cone the lower bulb can actually hold.
///
/// **This is smaller than the upper bulb holds** -- 7,968 px against 9,196 --
/// because a cone is bounded both by its own slope and by the bulb's taper, and
/// the two constraints bite at once. A real hourglass has the same problem and
/// solves it by not filling the top completely.
///
/// The consequence matters: mapping the pile's area directly to the drained
/// area saturates at fraction 0.1335 and then never changes again, so the pile
/// freezes for the last 13% of every countdown while the top keeps draining --
/// and about 12% of the visible sand simply disappears. The pile target is
/// therefore scaled against *this* capacity, not the upper bulb's. Sand is
/// conserved proportionally rather than literally, which is a deliberate lie
/// the eye cannot detect and a frozen pile very much can.
int32_t maxPileArea() {
    int32_t a = 0;
    for (int16_t y = NECK; y <= BOT; ++y) {
        const int16_t hw = halfWidthAt(y);
        const int16_t reach = static_cast<int16_t>(y - NECK);
        a += 2 * ((reach < hw) ? reach : hw);
    }
    return a;
}

/// Row at which the upper sand surface sits for a given fill fraction.
///
/// Solved for equal AREA rather than equal height. The bulb tapers by a factor
/// of ~15 from rim to neck, so a height-linear mapping would drain visibly fast
/// at the top and crawl at the bottom -- the opposite of a real hourglass, where
/// the surface descends slowly at first because the cross-section is widest.
int16_t upperSurfaceY(float fraction) {
    const int32_t target = static_cast<int32_t>(static_cast<float>(upperCapacity()) * fraction);
    if (target <= 0) return NECK + 1; // empty
    int32_t acc = 0;
    for (int16_t y = NECK; y >= TOP; --y) {
        acc += 2 * halfWidthAt(y);
        if (acc >= target) return y;
    }
    return TOP;
}

/// Height of the lower pile's apex above the floor, for a given filled area.
///
/// The pile is a 45-degree cone, not a flat level: a flat surface reads as
/// liquid and a cone reads as granular. That single choice does more for the
/// illusion than any amount of texture.
int16_t pileApexY(int32_t targetArea) {
    if (targetArea <= 0) return BOT + 1;

    // Area of the cone clipped to the bulb, for a given apex row.
    auto areaFor = [](int16_t apex) -> int32_t {
        int32_t a = 0;
        for (int16_t y = apex; y <= BOT; ++y) {
            const int16_t hw = halfWidthAt(y);
            // Cone surface at horizontal distance d is apex + d, so at row y the
            // pile spans |d| <= y - apex, clipped to the glass.
            const int16_t reach = static_cast<int16_t>(y - apex);
            const int16_t w = (reach < hw) ? reach : hw;
            a += 2 * w;
        }
        return a;
    };

    // A single descending scan, accumulating as it goes. The binary search this
    // replaced rescanned every row from the probe to BOT on each of its ~7
    // probes -- 623 row-visits at low fill against 103 for one descent.
    int32_t acc = 0;
    for (int16_t apex = BOT; apex >= NECK; --apex) {
        acc = areaFor(apex);
        if (acc >= targetArea) return apex;
    }
    // Never return above the neck: the cone tip would be clipped by the caller's
    // `y < NECK` guard and leave a detached blob sitting on the neck row.
    return NECK;
}

/// One horizontal run of sand, textured.
///
/// `fillPatternRect` indexes the pattern off absolute framebuffer coordinates,
/// so the texture is screen-anchored for free -- the boundary moves through a
/// stationary field rather than dragging it along.
void sandSpan(onebit::IFramebuffer& fb, int16_t y, int16_t x0, int16_t x1,
              const onebit::Pattern& p) {
    if (x1 < x0) return;
    onebit::fillPatternRect(fb, x0, y, static_cast<int16_t>(x1 - x0 + 1), 1, p);
}

/// Draw a 2 px wall by stamping the line twice, one pixel apart horizontally.
///
/// Not `drawThickLine`: it offsets parallel copies by integer pixel amounts, so
/// on a diagonal every copy lands on the diagonal lattice sqrt(2) apart and the
/// wall comes out striped (upstream 1bit-display#10). These walls run at ~59
/// degrees -- steeper than 45 -- so a horizontal offset of one pixel always
/// produces a solid band, and sidesteps the defect entirely.
void wall2px(onebit::IFramebuffer& fb, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
             int16_t dir) {
    onebit::drawLine(fb, x0, y0, x1, y1, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(x0 + dir), y0,
                     static_cast<int16_t>(x1 + dir), y1, BLACK);
}

void drawGlass(onebit::IFramebuffer& fb) {
    // Rim caps, 2 px: the glass is the top-level container of the whole face,
    // and a 1 px frame is not structural at 218 ppi.
    for (int16_t d = 0; d < 2; ++d) {
        onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), static_cast<int16_t>(TOP + d),
                         static_cast<int16_t>(CX + HW_MAX), static_cast<int16_t>(TOP + d), BLACK);
        onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), static_cast<int16_t>(BOT - d),
                         static_cast<int16_t>(CX + HW_MAX), static_cast<int16_t>(BOT - d), BLACK);
    }
    // The four tapering walls, thickened inward so the silhouette keeps its size.
    wall2px(fb, static_cast<int16_t>(CX - HW_MAX), TOP,
            static_cast<int16_t>(CX - HW_NECK), NECK, +1);
    wall2px(fb, static_cast<int16_t>(CX + HW_MAX), TOP,
            static_cast<int16_t>(CX + HW_NECK), NECK, -1);
    wall2px(fb, static_cast<int16_t>(CX - HW_NECK), NECK,
            static_cast<int16_t>(CX - HW_MAX), BOT, +1);
    wall2px(fb, static_cast<int16_t>(CX + HW_NECK), NECK,
            static_cast<int16_t>(CX + HW_MAX), BOT, -1);
}

/// How far the sand keeps clear of the glass wall.
///
/// The dither is 82% coverage against a solid wall -- only 45 density steps
/// apart, which is well inside the range where two regions read as one. Without
/// a paper gutter the sand merges into the glass and the silhouette is lost. Two
/// pixels of white is the cheapest fix and costs ~4% of the sand area.
constexpr int16_t SAND_GUTTER = 4; // 2 px of wall + 2 px of paper

} // namespace

void HourglassFace::renderAt(onebit::IFramebuffer& fb, float fraction, bool running,
                             uint32_t phase) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    fb.clear(WHITE);

    // Dense ordered dither. High density so the body reads as a solid mass with
    // grain rather than as a grey wash -- the point is texture on a silhouette,
    // not a fake grey level.
    const onebit::Pattern sand = onebit::bayer(210, 8);

    drawGlass(fb);

    // ------------------------------------------------------------- upper --
    const int16_t surf = upperSurfaceY(fraction);
    for (int16_t y = surf; y <= NECK; ++y) {
        const int16_t hw = static_cast<int16_t>(halfWidthAt(y) - SAND_GUTTER);
        if (hw <= 0) continue;
        sandSpan(fb, y, static_cast<int16_t>(CX - hw), static_cast<int16_t>(CX + hw), sand);
    }

    // ------------------------------------------------------------- lower --
    // Proportional, not literal, conservation. The lower bulb cannot hold a
    // 45-degree cone of the upper bulb's full area (7,968 px against 9,196), so
    // the pile is scaled against what it *can* hold. Mapping drained area
    // directly would saturate at fraction 0.13 and freeze the pile for the last
    // eighth of every countdown -- far more visible than a scale factor nobody
    // can measure by eye.
    const int32_t target =
        static_cast<int32_t>(static_cast<float>(maxPileArea()) * (1.0f - fraction));
    const int16_t apex = pileApexY(target);
    for (int16_t y = apex; y <= BOT; ++y) {
        if (y < NECK) continue;
        const int16_t hw = static_cast<int16_t>(halfWidthAt(y) - SAND_GUTTER);
        if (hw <= 0) continue;
        const int16_t reach = static_cast<int16_t>(y - apex);
        const int16_t w = (reach < hw) ? reach : hw;
        sandSpan(fb, y, static_cast<int16_t>(CX - w), static_cast<int16_t>(CX + w), sand);
    }

    // ------------------------------------------------------------ stream --
    // The main "is it running?" affordance, readable without reading anything.
    // Solid rather than dithered: at two pixels wide a dithered column
    // disappears into its own texture.
    //
    // Deliberately NOT animated. A per-frame wobble is the shimmer the rule set
    // forbids on the thinnest feature on screen, and it would make the stream
    // the only thing changing in most frames -- turning a face that needs a
    // repaint every few seconds into one that repaints continuously, on a
    // battery device, for a wobble nobody asked for.
    if (running && fraction > 0.0f) {
        const int16_t top = static_cast<int16_t>(NECK + 1);
        // Stop at the pile, but always draw at least one row: `apex` reaches the
        // neck near the end of the run, and a stream that vanishes while sand is
        // still visible upstairs is the worst frame in the whole animation.
        int16_t bot = (apex > BOT) ? BOT : apex;
        if (bot <= top) bot = static_cast<int16_t>(top + 2);
        for (int16_t y = top; y < bot; ++y) {
            fb.setPixel(CX, y, BLACK);
            fb.setPixel(static_cast<int16_t>(CX - 1), y, BLACK);
        }
    }
    (void)phase; // kept in the signature for a future grain-scale stream
}

void HourglassFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    // ~12 Hz stream animation, independent of how often the face is rendered.
    const uint32_t phase = static_cast<uint32_t>((now / 80'000ull) & 0xFFFFu);
    renderAt(fb, t.fraction(now), t.isRunning(), phase);
}

} // namespace h0
