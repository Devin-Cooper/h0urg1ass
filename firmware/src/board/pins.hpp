#pragma once

// Pin map for the Waveshare RP2350-Touch-LCD-1.69.
//
// Every assignment here is corroborated by the board schematic and by at least
// one vendor pin header; the five vendor artifacts agree with each other and
// with the schematic. See docs/hardware.md for the sourcing and the traps.
//
// Do not reuse a config from the RP2350-Touch-LCD-2.8 or -1.28. Different
// controller, different touch IC, different resolution, different offsets.

#include <cstdint>

namespace board {

// ---------------------------------------------------------------- display --
// ST7789V2, 240x280, 4-wire SPI on SPI1. Write-only: MISO is not wired.
namespace lcd {
inline constexpr unsigned LCD_DC   = 8;   // 0 = command, 1 = data
inline constexpr unsigned LCD_CS   = 9;   // plain GPIO, NOT GPIO_FUNC_SPI -- toggled by hand
inline constexpr unsigned LCD_SCK  = 10;  // GPIO_FUNC_SPI, SPI1 SCK, mode 0
inline constexpr unsigned LCD_MOSI = 11;  // GPIO_FUNC_SPI, SPI1 TX
inline constexpr unsigned LCD_RST  = 13;  // active low
inline constexpr unsigned BACKLIGHT = 25; // GPIO_FUNC_PWM, slice 4 channel B, active high
                                      // (NOT GPIO14 -- that is the power button)

inline constexpr int WIDTH  = 240;
inline constexpr int HEIGHT = 280;

// The 240x280 panel sits centred in a 240x320 GRAM, so a +20 offset applies on
// the 280-pixel axis -- RASET in portrait, and it MOVES to CASET in landscape.
inline constexpr int GRAM_OFFSET = 20;

// Rounded corners physically clip the panel at roughly this radius.
inline constexpr int CORNER_RADIUS_PX = 44;
} // namespace lcd

// -------------------------------------------------------------------- i2c --
// One bus, three devices. No address clash, but access must be serialised.
namespace i2c {
inline constexpr unsigned SDA = 6;
inline constexpr unsigned SCL = 7;
inline constexpr unsigned BAUD_HZ = 400'000;

inline constexpr uint8_t ADDR_TOUCH = 0x15; // CST816 family
inline constexpr uint8_t ADDR_RTC   = 0x51; // PCF85063A

// The QMI8658C address depends on how SA0/SDO is strapped. The schematic reads
// as tied to GND (0x6A), but this board answers on 0x6B -- confirmed by scan on
// hardware, 2026-07-29. Vendor drivers probe both, which is now clearly why.
inline constexpr uint8_t ADDR_IMU     = 0x6A; // SA0 low  (per schematic)
inline constexpr uint8_t ADDR_IMU_ALT = 0x6B; // SA0 high (what this board does)
} // namespace i2c

namespace touch {
inline constexpr unsigned INT = 21; // active low, internal pull-up
inline constexpr unsigned RST = 22; // active low, push-pull output
} // namespace touch

namespace imu {
inline constexpr unsigned INT1 = 23;
inline constexpr unsigned INT2 = 24;
} // namespace imu

namespace rtc {
inline constexpr unsigned INT = 18; // input, internal pull-up
} // namespace rtc

// ------------------------------------------------------------ power, misc --
namespace power {
// Must be driven high to stay powered on battery. The board behaves perfectly
// over USB without this, which is exactly why it is easy to miss.
inline constexpr unsigned SYS_EN = 15;

inline constexpr unsigned PWR_KEY = 14; // Key1, power button, input with pull-up
inline constexpr unsigned BAT_ADC = 29; // ADC channel 3
inline constexpr unsigned BAT_ADC_CHANNEL = 3;

// Divider is 200k (high) / 100k (low), so V_batt = V_adc * 3.
inline constexpr float BAT_DIVIDER_RATIO = 3.0f;
} // namespace power

inline constexpr unsigned BUZZER = 2; // via 4.7k into an SS8050 NPN; PWM1 A

// ------------------------------------------------------------- unusable ----
// GPIO3, 4, 5, 12 and 19 are net-labelled in the RP2350A symbol but connected
// to nothing and not brought out to any pad. GPIO12 in particular appears in
// the vendor headers as LCD_MISO; it is a phantom.
//
// Broken out on test points TP1-TP11: GPIO0, 1, 16, 17, 20, 26, 27, 28.
// GPIO26/27/28 are ADC0/1/2. GPIO0 is the only viable QMI CS1n if PSRAM is
// ever bodged on.

} // namespace board
