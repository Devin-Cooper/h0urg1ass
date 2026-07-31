#pragma once

#include <1bit/hal/pixel_format.hpp>

#include <cstdint>

#include "settings/settings.hpp"

namespace h0 {

/// A theme is exactly two RGB565 values, and every candidate costs identically.
/// So each entry has to earn its slot on a reason rather than a look.
struct Theme {
    uint16_t ink;
    uint16_t paper;
    const char* name;
};

const Theme& themeFor(ThemeId id);

} // namespace h0
