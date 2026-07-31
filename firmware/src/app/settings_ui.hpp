#pragma once

#include <cstdint>

#include "input/drag_column.hpp"
#include "settings/rows.hpp"
#include "settings/settings.hpp"

namespace h0 {

/// The settings screen's interaction, with no rendering and no hardware.
///
/// Live preview with a snapshot: every edit shows on the glass immediately --
/// the only honest way to choose a brightness -- while `cancel()` puts all of
/// them back. The accidental exit is therefore the safe one, and the deliberate
/// gesture is the only thing that writes.
class SettingsUi {
public:
    /// Enter, snapshotting `s`. Idempotent: opening while open does nothing,
    /// so a repeated gesture cannot silently discard an in-progress snapshot.
    void open(const Settings& s);

    bool isOpen() const { return open_; }

    /// Close, keeping the edits. Returns what to persist.
    Settings commit();

    /// Close, discarding the edits. Returns the snapshot to re-apply.
    Settings cancel();

    /// Feed a touch sample. `column` is 1 for the row wheel (left half) and 2
    /// for the value wheel (right half); 0 means no contact. Returns true when
    /// `live()` changed, so the caller knows to re-apply the theme or backlight.
    bool onDrag(uint8_t column, bool pressed, int16_t y);

    const Settings& live() const { return live_; }
    RowId currentRow() const { return static_cast<RowId>(rowIndex_); }
    uint8_t rowIndex() const { return rowIndex_; }
    uint8_t activeColumn() const { return activeColumn_; }

    /// Sub-unit drag offsets, for rendering the wheels mid-drag.
    int16_t rowOffsetPx() const { return rowCol_.offsetPx(); }
    int16_t valueOffsetPx() const { return valueCol_.offsetPx(); }

private:
    void syncValueGain();

    Settings live_ = kDefaults;
    Settings snapshot_ = kDefaults;
    DragColumn rowCol_, valueCol_;
    uint8_t rowIndex_ = 0;
    uint8_t activeColumn_ = 0;
    bool open_ = false;
};

/// The guard on the entry/exit swipe.
///
/// Pure and separate from main.cpp on purpose: these three rules are where the
/// design said the most likely bug lives, and a rule living in a 700-line main
/// loop is a rule nothing tests.
///
///   1. A swipe is ignored if any wheel has moved during this touch, so a
///      sloppy diagonal drag cannot open settings mid-set.
///   2. At most one gesture per press-to-release cycle, so a latching gesture
///      register cannot open settings and immediately commit them.
///   3. A refractory, so one physical action cannot produce two commands --
///      docs/research/imu-interaction.raw.md prescribes ~700 ms.
class GestureGate {
public:
    static constexpr uint64_t kRefractoryUs = 700'000ull;

    /// Feed one touch sample. `swipeEdge` is a NEW SlideLeft/SlideRight (see
    /// TouchPoint::gestureIsNew); `dragged` is true once a wheel has moved.
    /// Returns true when the swipe should be acted on.
    bool onTouch(bool pressed, bool swipeEdge, bool dragged, uint64_t now);

private:
    bool dragThisTouch_ = false;
    bool firedThisTouch_ = false;
    bool wasPressed_ = false;
    uint64_t lastFireUs_ = 0;
};

} // namespace h0
