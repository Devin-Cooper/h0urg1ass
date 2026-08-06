#pragma once

#include <1bit/hal/dcs_panel_display.hpp>

#include <cstdint>

#include "hardware/spi.h"

namespace board {

/// The 1.69" 240x280 panel on the Waveshare RP2350-Touch-LCD-1.69.
///
/// Everything about the ST7789 command set now lives in onebit::DcsPanelDisplay;
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
class St7789_1in69 : public onebit::DcsPanelDisplay {
public:
    /// `use16BitFrames` ships pixels as 16-bit SPI frames rather than bytes,
    /// halving the DMA transfer count and removing the manual byte swap the
    /// vendor code does. onebit::DcsPanelDisplay fixes the expander's wire
    /// format to big-endian RGB565, so this driver compensates in the DMA
    /// itself (channel_config_set_bswap) rather than asking the expander for
    /// little-endian bytes -- see sendPixels() in the .cpp.
    ///
    /// `rot` is honoured: onebit::DcsPanelDisplay::init() emits MADCTL from
    /// geometry().madctlFor(rotation()) after walking the board's table, so
    /// the panel's scan direction and the window addressing setWindow()
    /// computes cannot disagree. Only Rot0 has been seen on this glass --
    /// the vendor's own demos disagree on the landscape MADCTL value (0x78
    /// vs 0xA0), see PanelGeometry::st7789_240x280_1in69() -- so anything
    /// else is untested rather than unsupported.
    explicit St7789_1in69(onebit::Rotation rot = onebit::Rotation::Rot0,
                          uint32_t spiBaud = 62'500'000,
                          int stripRows = 40,
                          bool use16BitFrames = true);
    ~St7789_1in69() override;

    /// Board-level bring-up: pins, SPI, DMA, then the panel's own init.
    bool begin();

    /// Deleted, not merely unused. onebit::DcsPanelDisplay::init() is public
    /// and does panel bring-up only -- reset and the command table. Reaching
    /// it directly on this board means sendCommand() writing into an
    /// uninitialised spi1 with no GPIO directions set and no DMA channel
    /// claimed, which is not a compile error and not an obvious runtime one
    /// either. **Call begin()**, which sets all of that up and then calls
    /// DcsPanelDisplay::init() itself.
    bool init() = delete;

    onebit::DisplayCaps caps() const override {
        onebit::DisplayCaps c;
        c.partialUpdate = true;
        c.backlight = true;
        return c;
    }

    /// Neither of onebit::DcsPanelDisplay's versions is virtual, so these hide
    /// (not override) the inherited ones -- deliberately, because this panel
    /// needs INVON just to render ink as black at all. The base maps
    /// true->INVON/false->INVOFF verbatim; this panel's "normal" UI state
    /// (false) is INVON, so sending the base's mapping unmodified would
    /// complement every RGB565 value reaching the glass the moment a themed
    /// colour existed -- see main.cpp's colour-vs-inversion comment. This
    /// restores the pre-migration XOR against geometry().invert, and the
    /// pre-migration guard that defers the command until begin() has run
    /// (the base sends immediately, which would poke SPI before spi_init()).
    void setInverted(bool on);
    bool inverted() const { return uiInverted_; }

    /// SLPIN, then the settle delay the base class does not have.
    ///
    /// Hides (does not override) onebit::DcsPanelDisplay::sleepIn(), which
    /// sends the command and returns. The only caller is
    /// PowerButton::shutdown(), and what follows it there is three short I2C
    /// transactions, watchdog_disable() and then the SYS_EN latch drop --
    /// well under a millisecond, after which the rail collapses in ~50 us on
    /// battery. The controller needs a moment with power still applied, and
    /// there is no second chance on that path. Guarded on inited_ for the
    /// same reason setInverted() is: before begin(), spi1 is not up.
    void sleepIn();

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
    bool uiInverted_ = false;
    bool inited_ = false;
};

} // namespace board
