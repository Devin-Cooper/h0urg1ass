#pragma once

#include <1bit/hal/st7789_display.hpp>

#include <cstdint>

#include "hardware/spi.h"

namespace board {

/// The 1.69" 240x280 panel on the Waveshare RP2350-Touch-LCD-1.69.
///
/// Everything about the ST7789 command set now lives in onebit::St7789Display;
/// what remains here is the bus. Three copies of that command set used to
/// exist -- this one, and one per SDK in the library's platform examples.
///
/// The panel is **write-only**: MISO is not routed, so there is no controller
/// ID read, no GRAM read-back and no init verification. The only integrity
/// check available is to draw a known pattern and look at it.
///
/// ⚠ **Do not port pin defaults from the RP2350-Touch-LCD-2.8 driver.** That
/// board uses GPIO14 for D/C and GPIO15 for RST. On *this* board GPIO14 is the
/// power button and **GPIO15 is the battery power latch** -- driving it as a
/// reset line drops the latch and switches the board off mid-frame on battery,
/// while behaving perfectly over USB. The pin map lives in pins.hpp and is
/// corroborated by nine vendor code artifacts.
class St7789_1in69 : public onebit::St7789Display {
public:
    /// `use16BitFrames` ships pixels as 16-bit SPI frames rather than bytes,
    /// halving the DMA transfer count and removing the manual byte swap the
    /// vendor code does. onebit::St7789Display fixes the expander's wire
    /// format to big-endian RGB565, so this driver compensates in the DMA
    /// itself (channel_config_set_bswap) rather than asking the expander for
    /// little-endian bytes -- see sendPixels() in the .cpp.
    explicit St7789_1in69(onebit::Rotation rot = onebit::Rotation::Rot0,
                          uint32_t spiBaud = 62'500'000,
                          int stripRows = 40,
                          bool use16BitFrames = true);
    ~St7789_1in69() override;

    /// Board-level bring-up: pins, SPI, DMA, then the panel's own init.
    bool begin();

    /// onebit::St7789Display re-declares clear() as protected (it overrides
    /// DisplayDriver::clear(), which is public). This is not an override --
    /// no new body, just the access level main.cpp's `lcd.clear(WHITE)`
    /// already relies on restored.
    using onebit::St7789Display::clear;

    bool setBacklight(uint8_t level) override;
    void waitIdle();
    uint32_t actualBaud() const { return actualBaud_; }

protected:
    void sendCommand(uint8_t cmd, const uint8_t* params, size_t len) override;
    void beginPixelStream(uint8_t writeCmd) override;
    void sendPixels(const uint8_t* data, size_t len) override;
    void endPixelStream() override;
    void hardReset() override;
    void delayMs(uint32_t ms) override;
    uint8_t* stripBuffer(size_t& capacityBytes) override;

private:
    void setFrameBits(uint8_t bits);

    spi_inst_t* spi_ = nullptr;
    uint32_t requestedBaud_;
    uint32_t actualBaud_ = 0;
    int stripRows_;
    bool use16_;
    uint8_t frameBits_ = 0;
    bool pixelMode_ = false;
    int dmaChan_ = -1;
    uint8_t* strips_[2] = {nullptr, nullptr};
    int nextStrip_ = 0;
    size_t stripBytes_ = 0;
    int blSlice_ = -1;
};

} // namespace board
