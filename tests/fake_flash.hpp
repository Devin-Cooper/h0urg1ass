#pragma once

#include <cstring>
#include <map>
#include <vector>

#include "settings/flash_backend.hpp"

/// An in-RAM NOR flash that behaves like the real part.
///
/// Two properties make it worth having over a plain byte array:
///
///  - **Programming only clears bits.** `newByte = old & src` is what NOR does,
///    and it is why re-programming a page without erasing produces garbage
///    rather than the new value. A test that ignores this passes on a lie.
///  - **It can die mid-operation.** `dieAfter(n)` lets the next program or erase
///    write exactly n bytes and then stop, which is a power cut.
class FakeFlash : public h0::IFlashBackend {
public:
    explicit FakeFlash(uint32_t base, size_t bytes)
        : base_(base), mem_(bytes, 0xFF) {}

    void read(uint32_t offset, uint8_t* dst, size_t len) const override {
        std::memcpy(dst, &mem_[offset - base_], len);
    }

    bool programPage(uint32_t offset, const uint8_t* src) override {
        const size_t at = offset - base_;
        ++programs_;
        for (size_t i = 0; i < kPageBytes; ++i) {
            if (dieAfter_ >= 0 && written_ >= dieAfter_) return false;
            mem_[at + i] &= src[i]; // NOR: bits go 1 -> 0 only
            ++written_;
        }
        return true;
    }

    bool eraseSector(uint32_t offset) override {
        const size_t at = offset - base_;
        ++erases_;
        for (size_t i = 0; i < kSectorBytes; ++i) {
            if (dieAfter_ >= 0 && written_ >= dieAfter_) return false;
            mem_[at + i] = 0xFF;
            ++written_;
        }
        return true;
    }

    /// Stop writing after `n` more bytes. -1 disables.
    void dieAfter(long n) { dieAfter_ = n; written_ = 0; }
    void revive() { dieAfter_ = -1; written_ = 0; }
    int eraseCount() const { return erases_; }
    /// Page programs ATTEMPTED. "No write at boot" is a claim about this, and
    /// asserting only on eraseCount would let a stray program through.
    int programCount() const { return programs_; }

private:
    uint32_t base_;
    std::vector<uint8_t> mem_;
    long dieAfter_ = -1;
    long written_ = 0;
    int erases_ = 0;
    int programs_ = 0;
};
