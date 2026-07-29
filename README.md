# h0urg1ass

A gesture-driven multi-use timer for the [Waveshare RP2350-Touch-LCD-1.69][board],
rendered entirely in one bit.

> `h0urg1ass` — hourglass, with the two characters that could be bits spelled as bits.
> `0` and `1` are the only values a pixel takes on this display.

## What it is

A 1.69″ pocket timer you operate mostly by **moving it**. Flip it, set it down, pick it
up — the IMU is the primary control surface, because a 240×280 round-cornered screen has
no room for a control panel. Touch is reserved for the things gestures are bad at: dialling
in a duration and changing settings. The buzzer confirms that a gesture registered, so you
are never left guessing whether the device saw you.

The display is a colour TFT, but the whole UI is drawn into a **1-bit framebuffer** and
expanded on the way to the glass. That is a deliberate aesthetic constraint, not a hardware
limit — see [Visual language](docs/visual-language.md).

## Status

**Pre-implementation.** The repository currently holds the hardware brief, the graphics
library survey, and the design docs needed to start work. No firmware yet, and the board has
not been brought up. See [the issue tracker](../../issues) for the backlog.

## Hardware

| | |
|---|---|
| Board | Waveshare RP2350-Touch-LCD-1.69 ([wiki][board]) |
| MCU | RP2350A — 2× Cortex-M33 *or* 2× Hazard3 RISC-V, 520 kB SRAM, **no PSRAM** |
| Flash | 16 MB external QSPI NOR (W25Q128JVSIQ) |
| Display | 1.69″ IPS TFT, 240×280, ST7789V2, RGB565 over write-only SPI1, **~44 px rounded corners** |
| Touch | CST816-family, I²C `0x15`, **single contact only** + hardware gestures |
| IMU | QMI8658C 6-axis, I²C `0x6A`, INT1/INT2 on GPIO23/24 |
| RTC | PCF85063A, I²C `0x51` — **no backup cell fitted**, so wall-clock does not survive power-off |
| Feedback | Buzzer on GPIO2 (PWM1 A), PWM backlight on GPIO25 |
| Power | Li-ion + ETA6096 charger; **firmware must assert GPIO15 to stay powered on battery** |

Full pin map, timing budget and the traps that cost real debugging time:
**[docs/hardware.md](docs/hardware.md)**.

## Graphics

Rendering is the [`onebit`][onebit] library, vendored as a submodule at
`third_party/1bit-display`. It already ships `PanelGeometry::st7789_240x280_1in69()` for
this exact panel, and a hardware-validated pico-sdk reference app for the sibling 2.8″
board. What it gives us, what it doesn't, and the rule about not deviating outside its
capabilities: **[docs/display-library.md](docs/display-library.md)**.

## Getting the source

```sh
git clone --recurse-submodules git@github.com:Devin-Cooper/h0urg1ass.git
cd h0urg1ass
```

Already cloned without submodules:

```sh
git submodule update --init --recursive
```

> **Note:** `1bit-display` is currently a **private** repository, so the submodule only
> resolves for accounts with access to it. Until it is made public, an anonymous clone of
> `h0urg1ass` will succeed but the submodule checkout will fail.

## Building

Requires the [pico-sdk](https://github.com/raspberrypi/pico-sdk) (2.x), CMake 3.16+, and
`arm-none-eabi-gcc`. Build instructions land with the first firmware milestone.

## Documentation

Start at **[docs/README.md](docs/README.md)** — it is the reading order for anyone picking
this project up cold.

## License

MIT — see [LICENSE](LICENSE).

[board]: https://www.waveshare.com/wiki/RP2350-Touch-LCD-1.69
[onebit]: https://github.com/Devin-Cooper/1bit-display
