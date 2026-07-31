#pragma once

#include <1bit/core/framebuffer.hpp>
#include <1bit/signage/split_flap.hpp>

#include <cstdint>

#include "faces/layout.hpp"
#include "sand/agitation.hpp"
#include "sand/sand_vessel.hpp"
#include "timer/timer_model.hpp"

namespace h0 {

/// The timer: a split-flap readout housed in a lintel, over falling sand.
///
/// One face, not three. The sand carries the *feel* of the time passing and the
/// board carries the *number*, and neither can do the other's job: sand alone
/// cannot tell you it is 4:07, and a readout alone is a clock.
///
/// **The readout is legible because it is a hole in a wall.** The lintel is part
/// of the simulation's wall grid, so no grain can be inside it, and `renderSand`
/// assigns rather than or-s -- so the interior is repainted white every frame at
/// no cost. At one bit there is no other way to keep black glyphs off black
/// sand that does not amount to drawing the readout twice.
///
/// **The flap sequence is load-bearing.** `SplitFlapDisplay` steps one character
/// at a time, forward only, so a transition costs `(index(to) - index(from)) mod
/// length`. With the library's default alphanumeric sequence a digit *decrement*
/// -- what a countdown does every second -- costs 39 flaps, 5.19 s at the
/// default cadence. The board never lands, and what it shows instead of digits
/// is letters. A descending digits-only sequence makes a decrement one flap, and
/// the worst transition becomes the seconds-tens wrap 0 -> 5 at five.
///
/// The same reasoning excludes the colon from the sequence, and therefore splits
/// the readout into two two-cell units: a cycle containing ':' is a ':' every
/// digit cell passes through when it wraps.
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
        return (col < 2) ? mins_.getCurrentChar(col, 0)
                         : secs_.getCurrentChar(static_cast<int16_t>(col - 3), 0);
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

    SandVessel vessel_;
    Agitation agitation_;

    // Two independent two-cell units with a painted separator between them,
    // rather than one five-cell board. A flap sequence is a cycle, so a colon
    // in it is a colon every digit cell passes through on a wrap.
    onebit::SplitFlapDisplay mins_;
    onebit::SplitFlapDisplay secs_;

    bool started_ = false;
    uint32_t lastGen_ = 0;
    uint64_t lastTickUs_ = 0;
    uint64_t tickPeriodUs_ = kTickPeriodUs;

    uint64_t lastNow_ = 0; ///< for deriving the flap animation delta
    bool settled_ = false; ///< first frame snaps rather than cascading
    char shown_[8] = {0};  ///< last string handed to the board
};

} // namespace h0
