# Documentation

Design and reference material for `h0urg1ass` — a gesture-driven multi-use timer on the
Waveshare RP2350-Touch-LCD-1.69, rendered entirely in one bit.

No firmware exists yet. These documents are the groundwork: what the hardware is, what the
graphics library can do, how the thing should look, how it should be controlled, and how it
should behave on a battery.

## Reading order

Read in this order on a cold start. Each one assumes the ones above it.

| # | Document | What it answers |
|---|---|---|
| 1 | **[hardware.md](hardware.md)** | What the board is, the complete pin map, the traps ranked by how much time they cost, the rounded-corner safe area, the timing budget, and a verifiable bring-up order. |
| 2 | **[display-library.md](display-library.md)** | What the graphics library gives us and what it does not. Framebuffers, primitives, large text, patterns, dirty rects, colour, the HAL, host-side testing, and a gaps list. |
| 3 | **[visual-language.md](visual-language.md)** | How this should look. One-bit technique extracted from Lucas Pope's practice, what does and does not transfer from the Playdate, and a rule set mapped onto real library symbols. |
| 4 | **[interaction.md](interaction.md)** | Motion as the primary control surface. What the IMU does in hardware versus software, honest per-gesture ratings, the recommended minimal vocabulary, the touch model, and touch target sizing. |
| 5 | **[power-and-time.md](power-and-time.md)** | Battery, sleep, and keeping time with no RTC backup cell fitted. Includes the alarm protocol that avoids sleeping through your own alarm. |

## The five constraints everything else follows from

1. **240 × 280, one bit.** No greys. Tone comes from pattern, separation from edges.
2. **~44 px rounded corners physically clip the panel.** Nothing readable or touchable goes
   near a corner. See the safe-area numbers in [hardware.md](hardware.md).
3. **No RTC backup cell is fitted.** Wall-clock time does not survive a power cycle. Timers
   must be correct without ever knowing the time of day.
4. **Firmware must assert GPIO15 early** or the board switches off the moment the power
   button is released on battery. This is invisible over USB.
5. **The screen is a tiny target.** Motion gestures carry the running controls; touch is for
   setting values, with large targets and swipes only.

## Project rule on the graphics library

The UI stays inside the capabilities of the graphics library vendored at
`third_party/1bit-display`. Where something genuinely needed is missing, the fix is to
extend that library upstream as explicit, separate work — not to reach around it with
one-off drawing code here. [display-library.md](display-library.md) §12 tracks the known
gaps and, for each, whether the right home is upstream or local.

## A note on confidence

This board has **not yet been brought up**. Much of the hardware detail is sourced from the
schematic, the vendor demos and the relevant datasheets rather than from a bench. Claims
that have not been verified on hardware say so inline, and where two sources disagree both
readings are given rather than one being quietly picked. Two disputes are open and tracked
in the issue tracker: the power-path topology, and the touch controller's swipe-direction
gesture codes.
