#pragma once

#include "settings/flash_backend.hpp"
#include "settings/settings_codec.hpp"

namespace h0 {

/// Append-only settings storage across two alternating 4 kB sectors.
///
/// Wear is a non-issue: one page program per settings session, 16 per sector,
/// two sectors -- 3.2 million writes against the W25Q's 100,000 P/E cycles.
/// The hazard the design actually guards against is the 400 ms worst-case
/// sector erase, which is why erases are deferred and gated rather than
/// performed in response to a user action.
class SettingsStore {
public:
    /// Offsets from XIP_BASE, which is what flash_range_erase/program take --
    /// NOT absolute addresses. The top two sectors of the 16 MB part.
    static constexpr uint32_t kSectorA = 0xFFE000;
    static constexpr uint32_t kSectorB = 0xFFF000;
    static constexpr int kRecordsPerSector =
        static_cast<int>(IFlashBackend::kSectorBytes / kRecordBytes); // 16

    // IFlashBackend::programPage() takes no length parameter -- it
    // unconditionally writes kPageBytes from src, and every call site passes
    // a kRecordBytes buffer. The two constants live in separate headers and
    // are equal today only by coincidence; if that ever stops being true,
    // this becomes a silent out-of-bounds stack read that programs garbage
    // into flash, with no compiler diagnostic.
    static_assert(kRecordBytes == IFlashBackend::kPageBytes,
                  "a record must be exactly one flash page");
    static_assert(kSectorB - kSectorA == IFlashBackend::kSectorBytes,
                  "the two sectors must be exactly one kSectorBytes apart");
    static_assert(kSectorA % IFlashBackend::kSectorBytes == 0,
                  "kSectorA must be sector-aligned");
    static_assert(IFlashBackend::kSectorBytes % kRecordBytes == 0,
                  "kRecordsPerSector's division must be exact");

    explicit SettingsStore(IFlashBackend& backend) : flash_(backend) {}

    /// Scan both sectors. Always leaves settings() valid -- defaults if nothing
    /// decodes. Re-arms a deferred erase if a roll was interrupted.
    void load();

    const Settings& settings() const { return current_; }

    /// Append a record. Rolls to the other sector when the current one is full.
    /// Returns false if the underlying write failed.
    bool commit(const Settings& s);

    bool needsErase() const { return erasePending_; }

    /// Perform the deferred erase. The CALLER gates this on idleness: not flat,
    /// no timer running, no alarm, screen blanked. 400 ms of masked interrupts
    /// can drop the ~1 ms touch pulse outright.
    bool runDeferredErase();

private:
    int findFreeSlot(uint32_t sector) const;
    bool sectorHasValidRecord(uint32_t sector) const;

    IFlashBackend& flash_;
    Settings current_ = kDefaults;
    uint32_t seq_ = 0;
    uint32_t activeSector_ = kSectorA;
    uint32_t staleSector_ = kSectorB;
    bool erasePending_ = false;
};

} // namespace h0
