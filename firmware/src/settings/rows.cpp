#include "settings/rows.hpp"

namespace h0 {

namespace {

// Value ladders. These are raw stored values, not display strings -- formatting
// lives in settings_face.cpp so this file stays free of rendering concerns.
constexpr uint8_t  kBright[]  = {16, 24, 32, 48, 64, 96, 128, 192, 255};
constexpr uint8_t  kDimTo[]   = {4, 8, 12, 16, 24, 36};
constexpr uint16_t kDimAt[]   = {10, 20, 30, 60, 120, 0}; // 0 = never, and it is LAST
constexpr uint16_t kBlankAt[] = {30, 60, 120, 300, 0};
constexpr uint16_t kOffAt[]   = {120, 300, 600, 1800, 0}; // 0 = never, and it is LAST
constexpr uint16_t kAlarm[]   = {15, 30, 60, 120, 300};

constexpr uint8_t kThemeCount = static_cast<uint8_t>(ThemeId::Count);
// One AUTO position plus the 151 gains. AUTO is index 0 so that the row reads
// as "automatic, or pick a number", and so that the numeric range keeps its
// natural order.
constexpr uint8_t kCalCount = static_cast<uint8_t>((kCalMax - kCalMin) / 2 + 2); // 152

template <typename T, uint8_t N>
uint8_t nearestIndex(const T (&table)[N], T value) {
    // Nearest rather than exact: a stored value can be legal but off-ladder if
    // a ladder is ever retuned, and snapping is friendlier than resetting.
    uint8_t best = 0;
    int32_t bestDiff = -1;
    for (uint8_t i = 0; i < N; ++i) {
        const int32_t d = static_cast<int32_t>(table[i]) - static_cast<int32_t>(value);
        const int32_t mag = d < 0 ? -d : d;
        if (bestDiff < 0 || mag < bestDiff) {
            bestDiff = mag;
            best = i;
        }
    }
    return best;
}

template <typename T, uint8_t N>
T entry(const T (&table)[N], uint8_t index) {
    return table[index < N ? index : N - 1];
}

} // namespace

uint8_t rowCount() { return static_cast<uint8_t>(RowId::Count); }

const char* rowName(RowId id) {
    switch (id) {
        case RowId::Theme:    return "THEME";
        case RowId::Bright:   return "BRIGHT";
        case RowId::DimTo:    return "DIM TO";
        case RowId::DimAt:    return "DIM AT";
        case RowId::BlankAt:  return "BLANK AT";
        case RowId::OffAt:    return "OFF AT";
        case RowId::Alarm:    return "ALARM";
        case RowId::Sound:    return "SOUND";
        case RowId::Battery:  return "BATTERY";
        case RowId::Cal:      return "CAL";
        case RowId::Defaults: return "DEFAULTS";
        case RowId::Count:    break;
    }
    return "?";
}

uint8_t ladderSize(RowId id) {
    switch (id) {
        case RowId::Theme:    return kThemeCount;
        case RowId::Bright:   return sizeof(kBright) / sizeof(kBright[0]);
        case RowId::DimTo:    return sizeof(kDimTo) / sizeof(kDimTo[0]);
        case RowId::DimAt:    return sizeof(kDimAt) / sizeof(kDimAt[0]);
        case RowId::BlankAt:  return sizeof(kBlankAt) / sizeof(kBlankAt[0]);
        case RowId::OffAt:    return sizeof(kOffAt) / sizeof(kOffAt[0]);
        case RowId::Alarm:    return sizeof(kAlarm) / sizeof(kAlarm[0]);
        case RowId::Sound:    return 2;
        case RowId::Battery:  return 0; // read-only
        case RowId::Cal:      return kCalCount;
        case RowId::Defaults: return 2;
        case RowId::Count:    break;
    }
    return 0;
}

uint8_t ladderIndex(RowId id, const Settings& s) {
    switch (id) {
        case RowId::Theme:    return s.themeId;
        case RowId::Bright:   return nearestIndex(kBright, s.backlightActive);
        case RowId::DimTo:    return nearestIndex(kDimTo, s.backlightDim);
        case RowId::DimAt:    return nearestIndex(kDimAt, s.dimAfterS);
        case RowId::BlankAt:  return nearestIndex(kBlankAt, s.blankAfterS);
        case RowId::OffAt:    return nearestIndex(kOffAt, s.offAfterS);
        case RowId::Alarm:    return nearestIndex(kAlarm, s.alarmS);
        case RowId::Sound:    return s.mute;
        case RowId::Cal:
            return s.batCalAuto
                       ? 0
                       : static_cast<uint8_t>((s.batCalPermille - kCalMin) / 2 + 1);
        case RowId::Defaults: return 0; // always rests on KEEP
        case RowId::Battery:
        case RowId::Count:    break;
    }
    return 0;
}

void applyLadder(RowId id, uint8_t index, Settings& s) {
    const uint8_t n = ladderSize(id);
    if (n == 0) return;
    if (index >= n) index = static_cast<uint8_t>(n - 1);

    switch (id) {
        case RowId::Theme:   s.themeId = index; break;
        case RowId::Bright:  s.backlightActive = entry(kBright, index); break;
        case RowId::DimTo:   s.backlightDim = entry(kDimTo, index); break;
        case RowId::DimAt:   s.dimAfterS = entry(kDimAt, index); break;
        case RowId::BlankAt: s.blankAfterS = entry(kBlankAt, index); break;
        case RowId::OffAt:   s.offAfterS = entry(kOffAt, index); break;
        case RowId::Alarm:   s.alarmS = entry(kAlarm, index); break;
        case RowId::Sound:   s.mute = index; break;
        case RowId::Cal:
            if (index == 0) {
                // Re-arm. The gain itself is left alone; the next charge
                // plateau will overwrite it.
                s.batCalAuto = 1;
                break;
            }
            if (s.batCalAuto) {
                // Leaving AUTO. The index the wheel landed on is discarded
                // exactly once, here, so that hand-tuning starts from the gain
                // auto already learned rather than from 850. The wheel reads
                // its position back out of Settings on every drag, so the next
                // step continues from the learned value's index -- see
                // app/settings_ui.cpp's onDrag.
                s.batCalAuto = 0;
                break;
            }
            s.batCalPermille = static_cast<uint16_t>(kCalMin + (index - 1) * 2);
            break;
        case RowId::Defaults:
            // Selecting RESET applies defaults LIVE, so the user watches it
            // happen -- and lifting the device still undoes it, like any other
            // change. No new gesture, no confirmation dialog.
            if (index == 1) {
                // batFloorRawMv is EXEMPT. It is not a preference, it is an
                // observation that cost a full discharge to brownout to
                // acquire, and it is the only thing arming the low-voltage
                // cutoff -- so zeroing it would silently disarm the safety
                // route until another full discharge, which is the one thing
                // in this device a user cannot simply redo.
                //
                // batCalPermille and batCalAuto do reset, deliberately: those
                // are preference-shaped, and the next charge re-learns the
                // gain in a single cycle.
                const uint16_t floorRawMv = s.batFloorRawMv;
                s = kDefaults;
                s.batFloorRawMv = floorRawMv;
            }
            break;
        case RowId::Battery:
        case RowId::Count:   break;
    }
}

const char* ladderLabel(RowId id, uint8_t index) {
    if (id != RowId::Defaults) return nullptr;
    return index == 1 ? "RESET" : "KEEP";
}

bool rowAccelerates(RowId id) { return id == RowId::Cal; }

} // namespace h0
