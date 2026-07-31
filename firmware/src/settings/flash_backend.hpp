#pragma once

#include <cstddef>
#include <cstdint>

namespace h0 {

/// The seam that keeps the store host-testable.
///
/// The real implementation is board/flash_store.cpp and touches pico-sdk. The
/// test implementation enforces genuine NOR semantics -- program may only clear
/// bits 1->0, erase sets 0xFF -- and can be told to die mid-operation, which is
/// what turns "power cut during a roll" from a paragraph into a test case.
class IFlashBackend {
public:
    static constexpr size_t kSectorBytes = 4096;
    static constexpr size_t kPageBytes = 256;

    virtual ~IFlashBackend() = default;

    virtual void read(uint32_t offset, uint8_t* dst, size_t len) const = 0;
    virtual bool programPage(uint32_t offset, const uint8_t* src) = 0;
    virtual bool eraseSector(uint32_t offset) = 0;
};

} // namespace h0
