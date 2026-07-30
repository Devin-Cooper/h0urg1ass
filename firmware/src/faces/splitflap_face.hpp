#pragma once

#include <1bit/signage/split_flap.hpp>

#include "faces/face.hpp"

namespace h0 {

/// A mechanical split-flap countdown, of a piece with an hourglass: both are
/// devices that show time by physically moving.
///
/// **The flap sequence is load-bearing.** `SplitFlapDisplay` steps one character
/// at a time, forward only, so the cost of a transition is
/// `(index(to) - index(from)) mod length`. With the library's default
/// alphanumeric sequence a digit *decrement* -- which is what a countdown does
/// every single second -- costs 39 flaps, or 5.19 s at the default cadence. The
/// face never lands: it permanently chases a target moving away five times
/// faster than it can travel, and what it actually displays is letters.
///
/// A **descending digits-only sequence** makes a decrement exactly one flap. The
/// worst transition in the whole face becomes the seconds-tens wrap `0 -> 5`, at
/// six flaps. That single line is the entire difference between working and
/// broken.
///
/// Size is the widget's hard limit: `FLAP_13X26` is a fixed raster with no scale
/// factor, so a larger cell buys white space around a 13x26 glyph rather than a
/// larger digit. This face is therefore a compact readout by construction --
/// which suits a departure-board look, but cannot fill the panel the way the
/// digits face does.
class SplitFlapFace : public IFace {
public:
    SplitFlapFace();

    void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) override;
    const char* name() const override { return "splitflap"; }

    /// Character currently shown in cell `col`. Exposed so a test can assert the
    /// board actually reaches its target within a tick, which a pixel count
    /// cannot distinguish from a board stuck mid-cascade showing letters.
    char boardChar(int16_t col) const { return board_.getCurrentChar(col, 0); }

private:
    onebit::SplitFlapDisplay board_;
    uint64_t lastNow_ = 0;   ///< for deriving the animation delta
    bool settled_ = false;   ///< first frame snaps rather than cascading
    char shown_[8] = {0};    ///< last string handed to the board
};

} // namespace h0
