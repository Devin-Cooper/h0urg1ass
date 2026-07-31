#pragma once

#include <1bit/core/framebuffer.hpp>

#include "app/settings_ui.hpp"
#include "power/battery_model.hpp"

namespace h0 {

/// The settings list: a row wheel on the left, a value wheel on the right.
///
/// Geometry is inherited from the picker rather than invented -- the numbers
/// are already proven on hardware and already in the user's fingers. Three
/// exceptions, each forced: the picker's centre dots at x=120 collide with the
/// value column and are dropped; COL_HALF widens for the value column's
/// active-column bar; and centredNumber() is numeric-only, so this face draws
/// its own string rows.
class SettingsFace {
public:
    static void renderAt(onebit::IFramebuffer& fb, const SettingsUi& ui,
                         const BatteryReading& bat);

    /// The value string for one ladder entry, exposed so the formatting rules
    /// can be tested without rendering. `out` must hold at least 12 bytes.
    ///
    /// `index` is the wheel position being drawn, needed only by DEFAULTS --
    /// RESET rewrites the whole struct and leaves no trace of itself, so it
    /// cannot be recovered from `s`.
    static void formatValue(RowId id, uint8_t index, const Settings& s,
                            const BatteryReading& bat, char* out, size_t n);
};

} // namespace h0
