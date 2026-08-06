#include "board/st7789_1in69.hpp"

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pwm.h>
#include <pico/stdlib.h>

#include <cstdlib>

#include "board/pins.hpp"

namespace board {

namespace {

// Transcribed verbatim from the previous init(). Order, parameters and delays
// are load-bearing -- this is the sequence the panel was tuned with, and the
// gamma tables in particular are specific to this module.
//
// No MADCTL entry: onebit::St7789Display::init() emits 0x36 itself, from
// geometry().madctlFor(rotation()), after this table. That is the one command
// that has to agree with the window addressing the driver computes, so the
// driver owns it. A pinned entry here would be dead bytes at best and a
// contradiction at any rotation but Rot0.
//
// The old INVON/INVOFF line was `(geometry().invert != uiInverted_) ? 0x21 :
// 0x20`, sent once during init with uiInverted_ still at its default (false).
// geometry().invert is true for this panel, so that always resolved to INVON
// (0x21) -- pinned here the same way. setInverted() is never called anywhere
// in this firmware (see main.cpp's colour-vs-inversion comment), so this
// static entry is the only place that command is ever sent.
//
// Namespace scope is required, not incidental: St7789Config borrows this
// table by pointer and walks it on every init().
const uint8_t kColmod[]     = {0x05}; // 16 bpp on the MCU interface
const uint8_t kPorch[]      = {0x0B, 0x0B, 0x00, 0x33, 0x35};
const uint8_t kGctrl[]      = {0x11};
const uint8_t kVcoms[]      = {0x35};
const uint8_t kLcmctrl[]    = {0x2C};
const uint8_t kVdvvrhen[]   = {0x01};
const uint8_t kVrhs[]       = {0x0D};
const uint8_t kVdvs[]       = {0x20};
const uint8_t kFrctrl2[]    = {0x13};
const uint8_t kPwctrl1[]    = {0xA4, 0xA1};
const uint8_t kD6[]         = {0xA1}; // undocumented, present in every vendor sequence
const uint8_t kPvgamctrl[]  = {0xF0, 0x06, 0x0B, 0x0A, 0x09, 0x26, 0x29,
                               0x33, 0x41, 0x18, 0x16, 0x15, 0x29, 0x2D};
const uint8_t kNvgamctrl[]  = {0xF0, 0x04, 0x08, 0x08, 0x07, 0x03, 0x28,
                               0x32, 0x40, 0x3B, 0x19, 0x18, 0x2A, 0x2E};
const uint8_t kGatectrl[]   = {0x25, 0x00, 0x00};

const onebit::St7789InitCmd kInit[] = {
    {0x3A, sizeof(kColmod),    kColmod,    0},
    {0xB2, sizeof(kPorch),     kPorch,     0},
    {0xB7, sizeof(kGctrl),     kGctrl,     0},
    {0xBB, sizeof(kVcoms),     kVcoms,     0},
    {0xC0, sizeof(kLcmctrl),   kLcmctrl,   0},
    {0xC2, sizeof(kVdvvrhen),  kVdvvrhen,  0},
    {0xC3, sizeof(kVrhs),      kVrhs,      0},
    {0xC4, sizeof(kVdvs),      kVdvs,      0},
    {0xC6, sizeof(kFrctrl2),   kFrctrl2,   0},
    {0xD0, sizeof(kPwctrl1),   kPwctrl1,   0},
    {0xD6, sizeof(kD6),        kD6,        0},
    {0xE0, sizeof(kPvgamctrl), kPvgamctrl, 0},
    {0xE1, sizeof(kNvgamctrl), kNvgamctrl, 0},
    {0xE4, sizeof(kGatectrl),  kGatectrl,  0},
    {0x21, 0, nullptr, 0},   // INVON -- mandatory on this panel, see above
    {0x11, 0, nullptr, 120}, // SLPOUT, sent after the register block, not before it
    {0x29, 0, nullptr, 50},  // DISPON
};

onebit::St7789Config makeConfig() {
    onebit::St7789Config cfg;
    cfg.geometry = onebit::PanelGeometry::st7789_240x280_1in69();
    cfg.init = kInit;
    cfg.initCount = sizeof(kInit) / sizeof(kInit[0]);
    return cfg;
}

} // namespace

St7789_1in69::St7789_1in69(onebit::Rotation rot, uint32_t spiBaud, int stripRows,
                           bool use16BitFrames)
    : St7789Display(makeConfig(), rot)
    , spi_(spi1)
    , requestedBaud_(spiBaud)
    , stripRows_(stripRows)
    , use16_(use16BitFrames) {
    // No rotation guard: onebit::St7789Display::init() sends MADCTL from
    // geometry().madctlFor(rotation()), so the panel scans the axis the
    // window addressing assumes. Only Rot0 has been seen on this glass --
    // see the header.
}

St7789_1in69::~St7789_1in69() {
    if (dmaChan_ >= 0) dma_channel_unclaim(dmaChan_);
    for (uint8_t*& b : strips_) {
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

bool St7789_1in69::begin() {
    stripBytes_ = static_cast<size_t>(width()) * 2u * static_cast<size_t>(stripRows_);
    for (uint8_t*& b : strips_) {
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

    // Backlight: GPIO25 = PWM slice 4, channel B, active high, through a 1k
    // series resistor into an N-FET. A 100k/10k divider holds the gate at
    // ~0.3 V while the pin floats, so the panel cannot flash on during reset.
    // Set up here rather than after the panel init as the old code did --
    // the two peripherals are independent, and this way begin() ends with the
    // panel bring-up, matching every other board::*::begin() in this firmware.
    gpio_set_function(lcd::BACKLIGHT, GPIO_FUNC_PWM);
    blSlice_ = pwm_gpio_to_slice_num(lcd::BACKLIGHT);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(blSlice_, &cfg, true);
    pwm_set_gpio_level(lcd::BACKLIGHT, 0);

    const bool ok = St7789Display::init();
    inited_ = ok;
    return ok;
}

void St7789_1in69::sendCommand(uint8_t cmd, const uint8_t* params, size_t len) {
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

void St7789_1in69::beginPixelStream(uint8_t writeCmd) {
    // RAMWR, then hold CS low with D/C high so pixel DMA streams straight into
    // GRAM without re-issuing anything per chunk. For a glyph-sized update the
    // per-transaction preamble otherwise dominates the pixel cost entirely.
    waitIdle();
    setFrameBits(8);
    gpio_put(lcd::LCD_CS, 0);
    gpio_put(lcd::LCD_DC, 0);
    spi_write_blocking(spi_, &writeCmd, 1);
    gpio_put(lcd::LCD_DC, 1);
    pixelMode_ = true;

    if (use16_) setFrameBits(16);
}

void St7789_1in69::sendPixels(const uint8_t* data, size_t len) {
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
    // A 16-bit DMA read takes two memory bytes as a native (little-endian)
    // halfword and shifts it out MSB-first, which sends that pair in the
    // WRONG order for a big-endian buffer -- every pixel's bytes would swap.
    // bswap corrects that in the DMA itself, so the buffer needs no
    // pre-swapping. At 8 bits the DMA moves one byte at a time and there is
    // nothing to swap.
    //
    // Read from format().order rather than assumed: the expander's byte order
    // is set by a DEFAULT ARGUMENT on PixelFormat::rgb565() in the library,
    // which onebit::St7789Display calls with no arguments at all. Nothing
    // enforces that default across the repository boundary, and the failure
    // mode is the invisible kind -- WHITE and PAPER are 0x0000 and 0xFFFF,
    // both byte-swap-invariant, so a change upstream would look perfect in
    // the default theme and only corrupt colour in AMBER and NIGHT. Testing
    // the field instead makes this transport self-correcting either way.
    channel_config_set_bswap(&c, use16_ && format().order == onebit::ByteOrder::BigEndian);

    dma_channel_configure(dmaChan_, &c, &spi_get_hw(spi_)->dr, data,
                          use16_ ? (len / 2) : len, true);
}

void St7789_1in69::endPixelStream() {
    if (pixelMode_) {
        // St7789Display::clear() calls this right after the final sendPixels()
        // of the last chunk, with no intervening waitIdle() -- endPixelStream()
        // is the only post-stream hook the base class exposes, so its contract
        // implicitly requires the transport to leave the bus safe here.
        // Without draining first, the last chunk's DMA transfer (and/or the
        // SPI shift register) can still be in flight when CS goes high,
        // truncating it -- on this panel, at 40-row strips, that is the
        // bottom 40 rows of a clear() silently never reaching GRAM.
        if (dmaChan_ >= 0) {
            dma_channel_wait_for_finish_blocking(dmaChan_);
        }
        while (spi_is_busy(spi_)) {
            tight_loop_contents();
        }
        gpio_put(lcd::LCD_CS, 1);
        pixelMode_ = false;
    }
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

void St7789_1in69::delayMs(uint32_t ms) { sleep_ms(ms); }

void St7789_1in69::waitIdle() {
    if (dmaChan_ >= 0) {
        dma_channel_wait_for_finish_blocking(dmaChan_);
    }
    // DMA finishing only means the FIFO is fed; the shift register may still be
    // clocking bits out. Dropping CS here would truncate the last pixels.
    while (spi_is_busy(spi_)) {
        tight_loop_contents();
    }
    endPixelStream();
}

uint8_t* St7789_1in69::stripBuffer(size_t& capacityBytes) {
    capacityBytes = stripBytes_;
    // Alternate. With one DMA channel only one transfer is ever in flight, so
    // the buffer swapped to was drained before the current one started.
    nextStrip_ ^= 1;
    return strips_[nextStrip_];
}

void St7789_1in69::setInverted(bool on) {
    if (on == uiInverted_) return;
    uiInverted_ = on;
    // Deferred until begin() has actually run: St7789Display::setInverted()
    // sends immediately, and calling it before spi_init()/the GPIO setup in
    // begin() would poke uninitialised SPI. Before that point the preference
    // is simply remembered, same as the pre-migration guard.
    if (inited_) {
        // XORed against the panel's own polarity requirement, not sent
        // verbatim -- see the class declaration's comment. This is the same
        // trick the static INVON entry in kInit uses for the boot-time send.
        St7789Display::setInverted(geometry().invert != uiInverted_);
    }
}

void St7789_1in69::sleepIn() {
    if (!inited_) return;
    St7789Display::sleepIn();
    // The controller needs a moment before the rail goes. This delay was in
    // the pre-migration sleepIn() and the base class does not have it; the
    // only caller, PowerButton::shutdown(), drops SYS_EN a few hundred
    // microseconds later and the rail collapses in ~50 us on battery, so
    // there is nowhere else for this to happen.
    sleep_ms(5);
}

bool St7789_1in69::setBacklight(uint8_t level) {
    if (blSlice_ < 0) return false;
    pwm_set_gpio_level(lcd::BACKLIGHT, static_cast<uint16_t>(level));
    return true;
}

} // namespace board
