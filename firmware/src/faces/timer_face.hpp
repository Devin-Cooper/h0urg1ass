#pragma once

#include <1bit/core/framebuffer.hpp>
#include <1bit/signage/split_flap.hpp>

#include <cstdint>

#include "faces/layout.hpp"
#include "sand/agitation.hpp"
#include "sand/sand_render.hpp"
#include "sand/sand_vessel.hpp"
#include "timer/timer_model.hpp"

namespace h0 {

/// A pixel rect. `onebit::Rect` is already exactly this -- x/y/w/h as int16_t,
/// with contains() and intersected() on top -- so this is an alias rather than a
/// second rect type that would need converting at every library call.
using Rect16 = onebit::Rect;

/// The timer: a split-flap readout housed in a lintel, over falling sand.
///
/// One face, not three. The sand carries the *feel* of the time passing and the
/// board carries the *number*, and neither can do the other's job: sand alone
/// cannot tell you it is 4:07, and a readout alone is a clock.
///
/// **The readout is legible because it is opaque and drawn last.** It used to be
/// a hole in a wall: the lintel was part of the simulation's wall grid, so no
/// grain could be inside it, and `renderSand` assigns rather than or-s, which
/// repainted the interior white every frame at no cost. Black glyphs on black
/// sand was not a drawing problem, it was physically impossible.
///
/// Sand fills that region now, and the guarantee is bought with draw order
/// instead: the panel is composited AFTER the sand and after the expiry invert,
/// so every pixel inside it is this class's responsibility.
///
/// **A cell whose sand is behind it is drawn inverted**, which is what keeps the
/// contrast total instead of letting the readout compete with what is behind it
/// -- and makes the board a coarse gauge as well as a clock. The board is
/// rendered to a panel-sized scratch at normal polarity and each cell is then
/// stamped either way up; it cannot be a raster-op swap, because a flipping cell
/// writes WHITE to occlude its falling card and an Xor would turn that white
/// into "show the sand through the card".
///
/// **The flap sequence is load-bearing.** `SplitFlapDisplay` steps one character
/// at a time, forward only, so a transition costs `(index(to) - index(from)) mod
/// length`. With the library's default alphanumeric sequence a digit *decrement*
/// -- what a countdown does every second -- costs 39 flaps, 5.19 s at the
/// default cadence. The board never lands, and what it shows instead of digits
/// is letters. A descending digits-only sequence makes a decrement one flap.
///
/// That leaves one exception: the seconds-tens digit only ever holds '0'-'5',
/// and at a minute boundary it wraps 0 -> 5 -- five flaps in a ten-character
/// sequence, which no longer lands inside the one second it has to run in. The
/// fix is not a faster cadence, it is a smaller cycle: the seconds-tens cell
/// gets its own six-character sequence, in which '5' immediately follows '0', so
/// that wrap costs one flap like every other transition on the board. That is
/// why the seconds pair is two independent ONE-cell units rather than one
/// two-cell unit -- only the tens cell needs the different sequence, and a
/// shared cell cannot carry two.
///
/// The same reasoning excludes the colon from every sequence, and therefore the
/// separator is a fourth, unmoving unit: a cycle containing ':' is a ':' every
/// digit cell passes through when it wraps.
///
/// With every transition costing exactly one flap, `ms_per_flap` sets the whole
/// board's cadence directly: ~500 ms of animation once a second, then static
/// for the rest of it, with no cell left catching up.
class TimerFace {
public:
    TimerFace();

    /// Draw sand, then knock out the housing, then the board. `now` is passed
    /// rather than read so a frame can be rendered off-line at a chosen instant.
    void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now);

    /// Advance the simulation. Called on its own clock, NOT once per render:
    /// the sand must fall at a fixed rate regardless of how often the display
    /// happens to be redrawn.
    void tick(const TimerModel& t, uint64_t now);

    /// Simulation rate. Reduced while the screen is blanked.
    ///
    /// NOT achievable by calling tick() less often: it catches up at most 3
    /// ticks per call, so an 8 Hz call site still runs 24 ticks a second. The
    /// drain gate is a proportional controller on a cumulative count, so it
    /// self-adjusts to whatever rate it is given -- at 5 Hz the ceiling is
    /// 25 grains/s against the 6.7 the longest tier needs.
    void setTickHz(uint16_t hz);

    /// A muted buzzer is otherwise discoverable only by the absence of a
    /// sound, which is indistinguishable from a gesture the device did not
    /// see. Drawing a small glyph is what makes the state visible instead.
    void setMuted(bool m) { muted_ = m; }

    /// Recharge and restart the drain.
    void restart(const TimerModel& t, uint32_t seed);

    /// Feed the filtered accelerometer vector, in PANEL axes, once per tick.
    ///
    /// Takes the vector rather than a direction so the sub-sector remainder
    /// survives, and derives its own agitation so the sand's responsiveness is
    /// gated on the device being HANDLED rather than merely being at an angle.
    void setTilt(const Vec3& g) {
        agitation_.update(g);
        vessel_.setTilt(g.x, g.y, agitation_.value());
    }

    /// Direct control, for tests and callers with no accelerometer.
    void setGravity(Gravity g) { vessel_.setGravity(g); }

    float agitation() const { return agitation_.value(); }

