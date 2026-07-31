#pragma once

#include <1bit/hal/display.hpp>

#include <hardware/dma.h>
#include <hardware/spi.h>

#include <cstdint>

#include "board/pins.hpp"

namespace board {

/// ST7789V2 panel on the Waveshare RP2350-Touch-LCD-1.69 (240x280, portrait).
///
/// Everything above the transport -- geometry, the +20 GRAM offset, rotation,
/// 1-bit expansion, strip chunking, dirty rects -- is the graphics library's
/// portable layer. Only this file is board-specific.
///
/// ⚠ **Do not port pin defaults from the RP2350-Touch-LCD-2.8 driver.** That
/// board uses GPIO14 for D/C and GPIO15 for RST. On *this* board GPIO14 is the
/// power button and **GPIO15 is the battery power latch** -- driving it as a
/// reset line drops the latch and switches the board off mid-frame on battery,
/// while behaving perfectly over USB. The pin map lives in pins.hpp and is
/// corroborated by nine vendor code artifacts.
///
/// The panel is **write-only**: MISO is not routed, so there is no controller
/// ID read, no GRAM read-back and no init verification. The only integrity
/// check available is to draw a known pattern and look at it.
class St7789_1in69 : public onebit::WindowedDisplayDriver {
public:
    /// `use16BitFrames` ships pixels as 16-bit SPI frames rather than bytes,
    /// halving the DMA transfer count and removing the manual byte swap the
    /// vendor code does. It requires the expander to emit little-endian RGB565
    /// so the in-memory halfword shifts out big-endian, which is what the panel
    /// wants; the constructor selects the matching PixelFormat.
    explicit St7789_1in69(onebit::Rotation rot = onebit::Rotation::Rot0,
                          uint32_t spiBaud = 62'500'000,
                          int stripRows = 40,
                          bool use16BitFrames = true);
    ~St7789_1in69() override;

    bool init();

    onebit::DisplayCaps caps() const override {
        onebit::DisplayCaps c;
        c.partialUpdate = true;
        c.backlight = true;
        return c;
    }

    void clear(onebit::Color c = onebit::WHITE) override;
    bool setBacklight(uint8_t level) override;

    /// Swap ink and paper on the glass: white figures on a black field.
    ///
    /// A display MODE, not a redraw. INVON/INVOFF costs one byte on the wire and
    /// nothing per frame, so the framebuffer keeps its own convention -- BLACK
    /// still means ink -- and nothing upstream needs to know which way round the
    /// panel is showing it. Doing this by inverting the buffer instead would
    /// cost a pass over 8,400 bytes every frame to achieve the same thing.
    ///
    /// XORed with the panel's own requirement rather than replacing it. This
    /// panel needs INVON to render ink as black at all, so an inverted UI is
    /// INVOFF here -- and collapsing the two would make a taste decision look
    /// like a hardware quirk to the next person reading the init sequence.
    void setInverted(bool on);
    bool inverted() const { return uiInverted_; }

    /// Set the theme's ink and paper. Must be followed by a full-frame push --
    /// the dirty-rect tracker has no idea the colours moved, so nothing repaints
    /// on its own.
    void setColors(uint32_t ink, uint32_t paper) {
        WindowedDisplayDriver::setColors(ink, paper);
    }

    /// Block until DMA has drained *and* the SPI shift register is empty, then
    /// release CS. `writePixels` only queues a transfer, so anything timing a
    /// push -- or about to stop the clock -- must call this first.
    void waitIdle();

    /// What `spi_init` actually negotiated. The PL022 divides clk_peri by an
    /// integer prescale and postdiv, so the requested baud is rarely achieved,
    /// and every bus-time estimate assumes the real number.
    uint32_t actualBaud() const { return actualBaud_; }
    size_t stripBytes() const { return stripBytes_; }

protected:
    void setWindow(const onebit::Window& w) override;
    void writePixels(const uint8_t* data, size_t len) override;
    uint8_t* stripBuffer(size_t& capacityBytes) override;

private:
    void sendCmd(uint8_t cmd, const uint8_t* params = nullptr, size_t len = 0);
    void hardReset();
    void setFrameBits(uint8_t bits);
    void releaseCs();

    spi_inst_t* spi_;
    uint32_t requestedBaud_;
    uint32_t actualBaud_ = 0;
    int stripRows_;
    bool use16_;

    // Ping-pong strips: the expander fills one while DMA drains the other.
    // A single buffer is not merely slower -- writePixels is asynchronous, so
    // reusing it without waiting corrupts the frame in flight, and the
    // corruption does not change when you halve the SPI clock, so it reads as
    // signal integrity and sends you hunting the wrong problem.
    uint8_t* strip_[2] = {nullptr, nullptr};
    int cur_ = 1;
    size_t stripBytes_ = 0;

    int dmaChan_ = -1;
    uint8_t frameBits_ = 8;
    bool pixelMode_ = false; ///< CS held low with D/C high, mid-RAMWR
    int blSlice_ = -1;
    bool uiInverted_ = false;
    bool inited_ = false;
};

} // namespace board
