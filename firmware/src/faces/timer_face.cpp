#include "faces/timer_face.hpp"

#include <1bit/fonts/flap_13x26.hpp>
#include <1bit/fonts/term_6x9.hpp>
#include <1bit/render/bitmap_font.hpp>
#include <1bit/render/blit.hpp>
#include <1bit/render/primitives.hpp>

#include <cstdio>
#include <cstring>

#include "render/raster_ops.hpp"
#include "sand/sand_render.hpp"

namespace h0 {

using onebit::BLACK;
using onebit::WHITE;

namespace {

// ------------------------------------------------------------ the board --

constexpr int16_t kCols = 5; // M M : S S

/// The flap font's own raster. Named rather than left as bare 13s and 26s
/// scattered through the file, because every dimension below is DERIVED from it
/// and the derivation is what has to stay true when the scale changes -- a
/// literal that happened to be right at 1x is silently wrong at 2x.
constexpr int16_t kGlyphW = 13;
constexpr int16_t kGlyphH = 26;
static_assert(onebit::fonts::FLAP_13X26.glyph_width == kGlyphW &&
                  onebit::fonts::FLAP_13X26.glyph_height == kGlyphH,
              "the font stopped being 13x26 and every cell dimension below is derived from it");

/// Integer magnification of the GLYPH. The cell, its border and its split line
/// are not scaled by the library -- they are the housing the cards travel
/// behind -- so the cell is sized here to match.
///
/// A 13x26 readout is legible but small on a 240x280 panel, and the whole
/// point of taking the housing out of the wall grid was to afford this: at 1x
/// the board was confined to a 114 px lintel interior, and a 2x board is 170 px
/// wide.
constexpr int16_t kScale = 2;

constexpr int16_t kMargin = 8; ///< total, so 4 px each side at any scale
constexpr int16_t kCellW = kGlyphW * kScale + kMargin; // 34
constexpr int16_t kCellH = kGlyphH * kScale + kMargin; // 60
constexpr int16_t kBoardW = kCols * kCellW;            // 170

// The library CLAMPS scale down to what the cell holds, silently -- that is its
// documented contract, not an error -- so a cell too small for kScale would not
// fail to build, it would quietly render at 1x inside a 2x board.
//
// This is the constructor's arithmetic with ONE deliberate difference, and the
// difference is the limit of what it proves. The library divides by the largest
// extent it finds by walking the SEQUENCE's glyphs, precisely because
// `BitmapFont` has no invariant tying its header to its glyph table -- a
// fixed-width font may declare 13x26 and ship a taller glyph. A constant
// expression cannot walk that table, so this divides by the HEADER, checked
// against 13x26 above. It therefore catches the case that actually varies here
// -- someone changing kScale, kMargin or kCols -- and does NOT catch a
// regenerated font whose header no longer describes its glyphs.
//
// That residue is covered, but by the goldens rather than here: a board that
// silently dropped to 1x redraws every timer golden, and those are read before
// they are accepted.
static_assert(kCellW / kGlyphW >= kScale && kCellH / kGlyphH >= kScale,
              "the cell is too small for kScale and the library would silently downgrade it");

// The cell height must stay EVEN. The mechanical hinge sits at
// (kGlyphH / 2) * kScale below the glyph's top edge, while the decorative split
// line sits at kCellH / 2; both floors truncate, and an odd cell height moves
// one and not the other, so the falling card lands a row off the line it is
// supposed to land on.
static_assert(kCellH % 2 == 0, "an odd cell height desynchronises the hinge from the split line");

/// The separator is the middle cell, and it is drawn by hand rather than being
/// a flap cell. See kSequence below: a colon in the flap cycle is a colon every
/// digit cell must pass THROUGH on a wrap.
constexpr int16_t kSepCol = 2;

/// Where the board sits inside the panel, in PANEL-local pixels -- which is the
/// space the board is actually drawn in.
///
/// The widget positions its cells from `SplitFlapConfig::bounds`, and those
/// bounds used to be screen coordinates because the board was drawn straight
/// into the frame. It is drawn into a panel-sized scratch now -- so that a cell
/// can be stamped either way up -- and everything that draws into that scratch
/// (`baseConfig`, `drawSeparator`) has to work in its coordinates. `cellRect()`
/// converts back. Get this offset wrong and the whole readout lands in the wrong
/// place, or is silently cropped by the scratch's edge; the asserts below are
/// the tripwire for the second, which is the quiet one.
///
/// Horizontally centred: 170 of the panel's 182 px, so 6 px of gutter each side.
/// Vertically the panel's 92 px are spent
///
///     2 rail + 2 pad + 60 cell + 4 gap + 18 label + 4 pad + 2 rail
///
/// which puts the board 4 px down from the panel's top edge.
constexpr int16_t kBoardLX = (sandgeom::PANEL_W - kBoardW) / 2; // 6
constexpr int16_t kBoardLY = 4;

/// The same origin on the screen, which is what `cellRect()` and the coverage
/// maths speak.
constexpr int16_t kBoardX = sandgeom::PANEL_X + kBoardLX; // 35
constexpr int16_t kBoardY = sandgeom::PANEL_Y + kBoardLY; // 22

static_assert(kBoardLX >= 0 && kBoardLX + kBoardW <= sandgeom::PANEL_W,
              "the board must fit the panel-local scratch it is drawn into");
static_assert(kBoardLY >= 0 && kBoardLY + kCellH <= sandgeom::PANEL_H,
              "the board must fit the panel-local scratch it is drawn into");

// updateCoverage() divides a cell's screen rect into grid space, and integer
// division truncates toward zero -- so a board reaching left of or above the
// grid's origin would round the WRONG way and read the sand one cell over.
static_assert(kBoardX >= sandgeom::ORIGIN_X && kBoardY >= sandgeom::ORIGIN_Y,
              "the board must lie inside the sand grid for the coverage maths to hold");

// The housing is the PANEL now, not the lintel. Bounding the board against the
// lintel would be bounding it against a rect nothing draws any more -- and one
// this board no longer fits inside, which is the whole reason the lintel had to
// stop being a wall.
static_assert(kBoardX >= sandgeom::PANEL_X, "board must sit inside its housing");
static_assert(kBoardX + kBoardW <= sandgeom::PANEL_X + sandgeom::PANEL_W,
              "board must sit inside its housing");
static_assert(kBoardY + kCellH <= sandgeom::PANEL_Y + sandgeom::PANEL_H,
              "board must sit inside its housing");

/// The state label, also at kScale: a 6x9 caption under a 26x52 glyph reads as
/// an afterthought rather than as part of the readout.
constexpr int16_t kLabelH = 9 * kScale; // TERM_6X9, magnified

/// Where the state label sits, under the board and still inside the housing.
constexpr int16_t kLabelY = kBoardY + kCellH + 4; // 86
static_assert(kLabelY + kLabelH + 2 <= sandgeom::PANEL_Y + sandgeom::PANEL_H,
              "the label must not spill out through the panel's bottom rail");

/// Descending, digits only, and NO separator. See the header -- an ascending or
/// alphanumeric sequence makes every countdown tick cost 39 flaps.
///
/// The colon is deliberately absent. The sequence is a CYCLE, so any character
/// in it is one every cell passes through on a wrap: with ':' included, both
/// seconds cells flash a colon at every minute boundary and the board reads
/// "02:::" for about 110 ms, once a minute, forever. Measured, not theorised.
///
/// So the separator is not a flap cell at all -- it never changes, so it is
/// drawn as static ink.
const char kSequence[] = "9876543210";
// Derived, never written by hand. Upstream shipped a default sequence declaring
// 40 for a 41-character literal, which made the last character unreachable --
// findInSequence fell through to index 0 and silently rendered the wrong glyph,
// with no diagnostic. The same typo here would be just as quiet.
constexpr int16_t kSequenceLen = static_cast<int16_t>(sizeof(kSequence) - 1);

/// The seconds-TENS cell only ever displays '0'-'5' -- a minute has no more than
/// 59 seconds. Its own sequence, rather than sharing kSequence, is what turns
/// the minute-boundary wrap 0 -> 5 from five flaps into one: '5' sits right
/// after '0' in THIS cycle, exactly as every other digit sits after its
/// successor. That is the whole fix -- see the header for the symptom it kills.
const char kTensSequence[] = "543210";
constexpr int16_t kTensSequenceLen = static_cast<int16_t>(sizeof(kTensSequence) - 1);

/// Every transition on the board, minutes and seconds alike, now costs exactly
/// one flap (a flip-to-reset excepted -- see TimerFace::restart). One flap
/// every real second is the requested cadence: ~500 ms of animation, then
/// ~500 ms static, with nothing left to outrun the clock.
constexpr uint32_t kMsPerFlap = 500;

/// The shared geometry and cosmetics for a flap unit -- everything but the
/// sequence and the column span, which differ between the mm unit and the two
/// one-cell ss units.
onebit::SplitFlapConfig baseConfig(int16_t col0, int16_t cols) {
    onebit::SplitFlapConfig cfg;
    cfg.font = &onebit::fonts::FLAP_13X26;
    cfg.scale = kScale;
    cfg.cell_width = kCellW;
    cfg.cell_height = kCellH;
    cfg.cols = cols;
    cfg.rows = 1;
    // PANEL-LOCAL, not screen: the board is rendered into the scratch.
    cfg.bounds = {static_cast<int16_t>(kBoardLX + col0 * kCellW), kBoardLY,
                  static_cast<int16_t>(cols * kCellW), kCellH};
    cfg.ms_per_flap = kMsPerFlap;
    cfg.split_line_thickness = 1;
    cfg.cell_borders = true;
    return cfg;
}

/// The two-cell minutes unit, or a one-cell seconds unit -- `seq`/`seq_len`
/// picked per cell so the seconds-tens cell can carry its own five-flap-free
/// sequence.
onebit::SplitFlapConfig makeConfig(int16_t col0, int16_t cols, const char* seq,
                                    int16_t seq_len) {
    onebit::SplitFlapConfig cfg = baseConfig(col0, cols);
    cfg.flap_sequence = seq;
    cfg.sequence_length = seq_len;
    return cfg;
}

/// MM:SS. The board has five cells and cannot grow -- at kScale the five are
/// already 170 px of a 182 px panel, and a sixth would need 204 -- so 99:59 is
/// the ceiling. The dial is capped to match, which makes the clamp below
/// unreachable rather than merely unlikely.
void formatMMSS(uint32_t totalSeconds, char* out, size_t n) {
    const uint32_t m = totalSeconds / 60;
    const uint32_t s = totalSeconds % 60;
    if (m > 99) { std::snprintf(out, n, "99:59"); return; }
    std::snprintf(out, n, "%02lu:%02lu", static_cast<unsigned long>(m),
                  static_cast<unsigned long>(s));
}

// ------------------------------------------------------------- the sand --

/// Grains, by duration. Both ends of the range are constrained, in opposite
/// directions, so no single charge serves them: short timers cannot drain a
/// large charge (the attractor only moves so many grains per tick) and long
/// ones look steppy on a small one.
///
/// The top tier is 2000 for a reason that has now expired, and is left at 2000
/// deliberately anyway. It was 2000 because the lintel was a WALL and took a
/// fifth of the upper chamber: measured capacity with it in place was 2188
/// placed, so 2000 was the largest tier with a real margin against silent
/// truncation. The lintel is out of the wall grid now and capacity is 3722, so
/// the constraint is gone -- but raising the tier is a change to where every
/// grain lands, and doing it here would mix that diff into the housing's. It is
/// its own change.
///
/// One consequence is visible and worth knowing about: at 2000 the chamber is a
/// little over half full, so sand piled against the ceiling is a mound rather
/// than a slab, and the 2x board is wide enough that its outer two cells sit
/// past the mound's shoulders. The polarity gauge reads across the middle three.
int chargeFor(uint64_t durationUs) {
    const uint64_t s = durationUs / 1'000'000ull;
    if (s <= 60) return 400;
    if (s <= 180) return 900;
    return 2000;
}

/// `drawBitmapText` magnified by an integer factor.
///
/// The library has no scaled text call -- `drawBitmapText` takes no scale and
/// only `blit` does -- so this stamps glyph by glyph through the scaled blit,
/// which is exactly what `SplitFlapDisplay::renderCharInCell` does for a
/// resting card. Doing it that way rather than through a magnified scratch
/// buffer means there is no allocation to fail: the readout's own scratch
/// already has to be guarded for that, and this needs no second guard.
///
/// Advance and out-of-range handling mirror `drawBitmapText` exactly, scaled:
/// glyph width plus one pixel of spacing, and a character the font does not
/// have still takes its space. Ink composes with Or, so paper is transparent --
/// again the same as `drawBitmapText` with a BLACK colour.
void drawScaledText(onebit::IFramebuffer& fb, const onebit::BitmapFont& font,
                    int16_t x, int16_t y, const char* text, int16_t scale) {
    int16_t cursor = x;
    for (const char* p = text; *p; ++p) {
        const uint8_t ch = static_cast<uint8_t>(*p);
        int16_t advance = font.glyph_width;
        if (ch >= font.first_char && ch <= font.last_char) {
            const onebit::BitmapGlyph& g = font.glyphs[ch - font.first_char];
            if (font.glyph_width == 0) advance = static_cast<int16_t>(g.width);
            onebit::blit(fb, cursor, y,
                         onebit::BitmapView::tight(g.data, g.width, g.height),
                         onebit::Rect{0, 0, static_cast<int16_t>(g.width),
                                      static_cast<int16_t>(g.height)},
                         scale, scale, onebit::RasterOp::Or);
        }
        cursor = static_cast<int16_t>(cursor + (advance + 1) * scale);
    }
}

/// The magnified width of `text`, matching drawScaledText's advance.
int16_t scaledTextWidth(const onebit::BitmapFont& font, const char* text, int16_t scale) {
    return static_cast<int16_t>(onebit::getBitmapTextWidth(font, text) * scale);
}

/// Draw the separator cell exactly as the library draws a flap cell -- 1 px
/// outline, centre split line, centred glyph -- so it is indistinguishable from
/// its neighbours. It just never moves.
///
/// The colon has to be magnified along with everything else, and by the same
/// centring arithmetic `renderCharInCell` uses, or the one cell the library
/// does not draw would be the one cell that looks wrong: a 13x26 colon adrift
/// in a 34x60 box between two 26x52 digits.
///
/// Draws in PANEL-LOCAL coordinates, like the flap units it sits between: `fb`
/// here is the scratch, not the frame.
void drawSeparator(onebit::IFramebuffer& fb) {
    const int16_t px = kBoardLX + kSepCol * kCellW;
    onebit::drawRect(fb, px, kBoardLY, kCellW, kCellH, BLACK);
    onebit::drawLine(fb, px, static_cast<int16_t>(kBoardLY + kCellH / 2),
                     static_cast<int16_t>(px + kCellW - 1),
                     static_cast<int16_t>(kBoardLY + kCellH / 2), BLACK);
    const int16_t w = scaledTextWidth(onebit::fonts::FLAP_13X26, ":", kScale);
    drawScaledText(fb, onebit::fonts::FLAP_13X26,
                   static_cast<int16_t>(px + (kCellW - w) / 2),
                   static_cast<int16_t>(kBoardLY + (kCellH - kGlyphH * kScale) / 2), ":",
                   kScale);
}

/// Coverage above which a cell inverts, and below which it reverts.
///
/// Two thresholds, not one. Grains jitter at a boundary -- a cell sitting at the
/// sand's surface gains and loses a few every tick -- and a bare threshold makes
/// that jitter a polarity flip on consecutive frames, which reads as a fault
/// rather than as a gauge. The gap is what turns a noisy measurement into a
/// latched state.
constexpr int kCoverOnPct = 60;
constexpr int kCoverOffPct = 40;
static_assert(kCoverOffPct < kCoverOnPct, "the hysteresis band must not be empty");

static_assert(TimerFace::kCells == kCols, "the polarity latch must have one slot per cell");

} // namespace

Rect16 TimerFace::cellRect(int col) const {
    if (col < 0 || col >= kCells) return Rect16{0, 0, 0, 0};
    return Rect16{static_cast<int16_t>(kBoardX + col * kCellW), kBoardY, kCellW, kCellH};
}

void TimerFace::updateCoverage() {
    for (int c = 0; c < kCells; ++c) {
        const Rect16 r = cellRect(c);
        // The cell's pixel rect in GRID space: every grid cell the rect TOUCHES,
        // taken from its first and last pixel. A grid cell is SCALE px, so a
        // rect that begins or ends part-way through one is still standing on
        // that grain and has to count it.
        const int cx0 = (r.x - sandgeom::ORIGIN_X) / sandgeom::SCALE;
        const int cy0 = (r.y - sandgeom::ORIGIN_Y) / sandgeom::SCALE;
        const int cx1 = (r.x + r.w - 1 - sandgeom::ORIGIN_X) / sandgeom::SCALE;
        const int cy1 = (r.y + r.h - 1 - sandgeom::ORIGIN_Y) / sandgeom::SCALE;

        int occupied = 0, total = 0;
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                ++total;
                if (vessel_.sand().get(cx, cy)) ++occupied;
            }
        }
        const int pct = total ? (occupied * 100) / total : 0;
        if (!covered_[c] && pct > kCoverOnPct) covered_[c] = true;
        else if (covered_[c] && pct < kCoverOffPct) covered_[c] = false;
    }
}