    /// Character currently shown in cell `col`, indexed across the whole MM:SS
    /// readout. Exposed so a test can assert the board actually reaches its
    /// target within a tick, which a pixel count cannot distinguish from a board
    /// stuck mid-cascade showing the wrong glyph.
    ///
    /// Column 2 is the separator, which is painted rather than flapped.
    char boardChar(int16_t col) const {
        if (col == 2) return ':';
        if (col < 2) return mins_.getCurrentChar(col, 0);
        return (col == 3) ? secsTens_.getCurrentChar(0, 0) : secsUnits_.getCurrentChar(0, 0);
    }

    /// True while any cell is part-way through a flap. Exposed because the
    /// animation is only visible if this stays true across many CONSECUTIVE
    /// frames -- a board that reaches its target within a single frame renders
    /// a correct readout and correct goldens while showing the user nothing.
    /// Character- and pixel-level assertions both miss that; this does not.
    bool boardFlipping() const {
        return mins_.isFlipping() || secsTens_.isFlipping() || secsUnits_.isFlipping();
    }

    /// How many cells the board has, separator included. The separator is a
    /// cell for polarity purposes even though it never flaps: it is inside the
    /// board's ink box, so leaving it out would leave a stripe of the opposite
    /// polarity through the middle of the readout.
    static constexpr int kCells = 5;

    /// The pixel rect of cell `col`, in MAIN-framebuffer coordinates.
    ///
    /// The board itself is drawn into a panel-local scratch, so the widget's
    /// own bounds are panel-relative; this converts back, because every caller
    /// outside this class -- and every test -- means screen pixels by a rect.
    /// An out-of-range column gives an empty rect rather than a wild one.
    Rect16 cellRect(int col) const;

    /// Whether cell `col` is currently stamped inverted because sand is behind
    /// it. Latched with hysteresis in render(); see updateCoverage().
    bool cellCovered(int col) const {
        return (col >= 0 && col < kCells) ? covered_[col] : false;
    }

    /// Grid-space accessors. Framebuffer measurements cannot separate stranded
    /// sand from lintel ink, so the sand invariants are asserted here instead.
    const SandGrid& sand() const { return vessel_.sand(); }
    int charge() const { return vessel_.charge(); }
    int lowerCount() const { return vessel_.lowerCount(); }

private:
    /// Simulation rate. One tick costs about 4.5 ms on this part, so 30 Hz is
    /// roughly 13% of the CPU -- affordable, and enough for the drain to look
    /// continuous. The default, restored whenever setTickHz(0) is called.
    static constexpr uint64_t kTickPeriodUs = 33'333;

    /// Recompute `covered_` from the sand grid. Called once per render, before
    /// anything is stamped.
    void updateCoverage();

    SandVessel vessel_;
    Agitation agitation_;

    /// The board is drawn here first, at normal polarity, and then stamped into
    /// the frame a cell at a time -- straight, or complemented where sand is
    /// behind that cell. Panel-sized, so a cell's source rect is just its
    /// screen rect less the panel origin.
    ///
    /// A member rather than a local: 182 x 92 is 2,116 bytes, which has no
    /// business on the stack of a function the render loop calls every frame.
    /// A member rather than a file-scope static, too -- its lifetime is the
    /// face's, and there is no shared mutable state to reason about.
    ///
    /// Those 2,116 bytes come off the heap and the constructor has no way to
    /// report failing to get them, so `render()` checks `isValid()` before it
    /// draws. It must: a covered cell is a black fill followed by an Xor blit,
    /// and an invalid scratch swallows the blit while the fill still lands.
    onebit::Framebuffer<sandgeom::PANEL_W, sandgeom::PANEL_H> scratch_;

    /// Per-cell polarity, latched. Derived from the SAND GRID rather than from
    /// pixels: a framebuffer measurement cannot separate a grain from the
    /// readout's own ink, which is the same argument the grid accessors above
    /// are here for.
    bool covered_[kCells] = {};

    // Three independent flap units with a painted separator between them,
    // rather than one five-cell board. A flap sequence is a cycle, so a colon
    // in it is a colon every digit cell passes through on a wrap -- and the
    // seconds-tens cell needs a sequence of its own (see the class comment),
    // which a shared two-cell seconds unit could not carry.
    onebit::SplitFlapDisplay mins_;
    onebit::SplitFlapDisplay secsTens_;
    onebit::SplitFlapDisplay secsUnits_;

    bool started_ = false;
    bool muted_ = false;
    uint32_t lastGen_ = 0;
    uint64_t lastTickUs_ = 0;
    uint64_t tickPeriodUs_ = kTickPeriodUs;

    uint64_t lastNow_ = 0; ///< for deriving the flap animation delta
    bool settled_ = false; ///< first frame snaps rather than cascading
    char shown_[8] = {0};  ///< last string handed to the board

    /// Whole seconds remaining as of the last render() call. UINT32_MAX means
    /// "nothing shown yet" -- unreachable otherwise, since the dial is capped
    /// at 99:59. Compared against the CURRENT remaining seconds each call to
    /// tell an ordinary one-second tick (animate) from a discontinuity: a
    /// reset, a resume, an expiry, or a skipped second (snap). See render().
    uint32_t lastShownSeconds_ = UINT32_MAX;
};

} // namespace h0
