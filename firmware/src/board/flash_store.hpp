#pragma once

#include "settings/flash_backend.hpp"

namespace board {

/// The real flash, behind the store's interface.
///
/// Reads go straight through XIP as a pointer; writes go through
/// flash_safe_execute. Core 1 is never launched on this board, so masking
/// interrupts on core 0 is the entire requirement -- flash_safe_execute is used
/// as a choice rather than a necessity, because it costs nothing and survives
/// the day core 1 is launched. Its return value is checked for the same reason.
class FlashStore : public h0::IFlashBackend {
public:
    void read(uint32_t offset, uint8_t* dst, size_t len) const override;
    bool programPage(uint32_t offset, const uint8_t* src) override;
    bool eraseSector(uint32_t offset) override;
};

} // namespace board
