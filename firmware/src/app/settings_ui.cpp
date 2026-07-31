#include "app/settings_ui.hpp"

namespace h0 {

namespace {

int16_t clampIndex(int32_t v, uint8_t count) {
    if (v < 0) return 0;
    if (v >= count) return static_cast<int16_t>(count - 1);
    return static_cast<int16_t>(v);
}

} // namespace

void SettingsUi::open(const Settings& s) {
    if (open_) return;
    snapshot_ = s;
    live_ = s;
    rowIndex_ = 0;
    activeColumn_ = 0;
    rowCol_.reset();
    valueCol_.reset();
    // The row wheel never accelerates: ten entries under a 5x flick is not a
    // list, it is a lottery.
    rowCol_.setGainMax(DragColumn::kGainBase);
    syncValueGain();
    open_ = true;
}

Settings SettingsUi::commit() {
    open_ = false;
    rowCol_.reset();
    valueCol_.reset();
    activeColumn_ = 0;
    return live_;
}

Settings SettingsUi::cancel() {
    open_ = false;
    rowCol_.reset();
    valueCol_.reset();
    activeColumn_ = 0;
    live_ = snapshot_;
    return snapshot_;
}

void SettingsUi::syncValueGain() {
    valueCol_.setGainMax(rowAccelerates(currentRow()) ? DragColumn::kGainMax
                                                      : DragColumn::kGainBase);
}

bool SettingsUi::onDrag(uint8_t column, bool pressed, int16_t y) {
    if (!open_) return false;

    if (!pressed) {
        rowCol_.reset();
        valueCol_.reset();
        activeColumn_ = 0;
        return false;
    }

    // Lock the column on touch-down. A finger drifts sideways during a vertical
    // drag, and switching wheels mid-gesture would move whichever one it
    // wandered over -- the same reason the picker locks.
    if (activeColumn_ == 0) activeColumn_ = column;

    const int dRow = rowCol_.update(activeColumn_ == 1, y);
    const int dVal = valueCol_.update(activeColumn_ == 2, y);

    bool changed = false;

    if (dRow != 0) {
        // The ROW wheel wraps. Ten rows is more than the five visible, so an end
        // is never on screen and wrapping is what the user expects.
        const int n = rowCount();
        rowIndex_ = static_cast<uint8_t>((((rowIndex_ + dRow) % n) + n) % n);
        valueCol_.reset();
        syncValueGain();
    }

    if (dVal != 0) {
        const RowId id = currentRow();
        const uint8_t n = ladderSize(id);
        if (n > 0) {
            // VALUE wheels clamp. A setting has real ends, and hiding them is
            // dishonest -- and a wrapping two-entry wheel would show the same
            // value repeated across all five visible rows.
            const int16_t next =
                clampIndex(static_cast<int32_t>(ladderIndex(id, live_)) + dVal, n);
            applyLadder(id, static_cast<uint8_t>(next), live_);
            clamp(live_);
            changed = true;
        }
    }

    return changed;
}

bool GestureGate::onTouch(bool pressed, bool swipeEdge, bool dragged, uint64_t now) {
    if (pressed && dragged) dragThisTouch_ = true;

    bool fire = false;
    if (swipeEdge && !dragThisTouch_ && !firedThisTouch_ &&
        (lastFireUs_ == 0 || now - lastFireUs_ >= kRefractoryUs)) {
        fire = true;
        firedThisTouch_ = true;
        lastFireUs_ = now;
    }

    // The touch ends only on a GENUINE release, and this is evaluated AFTER the
    // swipe above -- so a controller that reports its gesture on the release
    // read is still honoured. Task 0 settles which it does; the gate works
    // either way, which is the point of not guessing.
    if (!pressed && wasPressed_) {
        dragThisTouch_ = false;
        firedThisTouch_ = false;
    }
    wasPressed_ = pressed;
    return fire;
}

} // namespace h0