TimerFace::TimerFace()
    : mins_(makeConfig(0, 2, kSequence, kSequenceLen)),
      secsTens_(makeConfig(3, 1, kTensSequence, kTensSequenceLen)),
      secsUnits_(makeConfig(4, 1, kSequence, kSequenceLen)) {}

void TimerFace::restart(const TimerModel& t, uint32_t seed) {
    vessel_.begin();
    vessel_.reset(seed, chargeFor(t.duration()));
    lastGen_ = t.generation();
    started_ = true;
}

void TimerFace::tick(const TimerModel& t, uint64_t now) {
    if (!started_ || t.generation() != lastGen_) {
        // A new run needs a new charge. Watching the generation rather than the
        // duration is what catches a flip: it resets the timer to full without
        // touching the duration, so a duration watch left the sand drained while
        // the clock ran from full.
        restart(t, static_cast<uint32_t>(now | 1u));
        lastTickUs_ = now;
        return;
    }

    // A zero duration reads as fraction 0.0, which would open the gate and pour
    // the sand while the board reads 00:00 SET. Clamping to full shuts the gate
    // and leaves the hourglass static, which is the honest picture.
    const float f = (t.duration() == 0) ? 1.0f : t.fraction(now);

    // Fixed-rate, catching up at most a few ticks. Without a cap a long stall --
    // a slow frame, a bus recovery -- would try to replay the whole gap at once
    // and stall the UI further.
    if (now < lastTickUs_) lastTickUs_ = now;
    int budget = 3;
    while (now - lastTickUs_ >= tickPeriodUs_ && budget-- > 0) {
        lastTickUs_ += tickPeriodUs_;
        vessel_.tick(f);
    }
    if (now - lastTickUs_ > tickPeriodUs_ * 8) lastTickUs_ = now; // gave up catching up
}

