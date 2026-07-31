#include "settings/theme.hpp"

namespace h0 {

namespace {

using onebit::rgb565;

// Computed, never transcribed. rgb565() is constexpr (pixel_format.hpp), and
// writing 0xFD20 for a colour that is actually 0xFD80 is a mistake that reads
// perfectly in review.
constexpr Theme kThemes[] = {
    {rgb565(255, 255, 255), rgb565(0, 0, 0),       "WHITE"}, // the shipped default
    {rgb565(0, 0, 0),       rgb565(255, 255, 255), "PAPER"}, // sun, at a low backlight
    {rgb565(255, 176, 0),   rgb565(0, 0, 0),       "AMBER"}, // phosphor
    {rgb565(255, 32, 0),    rgb565(0, 0, 0),       "NIGHT"}, // preserves dark adaptation
};

static_assert(sizeof(kThemes) / sizeof(kThemes[0]) ==
                  static_cast<size_t>(ThemeId::Count),
              "theme table and ThemeId disagree");

} // namespace

const Theme& themeFor(ThemeId id) {
    const uint8_t i = static_cast<uint8_t>(id);
    return kThemes[i < static_cast<uint8_t>(ThemeId::Count) ? i : 0];
}

} // namespace h0
