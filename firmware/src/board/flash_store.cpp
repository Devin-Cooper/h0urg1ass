#include "board/flash_store.hpp"

#include <cstring>

#include "hardware/flash.h"
#include "pico/flash.h"

#include "settings/settings_store.hpp"

namespace board {

namespace {

// Getting the region outside the configured flash is a build error rather than
// a runtime panic. See firmware/CMakeLists.txt for why the CMake variable and
// not the compile definition is what makes this true.
static_assert(h0::SettingsStore::kSectorB + h0::IFlashBackend::kSectorBytes <=
                  PICO_FLASH_SIZE_BYTES,
              "settings region falls outside the configured flash size");

struct ProgramArgs {
    uint32_t offset;
    const uint8_t* src;
};

struct EraseArgs {
    uint32_t offset;
};

void doProgram(void* param) {
    auto* a = static_cast<ProgramArgs*>(param);
    flash_range_program(a->offset, a->src, h0::IFlashBackend::kPageBytes);
}

void doErase(void* param) {
    auto* a = static_cast<EraseArgs*>(param);
    flash_range_erase(a->offset, h0::IFlashBackend::kSectorBytes);
}

/// Generous: a 4 kB erase is 45 ms typical and 400 ms worst case, and the
/// watchdog is 8 s. This only has to be longer than the operation and shorter
/// than the watchdog.
constexpr uint32_t kFlashTimeoutMs = 3000;

} // namespace

void FlashStore::read(uint32_t offset, uint8_t* dst, size_t len) const {
    std::memcpy(dst, reinterpret_cast<const uint8_t*>(XIP_BASE + offset), len);
}

bool FlashStore::programPage(uint32_t offset, const uint8_t* src) {
    ProgramArgs args{offset, src};
    return flash_safe_execute(doProgram, &args, kFlashTimeoutMs) == PICO_OK;
}

bool FlashStore::eraseSector(uint32_t offset) {
    EraseArgs args{offset};
    return flash_safe_execute(doErase, &args, kFlashTimeoutMs) == PICO_OK;
}

} // namespace board
