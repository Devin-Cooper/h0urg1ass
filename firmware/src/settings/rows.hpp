#pragma once

#include <cstdint>

#include "settings/settings.hpp"

namespace h0 {

/// Rows in wheel order. The row wheel WRAPS over these; every value wheel
/// CLAMPS -- see app/settings_ui.cpp (SettingsUi::onDrag, around the row-wheel
/// and value-wheel update blocks) for why that inverts the picker's rule.
enum class RowId : uint8_t {
    Theme, Bright, DimTo, DimAt, BlankAt, OffAt, Alarm, Sound, Battery, Cal, Defaults, Count
};

uint8_t rowCount();
const char* rowName(RowId id);

/// Number of entries in the row's value ladder. 0 means read-only.
uint8_t ladderSize(RowId id);

/// The index currently selected, given `s`. Undefined for read-only rows.
uint8_t ladderIndex(RowId id, const Settings& s);

/// Write ladder entry `index` into `s`. Out-of-range indices are clamped.
void applyLadder(RowId id, uint8_t index, Settings& s);

/// Label for a ladder entry that cannot be derived from Settings, or nullptr.
///
/// Only DEFAULTS needs this. Selecting RESET rewrites the whole struct and
/// leaves no trace of itself, so a formatter given only a Settings would draw
/// KEEP for both entries and the user would be asked to drag onto a word that
/// is never on screen.
const char* ladderLabel(RowId id, uint8_t index);

/// True when this row's value wheel should use DragColumn's velocity gain.
bool rowAccelerates(RowId id);

} // namespace h0
