#include "render/raster_ops.hpp"

#include "faces/layout.hpp"

namespace h0 {

namespace {

/// Bit-reversed byte. Built once; 256 bytes of RAM against a per-pixel loop.
uint8_t g_rev8[256];
bool g_rev8Ready = false;

void buildRev8() {
    if (g_rev8Ready) return;
    for (int b = 0; b < 256; ++b) {
        uint8_t v = 0;
        for (int i = 0; i < 8; ++i)
            if ((b >> i) & 1) v |= static_cast<uint8_t>(0x80u >> i);
        g_rev8[b] = v;
    }
    g_rev8Ready = true;
}

} // namespace

void invertSafeBox(onebit::IFramebuffer& fb) {
    // Byte alignment is what keeps this a whole-byte complement rather than a
    // masked read-modify-write on the edges. 16 and 224 are both multiples of 8.
    static_assert(safe::X % 8 == 0, "safe box must start on a byte boundary");
    static_assert((safe::X + safe::W) % 8 == 0, "safe box must end on a byte boundary");

    uint8_t* buf = fb.buffer();
    if (!buf) return;
    const int stride = fb.bytesPerRow();
    for (int16_t y = safe::Y; y < safe::Y + safe::H && y < fb.height(); ++y) {
        uint8_t* row = buf + static_cast<size_t>(y) * stride;
        for (int i = safe::X / 8; i < (safe::X + safe::W) / 8 && i < stride; ++i) {
            row[i] = static_cast<uint8_t>(~row[i]);
        }
    }
}

void rotate180(onebit::IFramebuffer& fb) {
    uint8_t* buf = fb.buffer();
    if (!buf) return;

    // The panel is 240 px wide, which is 30 whole bytes with no row padding. A
    // 180-degree rotation is therefore a plain reversal of the byte array with
    // every byte bit-reversed -- no row or column bookkeeping at all. That only
    // holds while there is no padding, so check rather than assume.
    const int stride = fb.bytesPerRow();
    const size_t n = fb.bufferSize();
    if (fb.width() % 8 != 0) return;
    if (static_cast<size_t>(stride) * static_cast<size_t>(fb.height()) != n) return;

    buildRev8();
    for (size_t i = 0, j = n - 1; i < j; ++i, --j) {
        const uint8_t t = g_rev8[buf[i]];
        buf[i] = g_rev8[buf[j]];
        buf[j] = t;
    }
    if (n & 1u) buf[n / 2] = g_rev8[buf[n / 2]];
}

} // namespace h0
