# h0urg1ass

A gesture-driven countdown timer for the [Waveshare RP2350-Touch-LCD-1.69][board],
rendered entirely in one bit.

> `h0urg1ass` — hourglass, with the two characters that could be bits spelled as bits.
> `0` and `1` are the only values a pixel takes on this display.

## What it is

A 1.69″ pocket timer you operate mostly by **moving it**. Flip it, set it down, pick it
up — the IMU is the primary control surface, because a 240×280 round-cornered screen has
no room for a control panel. Touch is reserved for the one thing gestures are bad at:
dialling in a duration, on two spinner columns that are live whenever the timer is idle
or the device is lying flat. The buzzer confirms that a gesture registered, so you are
never left guessing whether the device saw you.

The display is a colour TFT, but the whole UI is drawn into a **1-bit framebuffer** and
expanded on the way to the glass. That is a deliberate aesthetic constraint, not a hardware
limit — see [Visual language](docs/visual-language.md).

## What it does

Dial a duration — the spinner columns are live whenever the timer is idle or the device is lying
flat, so you can set one in the hand at power-on without putting the device down. Turn it over
to start, the gesture a real hourglass already has. Rest it on its end or lay it flat to pause;
stand it up again to carry on. Turn it over during a run and it resets to full and runs again.
Set it face down to silence a finished one.

Swipe sideways whenever the columns are showing for a settings screen — theme, brightness, the
dim-then-blank timeouts, the alarm length, and the battery calibration.

There is one screen: a split-flap `MM:SS` readout on an opaque panel, over a falling-sand
simulation. The board carries the number; the sand carries the feeling of the time passing.

The readout stays legible at one bit because **the panel is opaque and drawn last** — the sand
is painted first, then the panel over it, so black glyphs never land on black sand. Sand piles
up behind the panel when the device is tilted or turned over, and each flap cell is stamped
inverted where it does, so the contrast stays total whichever way the sand moves.

The display runs white on black, using the panel's own `INVON`/`INVOFF`. The framebuffer
convention is untouched: `BLACK` still means ink everywhere in the code.

## Status

**Running on hardware.** Bring-up is recorded in [docs/hardware.md](docs/hardware.md): the
display driven end to end at 62.5 MHz — 19.1 ms for a full 240×280 frame — touch at `0x15`,
IMU at `0x6B`, RTC at `0x51`, corner radius measured at ≈44 px.

Everything that does not need a board is tested on the host: 162 cases over the timer model,
the orientation classifier, the app state machine, the drag columns and the sand, and the
settings model, codec, flash store and screen, with the faces pinned to braille goldens. The
settings screen builds for the target — `firmware/flash.sh --build-only` succeeds — but has
not run on a board.

Known gaps: one sand tick costs ~4.5 ms, about 13% of the CPU at the 30 Hz the simulation
runs at, and the word-parallel fall step the bit-packed grid was laid out for is not written.
The duration ceiling is 99:59, because the flap board has five fixed-raster cells and cannot
grow. Off-state battery current has never been put on a meter. See
[the issue tracker](../../issues) for the rest.

## Hardware

| | |
|---|---|
| Board | Waveshare RP2350-Touch-LCD-1.69 ([wiki][board]) |
| MCU | RP2350A — 2× Cortex-M33 *or* 2× Hazard3 RISC-V, 520 kB SRAM, **no PSRAM** |
| Flash | 16 MB external QSPI NOR (W25Q128JVSIQ) |
| Display | 1.69″ IPS TFT, 240×280, ST7789V2, RGB565 over write-only SPI1, **~44 px rounded corners** |
| Touch | CST816-family, I²C `0x15`, **single contact only** + hardware gestures |
| IMU | QMI8658C 6-axis, I²C **`0x6B`** — the schematic straps SA0 low and predicts `0x6A`; the as-built board answers only at `0x6B`. INT1/INT2 on GPIO23/24 |
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

Requires the [pico-sdk](https://github.com/raspberrypi/pico-sdk) (2.x), CMake 3.16+, Ninja,
`arm-none-eabi-gcc` and `picotool`.

```sh
export PICO_SDK_PATH=~/pico-sdk   # firmware/flash.sh assumes this if unset
firmware/flash.sh                 # build, flash, run
firmware/flash.sh --monitor       # ... and stream the USB console
firmware/flash.sh --build-only    # compile only, do not touch the board
```

`flash.sh` asks the running firmware to reboot into BOOTSEL over its own USB interface, so
reflashing never needs the BOOT button — which is why `main()` idles three seconds before it
touches any hardware. If a build hangs before USB enumerates, BOOT + RESET is the way back.

The host tests need none of that — no board, no cross-compiler:

```sh
cmake -S tests -B tests/build && cmake --build tests/build && ./tests/build/h0urg1ass_tests
```

## Documentation

Start at **[docs/README.md](docs/README.md)** — it is the reading order for anyone picking
this project up cold.

## License

MIT — see [LICENSE](LICENSE).

[board]: https://www.waveshare.com/wiki/RP2350-Touch-LCD-1.69
[onebit]: https://github.com/Devin-Cooper/1bit-display
