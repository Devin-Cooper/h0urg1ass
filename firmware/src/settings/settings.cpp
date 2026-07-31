#include "settings/settings.hpp"

namespace h0 {

namespace {

template <typename T>
T clampTo(T v, T lo, T hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

void clamp(Settings& s) {
    s.version = Settings::kVersion;

    if (s.themeId >= static_cast<uint8_t>(ThemeId::Count)) s.themeId = 0;

    s.backlightActive = clampTo<uint8_t>(s.backlightActive, kBacklightFloor, 255);

    // Order matters: the dim level is clamped against the ALREADY-clamped
    // active level, or a garbage active value would drag the dim level with it.
    s.backlightDim = clampTo<uint8_t>(s.backlightDim, 1, s.backlightActive);

    if (s.dimAfterS > 600) s.dimAfterS = 20;
    if (s.blankAfterS > 600) s.blankAfterS = 60;
    s.alarmS = clampTo<uint16_t>(s.alarmS, 15, 300);
    if (s.mute > 1) s.mute = 0;
    s.batCalPermille = clampTo<uint16_t>(s.batCalPermille, kCalMin, kCalMax);
}

bool operator==(const Settings& a, const Settings& b) {
    return a.version == b.version && a.themeId == b.themeId &&
           a.backlightActive == b.backlightActive && a.backlightDim == b.backlightDim &&
           a.dimAfterS == b.dimAfterS && a.blankAfterS == b.blankAfterS &&
           a.alarmS == b.alarmS && a.mute == b.mute &&
           a.batCalPermille == b.batCalPermille;
}

} // namespace h0