void TimerFace::setTickHz(uint16_t hz) {
    tickPeriodUs_ = (hz == 0) ? kTickPeriodUs : (1'000'000ull / hz);
}

void TimerFace::render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) {
    // Board state first: it touches no pixels, and doing it here keeps the
    // drawing sequence below unbroken.
    const uint32_t remaining = t.remainingSeconds(now);
    char want[8];
    formatMMSS(remaining, want, sizeof(want));
    if (std::strcmp(want, shown_) != 0) {
        char mm[3] = {want[0], want[1], 0};
        char st[2] = {want[3], 0};
        char su[2] = {want[4], 0};
        mins_.setRow(0, mm);
        secsTens_.setRow(0, st);
        secsUnits_.setRow(0, su);
        std::snprintf(shown_, sizeof(shown_), "%s", want);
    }

    // Animate a tick, snap a jump. A real split-flap board cascades because
    // each flap is a physical card that must fall; this one is drawn, and the
    // only thing worth animating is the passage of a second. A reset, a
    // resume, an expiry, or a skipped second is not that -- it is a
    // discontinuity, and the target keeps moving as the clock counts down
    // underneath a cascading board, so animating it means showing a value
    // that is not the time for several seconds while the board chases a
    // moving target and even overshoots (minutes climbing the long way up
    // before coming back down). So: exactly one second less than what was
    // last shown gets the normal capped delta and animates one flap; anything
    // else -- including the first frame, via `settled_` -- snaps.
    //
    // The test is "the readout did not JUMP", which includes the frames where
    // it did not move at all. That distinction is the whole bug this predicate
    // was first written with: a second changes on one frame in thirty, so
    // requiring a difference of exactly 1 snapped the other twenty-nine, and
    // the flap that began on the tick frame was slammed shut before the next
    // frame reached the glass. The readout, the end state and the goldens were
    // all correct and the animation was invisible.
    const bool continuous = settled_ && lastShownSeconds_ != UINT32_MAX &&
                            lastShownSeconds_ >= remaining &&
                            (lastShownSeconds_ - remaining) <= 1;

    // Derive the animation delta from the caller's clock rather than counting
    // frames, so a tick's flap runs at a real cadence regardless of render
    // rate.
    uint32_t deltaMs = 0;
    if (!settled_) {
        // Snap on the first frame instead of cascading through the sequence from
        // a cold start -- update() catches up in one call, so a large delta
        // settles the board rather than animating it.
        deltaMs = 5000;
        settled_ = true;
    } else if (continuous) {
        if (now > lastNow_) {
            const uint64_t d = (now - lastNow_) / 1000ull;
            deltaMs = (d > 1000ull) ? 1000u : static_cast<uint32_t>(d);
        }
        // else: the clock did not advance -- deltaMs stays 0, no animation
        // this frame, same as before this change.
    } else {
        deltaMs = 5000; // a jump, not a tick -- snap rather than cascade
    }
    lastNow_ = now;
    lastShownSeconds_ = remaining;
    mins_.update(deltaMs);
    secsTens_.update(deltaMs);
    secsUnits_.update(deltaMs);

    // The draw order is not negotiable. renderSand ASSIGNS raw bytes across the
    // whole safe box, so anything drawn before it is annihilated silently.
    fb.clear(WHITE);
    renderSand(fb, vessel_.sand(), vessel_.walls());

    if (t.state() == TimerModel::State::Expired) invertSafeBox(fb);

    // The panel is opaque, and that is now the WHOLE legibility guarantee.
    // The lintel used to be a wall: sand could not be behind the readout, and
    // renderSand -- which ASSIGNS bytes -- repainted the interior white for
    // free. Sand fills this region now, so every pixel of the panel is this
    // function's responsibility, and it must land after renderSand and after
    // the Expired invert or it is annihilated by them.
    onebit::fillRect(fb, sandgeom::PANEL_X, sandgeom::PANEL_Y,
                     sandgeom::PANEL_W, sandgeom::PANEL_H, WHITE);
    onebit::drawRect(fb, sandgeom::PANEL_X, sandgeom::PANEL_Y,
                     sandgeom::PANEL_W, sandgeom::PANEL_H, BLACK);

    // Which cells have sand behind them. From the grid, before anything is
    // drawn -- the frame at this point is sand plus a white panel, and no pixel
    // measurement of it could tell a grain from the readout's own ink.
    updateCoverage();

    // The board is drawn on paper, and then each cell is stamped down either
    // way up.
    //
    // It cannot be done by compositing the widget with a different raster op:
    // the widget writes ink with Or, but a cell mid-flip CLEARS ITS CARD TO
    // WHITE to occlude what it passes over, and under Xor that white would come
    // out as "show the sand through the falling card" -- a hole in the readout,
    // once per flap, exactly when the eye is on it. So the polarity is applied
    // to the finished cell, not to the drawing of it.
    //
    // No clear inside the loop, either: the scratch is cleared once here, which
    // is the same white page the widget used to draw onto directly.
    //
    // A side effect worth naming, since a test pins it: the widget's white
    // occlusion clear can no longer reach the sand AT ALL. It used to be safe
    // only because it stayed inside the knockout; now it cannot leave this
    // buffer.
    //
    // GUARDED, and this branch is load-bearing. `onebit::Framebuffer` allocates
    // its storage on the heap and has no error path: a failed allocation leaves
    // it valid-looking but empty, reporting that only through `isValid()`, and
    // silently discarding every draw. A covered cell is stamped in TWO steps
    // and only the second one reads the scratch -- so with an invalid scratch
    // the black fill below would still land and the Xor that was going to turn
    // it into the cell's complement would do nothing, leaving a solid black
    // slab. That is precisely the black-on-black failure this whole design
    // exists to prevent, reached from the other side: not sand showing through
    // the readout, but the readout painting itself out. 2,116 bytes is a small
    // ask and it will almost always succeed; "almost always" is not a
    // legibility guarantee.
    //
    // Blank is the right degradation. The panel is already white paper with a
    // border at this point, so the failure reads as a readout with no digits --
    // obviously broken rather than quietly unreadable -- and the sand behind it
    // is untouched.
    if (scratch_.isValid()) {
        scratch_.clear(WHITE);
        mins_.render(scratch_);
        drawSeparator(scratch_);
        secsTens_.render(scratch_);
        secsUnits_.render(scratch_);

        for (int c = 0; c < kCells; ++c) {
            const Rect16 r = cellRect(c);
            const onebit::Rect src{static_cast<int16_t>(r.x - sandgeom::PANEL_X),
                                   static_cast<int16_t>(r.y - sandgeom::PANEL_Y), r.w, r.h};
            if (covered_[c]) {
                // Black paper, then Xor: 1 ^ src == ~src, so the cell lands as
                // the exact complement of itself -- glyph, border, split line
                // and background all flipped together. Inverting a finished
                // rect is the one thing `invertSafeBox` does, and it does it
                // only for the safe box, byte-aligned; rather than write a
                // second inverter for arbitrary rects, this gets the same
                // result out of the blit that has to happen anyway.
                onebit::fillRect(fb, r.x, r.y, r.w, r.h, BLACK);
                onebit::blit(fb, r.x, r.y, scratch_, src, onebit::RasterOp::Xor);
            } else {
                onebit::blit(fb, r.x, r.y, scratch_, src, onebit::RasterOp::Copy);
            }
        }
    }

    const char* label = nullptr;
    switch (t.state()) {
        case TimerModel::State::Idle:    label = "SET"; break;
        case TimerModel::State::Running: label = nullptr; break;
        case TimerModel::State::Paused:  label = "PAUSED"; break;
        case TimerModel::State::Expired: label = "DONE"; break;
    }
    if (label) {
        // Centred on x = 120, which is the panel's centre line as well as the
        // screen's: PANEL_X 29 + PANEL_W / 2 91 = 120. The widest label,
        // "PAUSED", is 82 px at kScale against 182 px of panel, so it never
        // reaches the rails.
        //
        // Normal polarity, deliberately. The label is panel chrome, not a flap
        // cell: it sits below the board, outside every cellRect, so it is not
        // stamped with the cells and does not invert with them. Inverting it
        // would put the one piece of text that says what the timer is DOING on
        // ground that shifts with the sand level.
        const int16_t w = scaledTextWidth(onebit::fonts::TERM_6X9, label, kScale);
        drawScaledText(fb, onebit::fonts::TERM_6X9,
                       static_cast<int16_t>(120 - w / 2), kLabelY, label, kScale);
    }

    // A muted buzzer is otherwise discoverable only by the absence of a sound,
    // which is indistinguishable from a gesture the device did not see.
    //
    // Top-left of the safe box. At y = 18 the corner disc's visible x begins at
    // 44 - sqrt(44^2 - 26^2) ~= 8.5, so x = 18 clears with ~10 px to spare.
    //
    // Drawn last, deliberately: renderSand ASSIGNS raw bytes across the whole
    // safe box (see the note above), so anything drawn before it is
    // annihilated silently. This must stay the final thing render() does.
    if (muted_) {
        constexpr int16_t GX = h0::safe::X + 2; // 18
        constexpr int16_t GY = h0::safe::Y + 2; // 18
        // The cone: a 4 px body with a 3 px flare, 9 px tall overall.
        onebit::fillRect(fb, GX, static_cast<int16_t>(GY + 3), 3, 3, BLACK);
        for (int16_t i = 0; i < 4; ++i) {
            onebit::drawLine(fb, static_cast<int16_t>(GX + 3 + i),
                             static_cast<int16_t>(GY + 3 - i),
                             static_cast<int16_t>(GX + 3 + i),
                             static_cast<int16_t>(GY + 5 + i), BLACK);
        }
        // The bar through it, which is what says "off" rather than "sound".
        onebit::drawLine(fb, GX, GY, static_cast<int16_t>(GX + 8),
                         static_cast<int16_t>(GY + 8), BLACK);
    }
}

} // namespace h0
