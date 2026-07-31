#include "board/cst816.hpp"

#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <pico/stdlib.h>

#include "board/pins.hpp"

namespace board {

namespace {

// 0x01 GestureID, 0x02 FingerNum, 0x03..0x06 X/Y -- read as one burst.
constexpr uint8_t REG_GESTURE = 0x01;
constexpr uint8_t REG_CHIP_ID = 0xA7;
constexpr uint8_t REG_IRQ_CTL = 0xFA;
constexpr uint8_t REG_IRQ_PULSE_WIDTH = 0xED; ///< units of 0.1 ms
constexpr uint8_t REG_NOR_SCAN_PER = 0xEE;    ///< units of 10 ms
constexpr uint8_t REG_DIS_AUTO_SLEEP = 0xFE;

// IrqCtl bits. Without EnTouch the controller never asserts INT at all, so an
// interrupt-driven reader sees nothing and a polled one sees a ~1 ms pulse it
// almost always misses.
constexpr uint8_t IRQ_EN_TOUCH = 0x40;  ///< assert while a finger is down
constexpr uint8_t IRQ_EN_CHANGE = 0x20; ///< assert when the report changes

constexpr uint8_t CHIP_ID_EXPECTED = 0xB5;
constexpr uint8_t DIS_AUTO_SLEEP = 0x07;   ///< hold awake
constexpr uint8_t ALLOW_AUTO_SLEEP = 0x00; ///< stand down after 2 s idle

constexpr uint32_t I2C_TIMEOUT_US = 5000;

} // namespace

bool Cst816::readRegs(uint8_t reg, uint8_t* dst, size_t len) {
    if (i2c_write_timeout_us(i2c1, board::i2c::ADDR_TOUCH, &reg, 1, true, I2C_TIMEOUT_US) != 1) {
        return false;
    }
    return i2c_read_timeout_us(i2c1, board::i2c::ADDR_TOUCH, dst, len, false, I2C_TIMEOUT_US) ==
           static_cast<int>(len);
}

bool Cst816::writeReg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(i2c1, board::i2c::ADDR_TOUCH, buf, 2, false, I2C_TIMEOUT_US) == 2;
}

bool Cst816::begin() {
    // Without this pulse the part never answers at all.
    gpio_init(board::touch::RST);
    gpio_set_dir(board::touch::RST, GPIO_OUT);
    gpio_put(board::touch::RST, 0);
    sleep_ms(20);
    gpio_put(board::touch::RST, 1);
    sleep_ms(80); // it needs this long before it will ACK

    gpio_init(board::touch::INT);
    gpio_set_dir(board::touch::INT, GPIO_IN);
    gpio_pull_up(board::touch::INT); // pull-UP, so errata E9 does not apply

    if (!readRegs(REG_CHIP_ID, &chipId_, 1)) return false;
    if (chipId_ != CHIP_ID_EXPECTED) return false;

    // Start in standby-permitted. The picker is not live at boot, and holding
    // the part awake costs 1.6 mA against 6 uA for a responsiveness nobody can
    // use until the device is laid flat.
    if (!writeReg(REG_DIS_AUTO_SLEEP, ALLOW_AUTO_SLEEP)) return false;

    // Actually enable the interrupt. Omitting this is silent: the part still
    // answers register reads perfectly, it simply never signals, so a reader
    // waiting on INT gets nothing and looks like a wiring fault.
    if (!writeReg(REG_IRQ_CTL, IRQ_EN_TOUCH | IRQ_EN_CHANGE)) return false;

    // Widen the pulse from the default 1 ms. Even with an edge interrupt a
    // wider pulse is cheap insurance, and it lets a polled fallback work.
    writeReg(REG_IRQ_PULSE_WIDTH, 20); // 2 ms
    writeReg(REG_NOR_SCAN_PER, 1);     // 10 ms scan -> ~100 Hz while touched
    return true;
}

bool Cst816::read(TouchPoint& out) {
    // Gesture, finger count and both coordinates in one burst: 0x01..0x06.
    // Separate transactions would let the reported point belong to a different
    // sample than the reported finger count.
    uint8_t b[6];
    if (!readRegs(REG_GESTURE, b, sizeof(b))) return false;

    const TouchGesture g = static_cast<TouchGesture>(b[0]);
    out.gesture = g;
    // Edge, not level. See the field's comment in the header.
    out.gestureIsNew = (g != TouchGesture::None) && (g != lastGesture_);
    lastGesture_ = g;

    out.pressed = (b[1] & 0x0F) != 0; // FingerNum: 0 or 1, nothing else exists

    // 12-bit coordinates: the high nibble of each pair carries the top 4 bits,
    // and the upper nibble holds flags that must be masked off or the point
    // lands off-panel.
    const int16_t x = static_cast<int16_t>((static_cast<uint16_t>(b[2] & 0x0F) << 8) | b[3]);
    const int16_t y = static_cast<int16_t>((static_cast<uint16_t>(b[4] & 0x0F) << 8) | b[5]);

    // A 12-bit mask is not a range check: one corrupt sample of ~4095 becomes a
    // ~4000 px drag delta, which after the picker's velocity gain is ~600 units
    // in a single frame -- enough to slam a dialled duration to zero with no
    // undo. So the POSITION is rejected...
    out.positionValid = (x >= 0 && x < 240 && y >= 0 && y < 280);
    if (!out.positionValid) {
        // ...but `pressed` keeps reporting what the controller said about
        // contact. Clearing it here made a corrupt sample look exactly like a
        // release, which silently cleared any state latched for the duration of
        // one touch.
        return true;
    }

    out.x = x;
    out.y = y;
    return true;
}

bool Cst816::setHeldAwake(bool awake) {
    if (awake == heldAwake_) return true;
    if (!writeReg(REG_DIS_AUTO_SLEEP, awake ? DIS_AUTO_SLEEP : ALLOW_AUTO_SLEEP)) return false;
    heldAwake_ = awake;
    return true;
}

} // namespace board
