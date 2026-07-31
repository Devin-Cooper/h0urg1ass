#pragma once

#include <cassert>
#include <cstring>
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
///
/// Bounds and sector-alignment are asserted rather than silently tolerated:
/// this class exists to catch address-arithmetic mistakes in the store, and
/// an out-of-range `offset - base_` wrapping into UB would defeat that.
class FakeFlash : public h0::IFlashBackend {
public:
    explicit FakeFlash(uint32_t base, size_t bytes)
        : base_(base), mem_(bytes, 0xFF) {}

    void read(uint32_t offset, uint8_t* dst, size_t len) const override {
        assert(offset >= base_);
        const size_t at = offset - base_;
        assert(at + len <= mem_.size());
        std::memcpy(dst, &mem_[at], len);
    }

    bool programPage(uint32_t offset, const uint8_t* src) override {
        assert(offset >= base_);
        const size_t at = offset - base_;
        assert(at + kPageBytes <= mem_.size());
        ++programs_;
        for (size_t i = 0; i < kPageBytes; ++i) {
            if (dieAfter_ >= 0 && written_ >= dieAfter_) return false;
            mem_[at + i] &= src[i]; // NOR: bits go 1 -> 0 only
            ++written_;
        }
        return true;
    }

    bool eraseSector(uint32_t offset) override {
        assert(offset >= base_);
        const size_t at = offset - base_;
        assert(at + kSectorBytes <= mem_.size());
        assert(at % kSectorBytes == 0); // real erases are whole, aligned sectors
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

    /// Test-only: force a byte range straight to the erased state (0xFF),
    /// bypassing eraseSector()'s dieAfter/byte-order machinery entirely.
    ///
    /// Real NOR sector erase is a single electrical pulse across the whole
    /// sector, not a byte-serial write -- unlike programPage, which genuinely
    /// is serial on real parts. A power cut mid-pulse does not guarantee any
    /// particular address ordering of which cells finish clearing first, so
    /// eraseSector's sequential dieAfter model cannot honestly produce every
    /// reachable partial-erase state (in particular it can never leave a
    /// record's magic and len bytes untouched while only its seq bytes, which
    /// sit between them, have gained bits). This lets a test construct that
    /// state directly instead of pretending the sequential model covers it.
    void forceErasedRange(uint32_t offset, size_t len) {
        assert(offset >= base_);
        const size_t at = offset - base_;
        assert(at + len <= mem_.size());
        for (size_t i = 0; i < len; ++i) mem_[at + i] = 0xFF;
    }

private:
    uint32_t base_;
    std::vector<uint8_t> mem_;
    long dieAfter_ = -1;
    long written_ = 0;
    int erases_ = 0;
    int programs_ = 0;
};
