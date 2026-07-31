#pragma once

#include <cstddef>
#include <cstdint>

#include "input/orientation.hpp"

namespace board {

/// QMI8658C 6-axis IMU, accelerometer only.
///
/// The gyro is deliberately left off: orientation needs only the gravity
/// vector, and enabling the gyro costs roughly 4x the entire rest of the IMU
/// budget (754 uA at 250 Hz against 155 uA accel-only).
///
/// **The part answers at 0x6B on this board, not the 0x6A the schematic
/// implies.** The datasheet is explicit that SA0 low gives 0x6A and SA0 pulled
/// up gives 0x6B; the as-built strap is high. The vendor's own MicroPython
/// driver hardcodes 0x6B and its C driver probes both. `begin()` probes both
/// too, and reports which one answered.
class Qmi8658 {
public:
    /// Probe both addresses, verify WHO_AM_I, and configure for orientation.
    /// Returns false if neither address responds with 0x05.
    bool begin();

    /// CTRL1.SensorDisable. About 6 uA against 30 uA for accel-only low power.
    bool powerDown();

    /// One accelerometer sample as a gravity direction in the PANEL frame:
    /// +x right across the screen, +y down the screen, +z out of the screen.
    ///
    /// The IMU's own axes are not the panel's -- the part is soldered in
    /// whatever orientation suited the layout -- so `axisMap` below is applied
    /// here. Returns false on a bus error, leaving `out` untouched.
    bool read(h0::Vec3& out);

    /// Raw device-frame reading in g, before any axis remapping. Bring-up only:
    /// this is what you look at to work out what the mapping should be.
    bool readRaw(h0::Vec3& out);

    uint8_t address() const { return addr_; }
    uint8_t revision() const { return revision_; }

private:
    bool writeReg(uint8_t reg, uint8_t value);
    bool readRegs(uint8_t reg, uint8_t* dst, size_t len);

    uint8_t addr_ = 0;
    uint8_t revision_ = 0;
};

/// Map the IMU's device axes onto the panel frame.
///
/// Determined on hardware by holding the board in known postures and reading
/// the raw vector -- there is no way to derive it from a datasheet, because it
/// depends on how the part is rotated on the PCB.
h0::Vec3 axisMap(const h0::Vec3& device);

} // namespace board
