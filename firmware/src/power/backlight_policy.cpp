#include "power/backlight_policy.hpp"

namespace h0 {

BacklightState backlightFor(const Settings& s, uint64_t idleUs, bool alarmSounding) {
    if (alarmSounding) return {s.backlightActive, true};

    if (s.dimAfterS == 0) return {s.backlightActive, true}; // NEVER dim
    const uint64_t dimAt = static_cast<uint64_t>(s.dimAfterS) * 1'000'000ull;
    if (idleUs < dimAt) return {s.backlightActive, true};

    if (s.blankAfterS == 0) return {s.backlightDim, true}; // NEVER blank
    const uint64_t blankAt = dimAt + static_cast<uint64_t>(s.blankAfterS) * 1'000'000ull;
    if (idleUs < blankAt) return {s.backlightDim, true};

    return {0, false};
}

} // namespace h0
