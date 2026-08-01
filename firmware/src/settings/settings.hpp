#pragma once

#include <cstddef>
#include <cstdint>

namespace h0 {

/// A theme is exactly two RGB565 values -- see the design spec, section 11.
/// Polarity is not a separate control: White and Paper ARE the old toggle.
enum class ThemeId : uint8_t { White = 0, Paper = 1, Amber = 2, Night = 3, Count = 4 };

/// Everything that survives a power cut.
///
/// Serialised field by field in settings_codec.cpp, never memcpy'd: struct
/// padding and member reordering survive code review and corrupt a user's
/// settings on a compiler upgrade.
///
/// Raw physical values, not ladder indices. An index silently remaps if a
/// ladder is ever retuned; a value does not.
struct Settings {
    static constexpr uint16_t kVersion = 1;

    uint16_t version = kVersion;
    uint8_t  themeId = 0;
    uint8_t  backlightActive = 64;
    uint8_t  backlightDim = 36;
    uint16_t dimAfterS = 20;    ///< 0 = never
    uint16_t blankAfterS = 60;  ///< 0 = never; measured from the dim step
    uint16_t alarmS = 60;
    uint8_t  mute = 0;
    uint16_t batCalPermille = 1000; ///< 1000 = never calibrated
    uint16_t offAfterS = 300;       ///< 0 = never; idle auto-off, section 9's 5 minutes
    uint8_t  batCalAuto = 1;        ///< 1 = automatic calibration armed; cleared by hand-setting CAL
    uint16_t batFloorRawMv = 0;     ///< learned floor, RAW mV; 0 = never learned
};

inline constexpr Settings kDefaults{};

/// The lowest backlight level that is still legible.
///
/// This is the clamp that matters. No reachable setting may make the screen
/// unreadable, because there is no undo anywhere in this device's vocabulary
/// and a dark screen removes the only way back.
inline constexpr uint8_t kBacklightFloor = 16;

inline constexpr uint16_t kCalMin = 850;
inline constexpr uint16_t kCalMax = 1150;

/// Force every field into its legal range.
///
/// Called on every load REGARDLESS of version, so a CRC-valid record holding
/// nonsense cannot produce a broken device. The version field is the weaker
/// guard; this is the real safety net.
void clamp(Settings& s);

bool operator==(const Settings& a, const Settings& b);
inline bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }

} // namespace h0
