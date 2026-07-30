#pragma once

#include <cstdint>
#include <cstring>

namespace h0 {

/// Bit-packed occupancy for the sand chamber.
///
/// One bit per cell. At a 2 px grain the whole vessel is 104x124 cells, which
/// is 1,984 bytes -- small enough that three of them plus a wall mask is under
/// 8 kB, so the memory discussion that usually dominates falling-sand designs
/// simply does not arise here.
///
/// Row-major with x packed into words, deliberately: it is the layout that lets
/// a fall step be computed a whole word at a time (`canFall = row & ~rowBelow`)
/// rather than cell by cell. Bit-packing addressed one cell at a time is
/// actually *slower* than a byte per cell -- measured, and the RP2350 has no
/// data cache to make up the difference -- so the packing only earns its place
/// combined with word-wide operations.
class SandGrid {
public:
    static constexpr int W = 104;
    static constexpr int H = 124;
    static constexpr int WORDS = (W + 31) / 32; // 4

    void clear() { std::memset(bits_, 0, sizeof(bits_)); }

    bool get(int x, int y) const {
        if (x < 0 || x >= W || y < 0 || y >= H) return true; // out of bounds is solid
        return (bits_[y][x >> 5] >> (31 - (x & 31))) & 1u;
    }

    void set(int x, int y, bool v) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        const uint32_t m = 1u << (31 - (x & 31));
        if (v) bits_[y][x >> 5] |= m;
        else bits_[y][x >> 5] &= ~m;
    }

    /// Cells set. Used by the tests to assert conservation, and by the valve to
    /// know how much is left upstairs.
    int count() const {
        int n = 0;
        for (int y = 0; y < H; ++y)
            for (int w = 0; w < WORDS; ++w) n += popcount(bits_[y][w]);
        return n;
    }

    int countRows(int y0, int y1) const {
        int n = 0;
        for (int y = y0; y <= y1 && y < H; ++y)
            for (int w = 0; w < WORDS; ++w) n += popcount(bits_[y][w]);
        return n;
    }

    uint32_t word(int y, int w) const { return bits_[y][w]; }

    /// MSB-first within a word, matching the framebuffer's own packing so a row
    /// can eventually be blitted rather than converted pixel by pixel.
    static int popcount(uint32_t v) {
        v = v - ((v >> 1) & 0x55555555u);
        v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
        v = (v + (v >> 4)) & 0x0F0F0F0Fu;
        return static_cast<int>((v * 0x01010101u) >> 24);
    }

private:
    uint32_t bits_[H][WORDS] = {};
};

} // namespace h0
