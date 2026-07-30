#include "board/qmi8658.hpp"

#include <hardware/i2c.h>
#include <pico/stdlib.h>

#include "board/pins.hpp"

namespace board {

namespace {

// Register map. Ax_L at 0x35 is the anchor that pins the rest.
constexpr uint8_t REG_WHO_AM_I = 0x00;
constexpr uint8_t REG_REVISION = 0x01;
constexpr uint8_t REG_CTRL1 = 0x02;
constexpr uint8_t REG_CTRL2 = 0x03;
constexpr uint8_t REG_CTRL5 = 0x06;
constexpr uint8_t REG_CTRL7 = 0x08;
constexpr uint8_t REG_AX_L = 0x35;

constexpr uint8_t WHO_AM_I_VALUE = 0x05;

// CTRL1: address auto-increment. Matches the value every vendor driver writes.
constexpr uint8_t CTRL1_CONFIG = 0x60;

// CTRL2: accelerometer full scale and output rate.
//   4g -- we only ever measure 1g of gravity, so a smaller range means finer
//         resolution per LSB. 8g would waste a bit for no benefit here.
//   125 Hz -- far more than a 350 ms dwell needs, and cheap.
constexpr uint8_t ACC_RANGE_4G = 0x01 << 4;
constexpr uint8_t ACC_ODR_125HZ = 0x06;
constexpr uint8_t CTRL2_CONFIG = ACC_RANGE_4G | ACC_ODR_125HZ;

// CTRL5: hardware low-pass off. Filtering happens in software, where it can be
// tuned and tested on a host. (The vendor driver also ends up here, though by
// accident -- it computes a value and then unconditionally overwrites it.)
constexpr uint8_t CTRL5_CONFIG = 0x00;

// CTRL7: accelerometer only.
constexpr uint8_t CTRL7_ACC_ONLY = 0x01;

/// Counts to g. Full scale maps to 32768 counts.
constexpr float LSB_TO_G = 4.0f / 32768.0f;

constexpr uint32_t I2C_TIMEOUT_US = 5000;

} // namespace

bool Qmi8658::writeReg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_write_timeout_us(i2c1, addr_, buf, 2, false, I2C_TIMEOUT_US) == 2;
}

bool Qmi8658::readRegs(uint8_t reg, uint8_t* dst, size_t len) {
    if (i2c_write_timeout_us(i2c1, addr_, &reg, 1, true, I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(i2c1, addr_, dst, len, false, I2C_TIMEOUT_US) ==
           static_cast<int>(len);
}

bool Qmi8658::begin() {
    // Probe the strapped-high address first: that is what this board actually
    // uses, whatever the schematic says.
    const uint8_t candidates[2] = {board::i2c::ADDR_IMU_ALT, board::i2c::ADDR_IMU};
    bool found = false;
    for (uint8_t a : candidates) {
        addr_ = a;
        uint8_t who = 0;
        if (readRegs(REG_WHO_AM_I, &who, 1) && who == WHO_AM_I_VALUE) { found = true; break; }
    }
    if (!found) { addr_ = 0; return false; }

    readRegs(REG_REVISION, &revision_, 1);

    if (!writeReg(REG_CTRL1, CTRL1_CONFIG)) return false;
    if (!writeReg(REG_CTRL2, CTRL2_CONFIG)) return false;
    if (!writeReg(REG_CTRL5, CTRL5_CONFIG)) return false;
    if (!writeReg(REG_CTRL7, CTRL7_ACC_ONLY)) return false;

    sleep_ms(20); // let the first conversions land before anyone reads
    return true;
}

bool Qmi8658::readRaw(h0::Vec3& out) {
    if (addr_ == 0) return false;
    uint8_t b[6];
    if (!readRegs(REG_AX_L, b, sizeof(b))) return false;

    // Little-endian, LSB first. CTRL1 bit 5 nominally selects big-endian, but
    // every vendor driver writes 0x60 and then combines LSB-first anyway -- so
    // the bit does not do what its name suggests on this part. Matching the
    // proven combination rather than the documented one.
    const int16_t x = static_cast<int16_t>(static_cast<uint16_t>(b[1]) << 8 | b[0]);
    const int16_t y = static_cast<int16_t>(static_cast<uint16_t>(b[3]) << 8 | b[2]);
    const int16_t z = static_cast<int16_t>(static_cast<uint16_t>(b[5]) << 8 | b[4]);

    out.x = static_cast<float>(x) * LSB_TO_G;
    out.y = static_cast<float>(y) * LSB_TO_G;
    out.z = static_cast<float>(z) * LSB_TO_G;
    return true;
}

bool Qmi8658::read(h0::Vec3& out) {
    h0::Vec3 raw;
    if (!readRaw(raw)) return false;
    out = axisMap(raw);
    return true;
}

h0::Vec3 axisMap(const h0::Vec3& d) {
    // Established on hardware, 2026-07-30. The part is rotated a quarter turn
    // relative to the panel and its z faces the other way, which no datasheet
    // states -- it depends purely on how the die was placed on the PCB.
    //
    // An accelerometer at rest reads the reaction to gravity, so the measured
    // vector points UP while gravity points DOWN; hence the negations. On top
    // of that:
    //
    //   device y -> panel x      (the quarter turn: holding the board upright
    //   device x -> panel y       read as EDGE, and on its edge read as UPRIGHT)
    //   device z -> panel z, uninverted
    //                            (flat screen-up read as FACEDOWN and vice
    //                             versa, so this axis was doubly negated)
    return h0::Vec3{-d.y, -d.x, d.z};
}

} // namespace board
