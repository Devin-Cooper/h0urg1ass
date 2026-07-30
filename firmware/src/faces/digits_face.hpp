#pragma once

#include "faces/face.hpp"

namespace h0 {

/// Big MM:SS digits -- the legibility face and the fallback for every state.
///
/// Uses the stroked vector font rather than a bitmap one, because the largest
/// bitmap cell available is 26 px and a glanceable countdown needs roughly
/// twice that. Vector glyphs are polylines in a 0-100 space, so the size is a
/// free parameter and `strokeWidth` doubles as a weight axis.
class DigitsFace : public IFace {
public:
    void render(onebit::IFramebuffer& fb, const TimerModel& t, uint64_t now) override;
    const char* name() const override { return "digits"; }
    bool supports(const TimerModel& t) const override;
};

} // namespace h0
