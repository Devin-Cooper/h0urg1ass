#include "sand/sand_render.hpp"

namespace h0 {

namespace {

/// byte -> 16 bits, each input bit doubled. Built once at startup.
///
/// 0b1011_0000 becomes 0b11001111_00000000. This is the whole of the 2x scale:
/// one lookup turns eight cells into sixteen pixels, in the framebuffer's own
/// MSB-first order.
uint16_t g_double[256];
bool g_doubleReady = false;

void buildDoubleTable() {
    if (g_doubleReady) return;
    for (int b = 0; b < 256; ++b) {
        uint16_t out = 0;
        for (int i = 0; i < 8; ++i) {
            if ((b >> (7 - i)) & 1) {
                out |= static_cast<uint16_t>(0x3u << (14 - 2 * i));
            }
        }
        g_double[b] = out;
    }
    g_doubleReady = true;
}

} // namespace

SandGrid makeVessel(int holeHalfWidth) {
    SandGrid w;
    // Border. The grid's own out-of-bounds reads already behave as solid, but a
    // real border means the renderer draws a visible vessel rather than relying
    // on an invisible rule.
    for (int x = 0; x < SandGrid::W; ++x) {
        w.set(x, 0, true);
        w.set(x, SandGrid::H - 1, true);
    }
    for (int y = 0; y < SandGrid::H; ++y) {
        w.set(0, y, true);
        w.set(SandGrid::W - 1, y, true);
    }
    // The floor, with its gap.
    for (int x = 0; x < SandGrid::W; ++x) {
        const bool inHole = (x >= sandgeom::HOLE_CX - holeHalfWidth) &&
                            (x <= sandgeom::HOLE_CX + holeHalfWidth);
        if (!inHole) w.set(x, sandgeom::FLOOR_ROW, true);
    }
    return w;
}

void fillLintelSolid(SandGrid& w) {
    for (int y = sandgeom::LINTEL_CY0; y <= sandgeom::LINTEL_CY1; ++y) {
        for (int x = sandgeom::LINTEL_CX0; x <= sandgeom::LINTEL_CX1; ++x) w.set(x, y, true);
    }
}

void drawLintelOutline(SandGrid& w) {
    // Jambs and soffit. No head rail: row LINTEL_CY0 - 1 is the vessel's own top
    // border, already drawn, and the lintel hangs from it.
    for (int y = sandgeom::LINTEL_CY0; y <= sandgeom::LINTEL_CY1; ++y) {
        w.set(sandgeom::LINTEL_CX0, y, true);
        w.set(sandgeom::LINTEL_CX1, y, true);
    }
    for (int x = sandgeom::LINTEL_CX0; x <= sandgeom::LINTEL_CX1; ++x) {
        w.set(x, sandgeom::LINTEL_CY1, true);
    }
}

void renderSand(onebit::IFramebuffer& fb, const SandGrid& sand, const SandGrid& walls) {
    buildDoubleTable();

    uint8_t* buf = fb.buffer();
    if (!buf) return;
    const int stride = fb.bytesPerRow();
    constexpr int kByteOffset = sandgeom::ORIGIN_X / 8; // 2

    // 104 cells is 13 bytes, and the last one is whole -- 104 is a multiple of
    // 8 even though it is not a multiple of 32, so no partial byte to mask.
    constexpr int kSrcBytes = SandGrid::W / 8;
    static_assert(SandGrid::W % 8 == 0, "row expansion assumes whole source bytes");

    for (int cy = 0; cy < SandGrid::H; ++cy) {
        const int16_t py = static_cast<int16_t>(sandgeom::ORIGIN_Y + cy * sandgeom::SCALE);
        if (py < 0 || py + 1 >= fb.height()) continue;

        uint8_t* r0 = buf + static_cast<size_t>(py) * stride + kByteOffset;
        uint8_t* r1 = r0 + stride;

        for (int b = 0; b < kSrcBytes; ++b) {
            // Sand and vessel are drawn as one ink layer: the wall is as solid
            // as the sand, and separating them would need a second pass for no
            // visual difference at one bit.
            const int wordIdx = b >> 2;
            const int shift = 24 - 8 * (b & 3);
            const uint8_t src =
                static_cast<uint8_t>(((sand.word(cy, wordIdx) | walls.word(cy, wordIdx)) >>
                                      shift) & 0xFFu);
            const uint16_t wide = g_double[src];
            const uint8_t hi = static_cast<uint8_t>(wide >> 8);
            const uint8_t lo = static_cast<uint8_t>(wide & 0xFF);
            r0[b * 2] = hi;
            r0[b * 2 + 1] = lo;
            r1[b * 2] = hi;
            r1[b * 2 + 1] = lo;
        }
    }
}

} // namespace h0
