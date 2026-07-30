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

    int16_t lo = NECK, hi = static_cast<int16_t>(BOT + 1);
    while (lo < hi) {
        const int16_t mid = static_cast<int16_t>(lo + (hi - lo) / 2);
        if (areaFor(mid) >= targetArea) lo = static_cast<int16_t>(mid + 1);
        else hi = mid;
    }
    return static_cast<int16_t>(lo - 1);
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

void drawGlass(onebit::IFramebuffer& fb) {
    // Rim caps.
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), TOP,
                     static_cast<int16_t>(CX + HW_MAX), TOP, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), BOT,
                     static_cast<int16_t>(CX + HW_MAX), BOT, BLACK);
    // The four tapering walls.
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_MAX), TOP,
                     static_cast<int16_t>(CX - HW_NECK), NECK, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX + HW_MAX), TOP,
                     static_cast<int16_t>(CX + HW_NECK), NECK, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX - HW_NECK), NECK,
                     static_cast<int16_t>(CX - HW_MAX), BOT, BLACK);
    onebit::drawLine(fb, static_cast<int16_t>(CX + HW_NECK), NECK,
                     static_cast<int16_t>(CX + HW_MAX), BOT, BLACK);
}

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
        const int16_t hw = static_cast<int16_t>(halfWidthAt(y) - 1);
        if (hw <= 0) continue;
        sandSpan(fb, y, static_cast<int16_t>(CX - hw), static_cast<int16_t>(CX + hw), sand);
    }

    // ------------------------------------------------------------- lower --
    // Conserve visible mass: what left the top arrives at the bottom.
    const int32_t drained =
        static_cast<int32_t>(static_cast<float>(upperCapacity()) * (1.0f - fraction));
    const int16_t apex = pileApexY(drained);
    for (int16_t y = apex; y <= BOT; ++y) {
        if (y < NECK) continue;
        const int16_t hw = static_cast<int16_t>(halfWidthAt(y) - 1);
        if (hw <= 0) continue;
        const int16_t reach = static_cast<int16_t>(y - apex);
        const int16_t w = (reach < hw) ? reach : hw;
        sandSpan(fb, y, static_cast<int16_t>(CX - w), static_cast<int16_t>(CX + w), sand);
    }

    // ------------------------------------------------------------ stream --
    // The stream is the main "is it running?" affordance, readable without
    // reading anything. Drawn solid rather than dithered: at one or two pixels
    // wide a dithered column disappears into its own texture.
    if (running && fraction > 0.0f) {
        const int16_t streamTop = static_cast<int16_t>(NECK + 1);
        const int16_t streamBot = (apex > BOT) ? BOT : apex;
        for (int16_t y = streamTop; y < streamBot; ++y) {
            // Jitter by one pixel on a fixed period so the stream shimmers
            // slightly instead of reading as a static drawn line.
            const int16_t j = static_cast<int16_t>(((y + static_cast<int16_t>(phase)) % 5 == 0) ? 1 : 0);
            fb.setPixel(static_cast<int16_t>(CX + j), y, BLACK);
            fb.setPixel(static_cast<int16_t>(CX - 1 + j), y, BLACK);
        }
    }
}

void HourglassFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    // ~12 Hz stream animation, independent of how often the face is rendered.
    const uint32_t phase = static_cast<uint32_t>((now / 80'000ull) & 0xFFFFu);
    renderAt(fb, t.fraction(now), t.isRunning(), phase);
}

} // namespace h0
