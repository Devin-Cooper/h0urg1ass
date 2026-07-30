# Documentation

Design and reference material for `h0urg1ass` — a gesture-driven countdown timer on the
Waveshare RP2350-Touch-LCD-1.69, rendered entirely in one bit.

The firmware exists: `firmware/src/` is the device, built and flashed from `firmware/`, with the
host-side test suite in `tests/`. These five documents predate it. They are the reasoning that
produced it — what the hardware is, what the graphics library can do, how the thing should look,
how it should be controlled, and how it should behave on a battery — not a description of it.

**Where a document and the source disagree, the source is right and the document is stale.** Read
these for *why* a constraint exists; read `firmware/src/` for what the device does. The passages
known to be superseded are called out in the table below.

## Reading order

Read in this order on a cold start. Each one assumes the ones above it.

| # | Document | What it answers |
|---|---|---|
| 1 | **[hardware.md](hardware.md)** | What the board is, the complete pin map, the traps ranked by how much time they cost, the rounded-corner safe area, the timing budget, and a verifiable bring-up order. **Read *Confirmed on hardware* first.** It is the 2026-07-29 bench record and it overrides the schematic-derived tables above it — most sharply on the IMU, which answers at `0x6B` while §1 still prints `0x6A`. |
| 2 | **[display-library.md](display-library.md)** | What the graphics library gives us and what it does not. Framebuffers, primitives, large text, patterns, dirty rects, colour, the HAL, host-side testing, and a gaps list. |
| 3 | **[visual-language.md](visual-language.md)** | How this should look. One-bit technique extracted from Lucas Pope's practice, what does and does not transfer from the Playdate, and a rule set mapped onto real library symbols. **§9 is superseded.** The hourglass is not a dithered silhouette whose fill tracks the remaining fraction, and it is not a bowtie: it is a grain-level falling-sand simulation over a flat floor with a hole (`firmware/src/sand/`), so the pattern-anchoring question §9 agonises over never arose. |
| 4 | **[interaction.md](interaction.md)** | Motion as the primary control surface. What the IMU does in hardware versus software, honest per-gesture ratings, the touch model, and touch target sizing. Its recommended vocabulary — double-tap plus shake — is **not** what shipped: the commands are posture transitions (stand up = start/resume, lay flat = pause and set, turn over = reset and run, face down = silence; `firmware/src/input/orientation.hpp`), and no tap engine is configured at all. Read it for the ratings, the bus arithmetic and the power tables, not as a spec. |
| 5 | **[power-and-time.md](power-and-time.md)** | Battery, sleep, and keeping time with no RTC backup cell fitted. Includes the alarm protocol that avoids sleeping through your own alarm. |

## The five constraints everything else follows from

1. **240 × 280, one bit.** No greys. Tone comes from pattern, separation from edges.
2. **~44 px rounded corners physically clip the panel.** Nothing readable or touchable goes
   near a corner. See the safe-area numbers in [hardware.md](hardware.md).
3. **No RTC backup cell is fitted.** Wall-clock time does not survive a power cycle. Timers
   must be correct without ever knowing the time of day.
4. **Firmware must assert GPIO15 early** or the board switches off the moment the power
   button is released on battery. This is invisible over USB.
5. **The screen is a tiny target.** Motion gestures carry the running controls. Touch does one
   job — dialling the duration — and only while the device is laid flat, because a dial that
   worked while running would let a pocket rewrite the timer and there is no undo. The control is
   a continuous vertical drag on two half-panel-wide columns, tracked from raw coordinates rather
   than the controller's gesture codes; the sideways swipe is deliberately left unbound.

## Project rule on the graphics library

The UI stays inside the capabilities of the graphics library vendored at
`third_party/1bit-display`. Where something genuinely needed is missing, the fix is to
extend that library upstream as explicit, separate work — not to reach around it with
one-off drawing code here. [display-library.md](display-library.md) §12 tracks the known
gaps and, for each, whether the right home is upstream or local.

Two places do write framebuffer bytes directly, each confined to one file and each paid for by a
measured win. `render/raster_ops.cpp`: 240 px is 30 whole bytes with no row padding, so a 180°
rotation of the finished frame is a reversal of the byte array with every byte bit-reversed, and
the scoped invert is a whole-byte complement rather than a masked read-modify-write.
`sand/sand_render.cpp`: a 104-cell row expands to 208 px through a byte-wide doubling table
instead of per-pixel writes, worth roughly an order of magnitude, which is why the sand origin is
held byte-aligned. Both check the assumption they rest on and bail if it does not hold. Neither is
a precedent — anything else that wants raw bytes is a gap to raise upstream.

## A note on confidence

This board was **brought up on 2026-07-29**. The bench results are in
[hardware.md](hardware.md) under *Confirmed on hardware*: A2 silicon, SPI clean at 62.5 MHz, a
full 240 × 280 frame in 19.1 ms, a 44 px corner radius measured off a drawn frame, and a 16 px
safe inset visible with margin. Everything outside that section is still schematic, vendor demo or
datasheet, and says so inline.

Where the bench contradicts the paper, the paper has not always been corrected. **The IMU answers
at `0x6B`, not the `0x6A` the schematic's SDO-to-GND strap predicts** — the as-built strap is high,
and a second error in the same schematic block (a `1V8` net with no source on a 3.3 V-only board)
says that block is not trustworthy. The block table in hardware.md §1 and the header of
interaction.md both still print `0x6A`. Probe `0x6B` first. The power-path topology is **settled**: Reading B, verified against a 600 dpi render of the
schematic (hardware.md §3) and corroborated on the bench — the divider reached 3.997 V of a
settled 4.001 V at ~45 ms, which is the RC that Reading B's C10/C12 predict and Reading A does
not. What remains open is listed in hardware.md *Open items* and power-and-time.md §10, ten
items and counting. The touch controller's 0x01 / 0x02 swipe codes are among them and are likely
to stay there: the firmware never reads `GestureID` for direction, it tracks drags from raw
coordinates, so nothing depends on the answer.
