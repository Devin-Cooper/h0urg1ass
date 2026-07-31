#include "settings/settings_store.hpp"

namespace h0 {

namespace {

constexpr uint32_t slotOffset(uint32_t sector, int slot) {
    return sector + static_cast<uint32_t>(slot) * static_cast<uint32_t>(kRecordBytes);
}

} // namespace

int SettingsStore::findFreeSlot(uint32_t sector) const {
    uint8_t rec[kRecordBytes];
    for (int i = 0; i < kRecordsPerSector; ++i) {
        flash_.read(slotOffset(sector, i), rec, kRecordBytes);
        if (isErased(rec)) return i;
    }
    return -1;
}

bool SettingsStore::sectorHasValidRecord(uint32_t sector) const {
    uint8_t rec[kRecordBytes];
    uint32_t seq = 0;
    Settings s{};
    for (int i = 0; i < kRecordsPerSector; ++i) {
        flash_.read(slotOffset(sector, i), rec, kRecordBytes);
        if (decodeRecord(rec, seq, s)) return true;
    }
    return false;
}

void SettingsStore::load() {
    current_ = kDefaults;
    seq_ = 0;
    erasePending_ = false;

    uint8_t rec[kRecordBytes];
    bool found = false;
    uint32_t bestSector = kSectorA;

    const uint32_t sectors[2] = {kSectorA, kSectorB};
    for (uint32_t sector : sectors) {
        for (int i = 0; i < kRecordsPerSector; ++i) {
            flash_.read(slotOffset(sector, i), rec, kRecordBytes);
            uint32_t seq = 0;
            Settings s{};
            if (!decodeRecord(rec, seq, s)) continue;
            if (!found || seq > seq_) {
                seq_ = seq;
                current_ = s;
                bestSector = sector;
                found = true;
            }
        }
    }

    activeSector_ = bestSector;
    staleSector_ = (bestSector == kSectorA) ? kSectorB : kSectorA;

    // A roll that was interrupted before its erase leaves BOTH sectors holding
    // valid records. Detecting that at boot and re-arming is what stops the
    // stale sector living forever and blocking the next roll.
    if (found && sectorHasValidRecord(staleSector_)) erasePending_ = true;
}

bool SettingsStore::commit(const Settings& s) {
    Settings toWrite = s;
    clamp(toWrite);

    int slot = findFreeSlot(activeSector_);

    if (slot < 0) {
        // The active sector is full. Write the FIRST record of the other sector,
        // and only then arm its erase. That ordering means a power cut here is
        // harmless: the new record is already durable before anything is
        // destroyed.
        if (erasePending_) {
            // Both sectors full and an erase still owed. Forced, stall and all:
            // the alternative is silently failing to save.
            if (!flash_.eraseSector(staleSector_)) return false;
            erasePending_ = false;
        }
        uint32_t target = staleSector_;
        slot = findFreeSlot(target);
        if (slot < 0) {
            if (!flash_.eraseSector(target)) return false;
            slot = 0;
        }

        uint8_t rec[kRecordBytes];
        encodeRecord(toWrite, seq_ + 1, rec);
        if (!flash_.programPage(slotOffset(target, slot), rec)) return false;

        staleSector_ = activeSector_;
        activeSector_ = target;
        ++seq_;
        current_ = toWrite;
        erasePending_ = true;
        return true;
    }

    uint8_t rec[kRecordBytes];
    encodeRecord(toWrite, seq_ + 1, rec);
    if (!flash_.programPage(slotOffset(activeSector_, slot), rec)) return false;

    ++seq_;
    current_ = toWrite;
    return true;
}

bool SettingsStore::runDeferredErase() {
    if (!erasePending_) return true;
    if (!flash_.eraseSector(staleSector_)) return false;
    erasePending_ = false;
    return true;
}

} // namespace h0
