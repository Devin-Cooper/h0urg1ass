#include "board/st7789_1in69.hpp"

#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <pico/stdlib.h>

#include <cstdlib>

namespace board {

namespace {

/// A 16-bit SPI frame shifts the in-memory halfword out MSB-first, so the
/// expander must lay RGB565 down little-endian for the wire order to come out
/// big-endian as the panel expects. Getting this backwards swaps every pixel's
/// bytes -- which on a 1-bit image means black and white come out as two
/// unrelated colours rather than looking obviously byte-swapped.
onebit::PixelFormat pickFormat(bool use16) {
    return use16
        ? onebit::PixelFormat::rgb565(0x0000, 0xFFFF, onebit::ByteOrder::LittleEndian)
        : onebit::PixelFormat::rgb565(0x0000, 0xFFFF, onebit::ByteOrder::BigEndian);
}

} // namespace

St7789_1in69::St7789_1in69(onebit::Rotation rot, uint32_t spiBaud, int stripRows,
                           bool use16BitFrames)
    : WindowedDisplayDriver(onebit::PanelGeometry::st7789_240x280_1in69(),
                            pickFormat(use16BitFrames), rot)
    , spi_(spi1)
    , requestedBaud_(spiBaud)
    , stripRows_(stripRows)
    , use16_(use16BitFrames) {}

St7789_1in69::~St7789_1in69() {
    if (dmaChan_ >= 0) dma_channel_unclaim(dmaChan_);
    for (uint8_t*& b : strip_) {
        std::free(b);
        b = nullptr;
    }
}

void St7789_1in69::setFrameBits(uint8_t bits) {
    if (frameBits_ == bits) return;
    // Mode 0. The vendor C BSP and the vendor MicroPython driver disagree on
    // this board (mode 3 vs mode 0); mode 0 is what the sibling 2.8 board was
    // hardware-validated with on the same controller family.
    spi_set_format(spi_, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    frameBits_ = bits;
}

void St7789_1in69::releaseCs() {
    if (pixelMode_) {
        gpio_put(lcd::LCD_CS, 1);
        pixelMode_ = false;
    }
}

bool St7789_1in69::init() {
    stripBytes_ = static_cast<size_t>(width()) * 2u * static_cast<size_t>(stripRows_);
    for (uint8_t*& b : strip_) {
        // malloc is at least 8-byte aligned, which covers the 2-byte alignment
        // a 16-bit DMA read needs.
        b = static_cast<uint8_t*>(std::malloc(stripBytes_));
        if (!b) return false;
    }

    actualBaud_ = spi_init(spi_, requestedBaud_);
    setFrameBits(8);

    gpio_set_function(lcd::LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(lcd::LCD_MOSI, GPIO_FUNC_SPI);
    // No MISO: GPIO12 appears in the vendor headers as LCD_MISO but is not
    // routed anywhere on this board. Deliberately not configured.

    // CS is a plain GPIO, NOT the PL022 hardware CSn -- it is held low across a
    // whole window setup plus pixel stream, which the hardware CS cannot do.
    gpio_init(lcd::LCD_CS);
    gpio_set_dir(lcd::LCD_CS, GPIO_OUT);
    gpio_put(lcd::LCD_CS, 1);
    gpio_init(lcd::LCD_DC);
    gpio_set_dir(lcd::LCD_DC, GPIO_OUT);
    gpio_put(lcd::LCD_DC, 1);
    gpio_init(lcd::LCD_RST);
    gpio_set_dir(lcd::LCD_RST, GPIO_OUT);
    gpio_put(lcd::LCD_RST, 1);

    dmaChan_ = dma_claim_unused_channel(true);

    hardReset();

    // Register sequence taken verbatim from the vendor driver for THIS panel
    // (LCD_1in69.c). Gamma and power curves are panel-specific -- the sibling
    // 2.8 board's sequence is a different panel and must not be substituted.
    const uint8_t madctl = geometry().madctlFor(rotation());
    sendCmd(0x36, &madctl, 1);

    const uint8_t colmod = 0x05; // 16 bpp on the MCU interface
    sendCmd(0x3A, &colmod, 1);

    const uint8_t porch[5] = {0x0B, 0x0B, 0x00, 0x33, 0x35};
    sendCmd(0xB2, porch, 5);
    const uint8_t gctrl = 0x11;
    sendCmd(0xB7, &gctrl, 1);
    const uint8_t vcoms = 0x35;
    sendCmd(0xBB, &vcoms, 1);
    const uint8_t lcmctrl = 0x2C;
    sendCmd(0xC0, &lcmctrl, 1);
    const uint8_t vdvvrhen = 0x01;
    sendCmd(0xC2, &vdvvrhen, 1);
    const uint8_t vrhs = 0x0D;
    sendCmd(0xC3, &vrhs, 1);
    const uint8_t vdvs = 0x20;
    sendCmd(0xC4, &vdvs, 1);
    const uint8_t frctrl2 = 0x13;
    sendCmd(0xC6, &frctrl2, 1);
    const uint8_t pwctrl1[2] = {0xA4, 0xA1};
    sendCmd(0xD0, pwctrl1, 2);
    const uint8_t d6 = 0xA1; // undocumented, present in every vendor sequence
    sendCmd(0xD6, &d6, 1);

    const uint8_t pvgamctrl[14] = {0xF0, 0x06, 0x0B, 0x0A, 0x09, 0x26, 0x29,
                                   0x33, 0x41, 0x18, 0x16, 0x15, 0x29, 0x2D};
    sendCmd(0xE0, pvgamctrl, 14);
    const uint8_t nvgamctrl[14] = {0xF0, 0x04, 0x08, 0x08, 0x07, 0x03, 0x28,
                                   0x32, 0x40, 0x3B, 0x19, 0x18, 0x2A, 0x2E};
    sendCmd(0xE1, nvgamctrl, 14);
    const uint8_t gatectrl[3] = {0x25, 0x00, 0x00};
    sendCmd(0xE4, gatectrl, 3);

    // INVON is mandatory on this panel. Every vendor driver sends it
    // unconditionally; omit it and, for a 1-bit library, black and white
    // silently swap -- which looks exactly like an ink/paper mistake.
    //
    // The UI preference rides on the same command, XORed in: white-on-black is
    // simply the other state of a control the panel already has.
    sendCmd((geometry().invert != uiInverted_) ? 0x21 : 0x20);

    // The vendor sends SLPOUT *after* the register block, not before it.
    sendCmd(0x11); // SLPOUT
    sleep_ms(120);
    sendCmd(0x29); // DISPON
    sleep_ms(50);

    // Backlight: GPIO25 = PWM slice 4, channel B, active high, through a 1k
    // series resistor into an N-FET. A 100k/10k divider holds the gate at
    // ~0.3 V while the pin floats, so the panel cannot flash on during reset.
    gpio_set_function(lcd::BACKLIGHT, GPIO_FUNC_PWM);
    blSlice_ = pwm_gpio_to_slice_num(lcd::BACKLIGHT);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(blSlice_, &cfg, true);
    pwm_set_gpio_level(lcd::BACKLIGHT, 0);

    inited_ = true;
    return true;
}

void St7789_1in69::hardReset() {
    // The vendor uses 100 ms in each phase; the sibling board proved 20/20/120
    // adequate. Use the vendor's timing for this panel -- reset happens once.
    gpio_put(lcd::LCD_RST, 1);
    sleep_ms(100);
    gpio_put(lcd::LCD_RST, 0);
    sleep_ms(100);
    gpio_put(lcd::LCD_RST, 1);
    sleep_ms(100);
}

void St7789_1in69::setInverted(bool on) {
    if (on == uiInverted_) return;
    uiInverted_ = on;
    // Before init the value is simply remembered; init() sends it as part of the
    // register block. sendCmd waits for DMA to drain, so a mid-run toggle cannot
    // land inside a frame.
    if (inited_) sendCmd((geometry().invert != uiInverted_) ? 0x21 : 0x20);
}

void St7789_1in69::sendCmd(uint8_t cmd, const uint8_t* params, size_t len) {
    waitIdle();
    setFrameBits(8);

    gpio_put(lcd::LCD_CS, 0);
    gpio_put(lcd::LCD_DC, 0);
    spi_write_blocking(spi_, &cmd, 1);
    if (params && len) {
        gpio_put(lcd::LCD_DC, 1);
        spi_write_blocking(spi_, params, len);
    }
    gpio_put(lcd::LCD_CS, 1);
}

void St7789_1in69::waitIdle() {
    if (dmaChan_ >= 0) {
        dma_channel_wait_for_finish_blocking(dmaChan_);
    }
    // DMA finishing only means the FIFO is fed; the shift register may still be
    // clocking bits out. Dropping CS here would truncate the last pixels.
    while (spi_is_busy(spi_)) {
        tight_loop_contents();
    }
    releaseCs();
}

void St7789_1in69::setWindow(const onebit::Window& w) {
    // The +20 GRAM offset is already applied by PanelGeometry -- it lives in
    // data, not here, precisely so it gets debugged once. In portrait it lands
    // on the row axis; in landscape it migrates to the column axis.
    const uint8_t caset[4] = {
        static_cast<uint8_t>(w.colStart >> 8), static_cast<uint8_t>(w.colStart & 0xFF),
        static_cast<uint8_t>(w.colEnd >> 8), static_cast<uint8_t>(w.colEnd & 0xFF)};
    const uint8_t raset[4] = {
        static_cast<uint8_t>(w.rowStart >> 8), static_cast<uint8_t>(w.rowStart & 0xFF),
        static_cast<uint8_t>(w.rowEnd >> 8), static_cast<uint8_t>(w.rowEnd & 0xFF)};

    sendCmd(0x2A, caset, 4);
    sendCmd(0x2B, raset, 4);

    // RAMWR, then hold CS low with D/C high so pixel DMA streams straight into
    // GRAM without re-issuing anything per chunk. For a glyph-sized update the
    // per-transaction preamble otherwise dominates the pixel cost entirely.
    waitIdle();
    setFrameBits(8);
    const uint8_t ramwr = 0x2C;
    gpio_put(lcd::LCD_CS, 0);
    gpio_put(lcd::LCD_DC, 0);
    spi_write_blocking(spi_, &ramwr, 1);
    gpio_put(lcd::LCD_DC, 1);
    pixelMode_ = true;

    if (use16_) setFrameBits(16);
}

void St7789_1in69::writePixels(const uint8_t* data, size_t len) {
    if (dmaChan_ < 0) return;

    // One DMA channel, so the previous transfer must drain before another
    // starts. stripBuffer() alternates, so the expander has already filled the
    // *other* buffer while this one was in flight.
    dma_channel_wait_for_finish_blocking(dmaChan_);

    dma_channel_config c = dma_channel_get_default_config(dmaChan_);
    channel_config_set_transfer_data_size(&c, use16_ ? DMA_SIZE_16 : DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(spi_, true));

    dma_channel_configure(dmaChan_, &c, &spi_get_hw(spi_)->dr, data,
                          use16_ ? (len / 2) : len, true);
}

uint8_t* St7789_1in69::stripBuffer(size_t& capacityBytes) {
    capacityBytes = stripBytes_;
    // Alternate. With one DMA channel only one transfer is ever in flight, so
    // the buffer swapped to was drained before the current one started.
    cur_ ^= 1;
    return strip_[cur_];
}

void St7789_1in69::clear(onebit::Color c) {
    if (!strip_[0]) return;
    waitIdle();

    const uint16_t v = (c == onebit::BLACK) ? static_cast<uint16_t>(format().ink)
                                            : static_cast<uint16_t>(format().paper);

    uint8_t* fill = strip_[0];
    const bool little = format().order == onebit::ByteOrder::LittleEndian;
    for (size_t i = 0; i < stripBytes_ / 2; ++i) {
        fill[i * 2]     = little ? static_cast<uint8_t>(v & 0xFF) : static_cast<uint8_t>(v >> 8);
        fill[i * 2 + 1] = little ? static_cast<uint8_t>(v >> 8) : static_cast<uint8_t>(v & 0xFF);
    }

    setWindow(geometry().window(onebit::Rect{0, 0, width(), height()}, rotation()));

    const size_t rowBytes = static_cast<size_t>(width()) * 2u;
    const size_t rows = stripBytes_ / rowBytes;
    int16_t remaining = height();
    while (remaining > 0) {
        const size_t chunkRows =
            (static_cast<size_t>(remaining) < rows) ? static_cast<size_t>(remaining) : rows;
        cur_ = 0; // clear() owns the bus; no ping-pong needed
        writePixels(fill, chunkRows * rowBytes);
        remaining = static_cast<int16_t>(remaining - chunkRows);
    }
    waitIdle();
}

bool St7789_1in69::setBacklight(uint8_t level) {
    if (blSlice_ < 0) return false;
    pwm_set_gpio_level(lcd::BACKLIGHT, static_cast<uint16_t>(level));
    return true;
}

} // namespace board
