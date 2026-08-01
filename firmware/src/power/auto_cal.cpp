#include "power/auto_cal.hpp"

#include "settings/settings.hpp" // kCalMin, kCalMax

namespace h0 {

bool AutoCal::shouldStore(uint16_t learned, uint16_t stored, bool autoArmed) {
    if (!autoArmed) return false;
    const int32_t d = static_cast<int32_t>(learned) - static_cast<int32_t>(stored);
    return d > kCalDeadband || d < -kCalDeadband;
}

void AutoCal::endSession() {
    charging_ = false;
    anchored_ = false;
    min_ = flatRef_;
    peak_ = flatRef_;
}

uint16_t AutoCal::push(uint16_t rawMv) {
    if (!primed_) {
        primed_ = true;
        min_ = rawMv;
        peak_ = rawMv;
        flatRef_ = rawMv;
        flatFor_ = 0;
        return 0;
    }

    if (rawMv < min_) min_ = rawMv;
    if (rawMv > peak_) peak_ = rawMv;

    // The rise. Measured from the minimum since the last session ended, so a
    // device booted mid-CV sees no rise and simply waits for the next cycle.
    if (!charging_ && rawMv >= static_cast<uint32_t>(min_) + kRiseMv) {
        charging_ = true;
        peak_ = rawMv;
    }

    // The fall. Charge terminated or the cable came out; either way the
    // session is over and the next rise may anchor again.
    if (charging_ && static_cast<uint32_t>(rawMv) + kFallMv <= peak_) {
        flatRef_ = rawMv;
        flatFor_ = 0;
        endSession();
        return 0;
    }

    // The plateau. Any step outside the band re-centres it and restarts the
    // count -- so a slow ramp never accumulates a window.
    const int32_t d = static_cast<int32_t>(rawMv) - static_cast<int32_t>(flatRef_);
    if (d > kFlatMv || d < -kFlatMv) {
        flatRef_ = rawMv;
        flatFor_ = 0;
        return 0;
    }
    ++flatFor_;

    if (!charging_ || anchored_ || flatFor_ < kFlatSamples) return 0;

    // Anchor. Once per session, whatever happens afterwards: a device left
    // plugged in must not re-emit the same gain every second and drive the
    // caller to write flash forever.
    anchored_ = true;
    if (rawMv == 0) return 0;
    const uint32_t permille = (1000u * kCvMv) / rawMv;

    // Refused rather than clamped. A board needing more than this range has a
    // divider or an LDO out of spec, and clamping would call it calibrated.
    //
    // <= and >=, matching settings_face.cpp's AT LIMIT test exactly. With
    // strict < and > this could store precisely 850 or 1150, and the CAL row
    // would then call the gain out of spec -- while showing neither AUTO nor
    // MAN, which is the only warning that row carries. The endpoints mean "no
    // headroom left" either way, so refusing them here is the cheaper side of
    // the agreement; a hand-set gain may still sit on one, and the row says
    // AT LIMIT about it, which is the truth.
    if (permille <= kCalMin || permille >= kCalMax) return 0;
    return static_cast<uint16_t>(permille);
}

} // namespace h0
