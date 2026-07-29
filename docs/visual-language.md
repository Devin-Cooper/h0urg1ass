# Visual language

A design brief for a 1-bit UI on the **Waveshare RP2350-Touch-LCD-1.69**, derived from Lucas
Pope's 1-bit practice (Playdate / *Mars After Midnight*, *Return of the Obra Dinn*) and grounded
in what the `onebit` graphics library — vendored at `third_party/1bit-display` — actually
implements today.

Read this before writing any drawing code. Section 1 is the technique list. Section 2 says where
the Playdate advice does *not* transfer to this panel. Section 3 is the rule set to code against.
Section 4 maps every rule onto a real library symbol or flags it as new upstream work.

---

## 0. Sources

| Source | What it gives | Standing |
|---|---|---|
| Pope, ["Working in One Bit"](https://dukope.itch.io/mars-after-midnight/devlog/285964/working-in-one-bit), 21 Aug 2021 | The core 2D 1-bit doctrine: dither sparingly, design for the limit, pixel-persistence under motion | The devlog is reproduced here as summary, not verbatim capture. Quotations are short and second-hand; check exact wording against the original before requoting at length |
| Pope, ["Typography"](https://dukope.itch.io/mars-after-midnight/devlog/440614/typography), 18 Oct 2022 | He abandoned bitmap fonts for a **line-based vector font** authored as SVG in Illustrator, compiled by a Python script into Lua data; variable line width for bolding; strings scale, rotate and animate | Primary, directly available |
| Pope, ["Photoshop Exporting & Scenegraphing"](https://dukope.itch.io/mars-after-midnight/devlog/449905/photoshop-exporting-scenegraphing), 10 Nov 2022 | PSD layer tree *is* the scene graph; `psd-tools` + PIL export to `_def.json` + per-layer PNGs; "1 scene = 1 PSD" | Primary, directly available |
| [PlayStation Blog interview](https://blog.playstation.com/archive/2019/10/17/lucas-pope-on-return-of-the-obra-dinns-art-style/), Oct 2019 | Obra Dinn: Bayer vs blue noise trade-off, **"the most important part of using dithering was to use it as little as possible"**, outlines aid spatial reading, resolution raised 640×360 → 800×450, a new dither technique made output "less flickery" | Primary, directly available |
| Pope's TIGSource dithering post, via secondary summaries: [Set Side B](https://setsideb.com/a-forum-post-about-the-dithering-in-return-of-the-obra-dinn/), [Alan Zucconi](https://www.alanzucconi.com/2018/10/24/shader-showcase-saturday-11/), [Daniel Ilett](https://danielilett.com/2020-02-26-tut3-9-obra-dithering/) | **8×8 Bayer matrix** for smooth ranges, **128×128 blue-noise field** for unordered output; Bayer mapped onto a **sphere centred on the camera** so the dither is pinned to geometry under camera *rotation* (not translation); blue noise everywhere else | **Secondary.** The primary forum post and `dukope.com/devlogs` both serve HTTP 403 to automated retrieval. The three summaries agree with each other, but none is the original |
| [Board wiki](https://www.waveshare.com/wiki/RP2350-Touch-LCD-1.69) and [schematic PDF](https://files.waveshare.com/wiki/RP2350-Touch-LCD-1.69/RP2350-Touch-LCD-1.69.pdf) | Pin map, backlight drive, bus topology, absence of a TE pin | Vendor primary. Schematic is authoritative where the wiki prose disagrees with it |
| [ST7789V2 datasheet](https://files.waveshare.com/wiki/common/ST7789V2.pdf) | COLMOD wire formats, FRCTRL2 refresh table, IDMON/IDMOFF, TSCYCW timing ceiling, partial-mode commands | Controller primary |
| [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf) | 520 kB SRAM, PWM slice/channel mapping, SPI function table | MCU primary |
| [1.69" Touch LCD Module wiki](https://www.waveshare.com/wiki/1.69inch_Touch_LCD_Module) and [2D drawing V2](https://files.waveshare.com/wiki/1.69inch-Touch-LCD-Module/1.69inch_Touch_LCD_Module_2D_Drawing_V2.pdf) | Same panel on a bare module: viewing area 28.27 × 32.93 mm, **R5.15 mm corner radius**, cover glass 33.13 × 41.13 mm with 4×R7.0 mm | Vendor primary for the mechanical drawing; the panel identity between module and board is inferred from identical active area, pitch and outline |
| [CST816S register declaration](https://files.waveshare.com/wiki/common/CST816S_register_declaration.pdf) and [CST816S datasheet](https://files.waveshare.com/wiki/common/CST816S_Datasheet_EN.pdf) | `FingerNum @0x02` = "0: no finger 1: one finger" — the single-contact limit behind R21 | Vendor primary. The board wiki labels the part CST816**T** while linking CST816**S** documents; all variants share address, register map and chip ID |
| Vendor firmware: [basic/GUI demo](https://files.waveshare.com/wiki/RP2350-Touch-LCD-1.69/RP2350-Touch-LCD-1.69-Code.zip), [LVGL demo](https://files.waveshare.com/wiki/RP2350-Touch-LCD-1.69/RP2350-Touch-LCD-1.69-LVGL.zip) | Init sequence, the +20 GRAM offset, unconditional `INVON`, backlight PWM setup, SPI clock choices | Vendor primary, but **out of spec in places** — see §2 and §4.2 |
| `third_party/1bit-display`, headers under `include/1bit/` | The API surface named throughout §4 | Primary. **The headers are authoritative, not the library's prose documentation** — the two disagree in at least one place, flagged in §4.1 |

The board itself has **not been hardware-validated with this library**. Every timing figure below
(17.2 ms full frame, 1.47 ms text line, ~53 Hz refresh) is arithmetic from sourced clock numbers,
not measurement. Figures that need an eyeball or a scope are flagged where they appear and
collected in §8.

---

## 1. Pope's techniques, extracted

### 1.1 Dithering — what he uses, and where

**Two patterns, two jobs.** Obra Dinn ships an **8×8 ordered Bayer matrix** and a **128×128
blue-noise field**. Bayer reproduces a tonal *range* more accurately and reads as deliberately
mechanical; blue noise "feels more organic" and — critically — **survives scaling and video
compression far better**, because it has no low-frequency energy and no repeating grid to beat
against a resampling kernel. In-game, environments lean blue noise; foreground subjects and the
Memento Mortem UI lean Bayer.

**Error diffusion is absent from his realtime work, and that is not an accident.**
Floyd–Steinberg and friends are *sequential* — each pixel's output depends on the accumulated
error of its predecessors — so they cannot be evaluated as a per-pixel predicate, cannot be
evaluated in parallel, and above all are **temporally chaotic**: a one-level change in one source
pixel re-rolls the whole downstream row. Ordered dithering is a stateless threshold comparison
`src[x,y] > T[x mod w, y mod h]`, which makes it the only family that can be *anchored* (see 1.2).
Reserve error diffusion for one-shot, static, photographic content.

**Why it works on a 1-bit panel at all:** dithering trades spatial resolution for apparent tonal
resolution. On a panel whose pixel pitch is below the eye's resolving limit at working distance, a
4×4 tile at 50% coverage integrates to mid-grey. That is also the trap.

**Pope's actual position on dithering is restrictive.** Summarising the *Working in One Bit*
devlog: dithering does successfully re-create grayscale tones, but stylistically he doesn't think
the best use of 1-bit is to simulate 8-bit grayscale — *"I'd rather make something that's enhanced
by the limitations rather than pressed up against them."* Combined with the Obra Dinn line — use
dithering **as little as possible** — the operational rule is:

> Dither is a *material*, not a *renderer*. It is applied to specific chosen regions as a texture,
> not swept across the frame to fake a value ramp.

His Photoshop method matches that: he imports a **full 400×240 image of the repeating pattern as a
layer and masks it**, rather than applying a dither filter to a grayscale render. The pattern is a
paint bucket, not a post-process.

**Packed-1bpp implementation.** A pattern is a pure predicate `bool f(x,y)`. For any tile whose
width divides 8 (4, 8, 16 all do), the predicate can be hoisted to *byte* granularity: precompute,
for each `y mod tile_h` and each `byte_x mod (tile_w/8)`, the 8-bit mask of set pixels. A dithered
horizontal span then becomes a byte loop with three cases (head partial byte, aligned run, tail
partial byte) exactly like a solid fill, instead of `w` calls to `setPixel`. This is the single
largest speedup available in the library today — see §4.2, item N2.

### 1.2 Dither under motion: crawling, shimmer, and how he avoids it

**The failure.** A screen-anchored dither pattern under moving content re-samples the pattern at a
new phase every frame. Pixels that belong to one flat region flicker on and off in a travelling
wave. Pope's phrasing for the Playdate case: the Sharp memory LCD *"suffers from strobing when
flipping pixels on/off. This isn't normally perceptible **except** when scrolling a dithered
image."* On Obra Dinn, the same phenomenon under camera motion is what he spent ~100 hours
removing, and what he described as making the output "less flickery."

Two distinct causes are conflated in the literature and are worth separating:

1. **Perceptual crawl** — the pattern phase changes relative to the content. Universal. Applies to
   every display technology.
2. **Panel strobing** — the Sharp memory LCD's pixel-flip transient. Playdate-specific hardware.
   **Does not apply to the ST7789V2 IPS panel used here.**

**The two fixes he names.**

- *Obra Dinn (3D, realtime):* **anchor the pattern to the content, not the screen.** He projects
  the Bayer matrix onto a **sphere centred on the camera**, so under camera rotation the dither
  stays pinned to the geometry it is shading. It still slips under camera translation — he judged
  that an acceptable compromise. The generalisable principle: make the pattern's coordinate space a
  function of the *object*, not of the *viewport*.
- *Mars After Midnight (2D, hand-drawn):* an image that must move should either have its dither
  applied *after* the move (hard), or — his preference — be *"more carefully designed to persist
  its pixels as much as possible in the direction of movement"* (less hard). In practice: shading
  that runs *along* the motion axis (horizontal hatching for horizontal scroll) keeps most pixels
  on as the content slides; shading that runs *across* it toggles every pixel every frame.

**Third fix, implicit:** don't dither the thing that moves. Solid silhouettes have no phase to slip.

**Packed-1bpp implementation of anchoring.** Keep an `(offset_x, offset_y)` on every pattern and
subtract the drawn shape's screen position from it before filling, so the predicate is evaluated in
the shape's local space:

```
p.xform.offset_x = -shape.x;   // pattern travels with the shape
p.xform.offset_y = -shape.y;
```

For a full-screen parallax scroll, give each layer its own offset equal to its own scroll amount.
If the *texture* should instead be a fixed property of the world (a wall), lock the offset to world
coordinates and let it slide under the viewport — that is correct and stable too. What is never
correct is leaving the offset at zero while the content moves.

### 1.3 Contrast, edges, and separating figure from ground without grey

**The governing fact:** in 1-bit there is no such thing as an implicit edge. Two regions of
different *density* touching each other do not produce a boundary — they produce a smudge. Every
boundary that is wanted must be drawn.

**Outlines.** From the Obra Dinn work: *"Clear outlines on the geometry like this make
understanding the 3D space much easier."* Obra Dinn generates them with a compositor edge-detection
node network fed by per-object vertex-colour IDs; Pope notes he had "infinite bits" of per-object
parameters via Blender AOVs, versus the single reserved bit in his own renderer. **None of that
machinery exists in 2D** — in *Mars After Midnight* he says most of his existing skillset and
pipeline was "almost useless," and legibility came down to placing *"each and every pixel."*

**The invisible-overlap problem.** Black ink drawn on top of a dense pattern has no visible
separation. The fix is a **knockout halo**: clear a 1–2 px paper-coloured band around the
foreground before drawing it. This is the 1-bit equivalent of a drop shadow, and it is mandatory,
not decorative.

**Flat areas.** Large solid black and large solid white are the two strongest signals available —
Pope's "use dithering as little as possible" is exactly a reservation of these. Reserve `solid` for
the most important element on screen. Everything competing with it dilutes it.

**Legibility scale.** He repeatedly returns to physical size: the Playdate screen is *"2 inches
across, so legibility is already a challenge"*, and unlike 3D where *"moving the camera around is a
critical part of understanding the scene,"* in a static 2D frame *"you need to be able to quickly
and easily interpret only what you're seeing on screen right now."* There is no second look.
Everything must resolve in one glance.

### 1.4 Motion and animation

**What reads well in 1-bit:** silhouette change, hard position change, discrete state flips,
directional wipes, and anything mechanical (flip-dot cascades, split-flap rolls, dot-matrix
scroll). These are *native* to a binary medium — the medium's grain is the animation's grain.

**What falls apart:**

- **Fades and dissolves.** There is no apparent-brightness axis. A density ramp is not a fade; it
  is a boiling texture.
- **Sub-pixel motion.** With no anti-aliasing there is nothing to carry the fractional position;
  motion quantises to hard 1-px jumps and reads as judder.
- **Rotation of dithered fills.** The pattern rotates with the shape or it doesn't — either way
  something shears.
- **Fine detail in motion.** Anything at 1–2 px scale strobes.

**Frame-rate feel.** Pope states no figure. The operative constraint on any 1-bit device is that *a
wrong pixel is 100% wrong* — there is no partial credit — so a lower, perfectly stable frame rate
beats a higher one with any per-frame pixel churn. Prefer 30 fps with zero shimmer over 60 fps with
crawl.

### 1.5 Typography

**Pope's answer for Mars was a custom line-based vector font, not a bitmap font.** He abandoned
bitmap glyphs, drew a single font as line paths (no filled shapes) on a grid in Illustrator,
exported as SVG with each glyph in a named group, and had a Python build script read the SVG
directly into Lua data. One font for the whole game — enforced typographic restraint. Line *width*
is the bolding axis. Strings scale, rotate, wiggle, and deliberately break out of *"rigid frame of
cardinal alignment."* His first attempt looked like a worse bitmap font; the fix was to **"lean
into the vector-ness"** and design something distinctly linear rather than fight the technique.

**Anti-aliasing into dither: don't.** He does not discuss it, and the reason is structural. A
dithered glyph edge at UI sizes is a chewed stem: the dither cell is a meaningful fraction of the
stroke width, so the pattern's phase decides whether a stem is 1 px or 2 px wide. Different letters
in the same word end up different weights. Bitmap fonts are hinted by construction; a stroke-vector
font rasterised with hard-edged thick lines is hinted by having only one edge state. Both are
correct. Anti-aliasing is not.

**Sizing.** No pixel numbers appear in his devlogs. The transferable rule is his physical-size
framing: judge type by millimetres on the glass, not by pixels in the buffer.

### 1.6 Embracing the limit

The through-line, stated most directly in *Working in One Bit*: he'd *"rather make something that's
enhanced by the limitations rather than pressed up against them"*, and dithering-to-simulate-
grayscale is the canonical way of pressing up against them. Corollaries he states or demonstrates:

- Accept imperfect drawing; the art *"should make it clear that no one with excessive drawing
  skills was involved."* Style beats fidelity.
- The constraint is a feature: *"There's very few platforms that are more constrained than 400×240
  1-bit, so even the 'power through it' stuff isn't that bad."*
- Design decisions are made at the pixel, in the target resolution, with the target tool — he moved
  between Procreate (sketch), Pixaki (pixel-precise retrace) and Photoshop (pattern layers,
  perspective, composition) specifically because low-res work needs pixel-level control, and
  rejected nostalgic period tools for lacking unlimited undo.

---

## 2. The target is not a Playdate

| | Playdate | RP2350-Touch-LCD-1.69 | Consequence |
|---|---|---|---|
| Panel tech | Sharp **reflective** memory LCD, no backlight | **Transmissive IPS TFT, PWM LED backlight** (GPIO25, 0–100%, ~29.7 kHz at the stock 150 MHz clock) | Contrast is high, constant, and ambient-independent. **White is emissive, not paper.** |
| Pixel flip artefact | Strobes on flip; visible when scrolling dither | ST7789V2 — no such transient | Pope's *hardware* justification for pixel-persistence **does not apply**. The *perceptual* crawl rule still does. |
| Resolution | 400×240 = 96,000 px | **240×280 = 67,200 px** (portrait) | 70% of the pixels, and **portrait**. Nothing about a 400×240 landscape composition transfers. |
| Pixel pitch | ~0.147 mm (~173 ppi) | **0.11655 mm (~218 ppi)** | Pixels here are **0.79× the linear size**. Everything drawn is ~21% smaller in the hand. |
| Physical area | ~58.8 × 35.3 mm (2076 mm²) | **27.972 × 32.634 mm (913 mm²)** | **44% of the Playdate's glass.** Density budgets do not transfer; there is less than half the room. |
| Shape | Rectangle | **Rounded corners, ~44 px radius** (R5.15 mm ÷ 0.11655 mm/px) | Four quarter-circles of the framebuffer are physically invisible. |
| Colour | None | **`AttributeMap` per-8×N-cell ink/paper over RGB565** | A semantic colour channel Pope does not have. |
| Refresh / sync | 50 Hz, hardware-synced | **~53 Hz panel; no TE line wired** | Flushes cannot be synced to scan-out. A full-frame push (17.2 ms at 62.5 MHz) races an 18.9 ms scan → visible tearing. |
| Wire cost | Native 1-bit | **No wire format below 12 bpp**; RGB565 = 16× expansion, 134,400 B/frame | Bus time, not CPU, is the frame budget. Partial updates are the design, not the optimisation. |
| Polarity | — | `INVON` mandatory; omit it and black/white silently swap | A polarity bug and an ink/paper swap look identical on a bench. |

The refresh figure comes from the vendor init writing FRCTRL2 (0xC6) = 0x13, which the ST7789V2
datasheet's table maps to 53 Hz in normal mode; with PORCTRL (0xB2) = 0x0B,0x0B,0x00,0x33,0x35 the
datasheet formula `10 MHz / ((320+FPA+BPA)×(250+RTNA×16))` gives 10e6/(342×554) ≈ 52.8 Hz. The
COLMOD table in the same datasheet accepts only `011` = RGB444 (12 bpp), `101` = RGB565 (16 bpp),
`110` = RGB666 (18 bpp) and `111` = 16M truncated — **there is no 1, 2, 4 or 8 bpp wire format.**
The nearest approach to a mono mode is IDMON (0x39), which honours only the MSB of each channel for
an 8-colour output; it saves panel power and pins black and white to exact endpoints but does not
reduce SPI traffic, since COLMOD is unchanged.

### What each difference changes about the recipe

**Backlit and transmissive is the biggest one.** On a reflective panel, white is the substrate and
black is dim grey; contrast is modest and the whole image reads as printed matter, which is why
dither reads as *texture* there. On a backlit IPS at full duty, white pixels are point light
sources with a hard on/off edge and a very high contrast ratio. Two consequences:

1. **Large white fields glare.** A full-screen `solid(false)` on a white-paper polarity is a torch
   in a dark room. **Default to light-on-dark**: set `PixelFormat::ink` = white, `paper` = black, so
   the 1-bit "ink" is the *lit* thing. The 1-bit design is unchanged — polarity lives in exactly one
   place in the HAL — but the entire perceptual character flips from "printed page" to "instrument
   panel," and the latter is what this hardware wants. **Unverified on hardware** (§8).
2. **Dither shimmer is worse, not better.** Higher contrast between adjacent pixels plus a finer
   pitch (218 ppi) puts a 50%-coverage 1-px pattern right at the eye's scintillation threshold.
   Pope's "use it as little as possible" tightens rather than relaxes here.

**Higher ppi cuts both ways.** Fine patterns blend better — a `bayer(d, 4)` tile is 0.47 mm,
genuinely sub-perceptual as a grid. But **text gets smaller in the hand**: `TERM_5X7` is 0.82 mm
tall on this glass versus 1.03 mm for the same glyph on a Playdate. Bump every type size up one
step relative to Playdate intuition.

**Portrait 240×280 with rounded corners** kills any horizontally-scrolling-panorama composition.
The natural idiom here is a **stacked vertical card / instrument face / status column**, not a
side-scrolling scene. And a horizontal scroll — which is what Pope's pixel-persistence rule was
written for — is now the *unnatural* axis; vertical scroll is the likely one, so the persistence
rule flips to **horizontal hatching for vertical motion**.

**No TE line** means the frame-rate ceiling is a tearing problem, not a throughput problem. No
TE/LCD_TE/TEAR net appears in the schematic, the FPC pin list has no such pin, and no vendor driver
sends TEON (0x35) — the ST7789V2 silicon supports TE, but the pin is not brought out on this board.
Partial updates via the dirty tracker are therefore not a perf nicety; they are how tearing is
avoided.

**Colour changes the argument for dither entirely.** `fillPatternRect` writes only `BLACK`, so a
Bayer fill inside a cell whose ink is (say) amber mixes ink and paper and yields an *intermediate
tone in colour*. That is a legitimate value axis — and it is the one form of "faking grey" that
Pope's objection does not cover, because it is not simulating grayscale, it is mixing two chosen
hues. Use it deliberately and sparingly; the library's own measurements on an RP2350 at 250 MHz put
the cost at **+28.8% on drawing with a pen active and +11.2% on a full-frame push**.

---

## 3. The rule set

Code against these.

### 3.1 Tonal system

**R1 — Three tones, then texture.** Reserve `solid(true)` and `solid(false)` for the two most
important things on screen. Everything else is texture. If three regions are all patterned, none of
them is foreground.

**R2 — Dither budget: ≤ 25% of frame area.** Measure it. If more than a quarter of the visible
pixels are carrying a pattern, the design is simulating grayscale.

**R3 — Fix a pattern vocabulary of four and never exceed it.** Suggested set, with the physical
tile size on this glass:

| Role | Pattern | Tile on glass | Why |
|---|---|---|---|
| Mechanical fill / UI chrome | `bayer(d, 4)` | 0.47 mm | Sub-perceptual grid, predictable density, cheapest hoistable-to-byte tile |
| Large organic area | `blueNoise(d)` | 14.9 mm (128 px) | No visible repeat across a 240 px screen; no grid to moiré with the panel |
| Printed / physical texture | `clusteredDot(d)` or `halftone(d, 6)` | 0.93 / 0.70 mm | Reads as newsprint; strong identity |
| Hand-made / disabled state | `hatch(45, 4, 1)` | 0.47 mm | Directional, obviously drawn |

Banned by default: `noise(seed, density)` (white noise is the perceptually worst dither and the
reason blue noise exists); `crosshatch()` full-screen (2× hatch cost — the library's render
documentation explicitly says not to use it full-screen at 60 fps); `xorTexture()` and `checker()`
for anything but deliberate test or glitch content.

**R4 — Never distinguish two regions by density alone.** Adjacent regions must differ by (a) a
drawn 1–2 px outline, **or** (b) a 2 px paper gutter, **or** (c) a different pattern *kind* plus
≥ 64 density steps. Two shades of Bayer touching is a smudge.

### 3.2 Motion

**R5 — Patterns are anchored to content.** When a filled shape moves by `(dx, dy)`, its pattern
moves with it. Set `p.xform.offset_x = -shape.x; p.xform.offset_y = -shape.y` (or accumulate the
delta). Zero offset on moving content is a bug, and it is the single most common way a 1-bit UI
ends up looking cheap.

**R6 — Moving things are solid.** If an element animates continuously, fill it `solid`. If it must
be patterned and must move, either quantise its motion to whole tile steps (4 px for `bayer(d, 4)`,
8 px for `clusteredDot` / `bayer(d, 8)`) so the phase is preserved exactly, or render it solid
during motion and re-dither on settle.

**R7 — Hatch along the motion axis.** For the likely vertical scroll on this portrait panel, use
`hatch(0, …)` (horizontal lines) or `stripes(on, off, /*vertical=*/false)`. Pixels then persist as
content slides. Never hatch across the scroll direction.

**R8 — No fades. Substitute one of:** a `TransitionKind::Wipe` (reads beautifully in 1-bit — hard
edge, unambiguous direction); a **backlight ramp** via `setBacklight` + `easeInOut` (a true optical
fade, costing zero pixels and producing zero shimmer — the Playdate cannot do this and it is the
best trick this hardware gives); or a `TransitionKind::DensityRamp` capped at ≤ 300 ms with a
coarse pattern, used as a deliberate *dissolve* effect rather than as a fade.

**R9 — Integer pixels only.** Round positions once, at the draw call. Never accumulate float
positions frame over frame; keep a float accumulator and floor it, or the jitter is unbounded.

**R10 — Frame budget by wire time, not CPU.** At the safe 62.5 MHz SCK ceiling: full frame 17.2 ms;
a 240×24 text line 1.47 ms; a 240×16 strip 0.98 ms; an 8×12 glyph cell ~25 µs. Target **30 fps**
for full-screen motion and use `DirtyRectTracker` so the typical frame pushes 1–3 small rects.
Because there is no TE line, a full-frame push against a 53 Hz refresh will tear; keep full pushes
for scene changes.

### 3.3 Contrast and readability

**R11 — Knockout, never overlay.** Before drawing ink over any patterned region, clear a 1–2 px
halo. `drawDropShadow(fb, fg, dx, dy, pattern, halo_px)` does this for rects;
`renderStringWithHalo(...)` does it for vector text. For arbitrary sprites over a pattern, see
§4.2 N4.

**R12 — Outline the primary container.** 2 px for the top-level frame or modal, 1 px for secondary
groups. At 218 ppi a 1 px line is 0.117 mm and disappears at arm's length; do not use 1 px for
anything structural.

**R13 — Silhouette first.** Any icon or illustration must be identifiable as a solid black shape
before any interior detail is added. If it isn't, no amount of interior texture will save it.

### 3.4 Typography

**R14 — Body text is `TERM_6X9` or `TERM_8X12`. Not `TERM_5X7`.** On this glass `TERM_8X12` gives a
1.40 mm cap height, `TERM_6X9` gives 1.05 mm, `TERM_5X7` gives 0.82 mm. Reserve 5×7 for dense
tabular data and unit labels only.

**R15 — Headlines use the vector font**, matching Pope's Mars approach exactly:
`renderString(fb, txt, x, y, charWidth, charHeight, spacing, strokeWidth, color)` at `charHeight`
24–40, `strokeWidth` 2–3. `strokeWidth = 1` breaks up at this pitch — never use it above
`charHeight` 20.

**R16 — One font family per screen.** Pope shipped one font for the whole game. Mixing
`TRANSIT_7X12` and `FLAP_13X26` and the vector font on one screen reads as a font catalog, not a
product. Pick a signage font as the *identity* and a terminal font as the *body*, and keep that
pairing global.

**R17 — Never dither type, never anti-alias type.** No exceptions. Both bitmap and stroke-vector
rasterisation are hard-edged by construction; keep them that way.

**R18 — Type never sits on a pattern bare.** Either knock out a halo (R11) or clear a solid plate
under it.

### 3.5 Layout on this specific panel

**R19 — Safe box: inset 16 px.** The 44 px corner radius clips the diagonal; the minimum diagonal
clearance is `44·(1 − 1/√2) ≈ 12.9 px`. Use **16 px** for headroom. **Content rect =
`Rect{16, 16, 208, 248}`.** With `TERM_8X12` (cell 8×13) that is a **26 × 19 character grid**.
Nothing readable or tappable goes outside it. The 44 px figure is arithmetic from the module
drawing's R5.15 mm divided by the 0.11655 mm pitch, not a measurement of this board (§8).

**R20 — Backgrounds bleed to the full 240×280.** Patterns and fills should run edge to edge so the
physical corners read as a deliberate bezel, not a clipping accident. Only *content* respects the
safe box. The sibling module wiki warns plainly that "due to the four round corners, some parts of
the input images may not be displayed."

**R21 — Touch targets ≥ 40 px.** 40 px = 4.7 mm. The CST816 register map defines `FingerNum @0x02`
as "0: no finger 1: one finger" — there is **no encoding for a second contact**. Design for a
single finger: no pinch, no two-finger gestures. (The CST816S datasheet prose mentions "single-point
and real two-point gestures," but that refers to gesture *sensing*, not to two reported
coordinates.)

**R22 — Default polarity is light-on-dark.** `PixelFormat` ink = white, paper = black. **Unverified
on hardware** — this is the one rule here that genuinely needs an eyeball before the UI is built
around it.

**R23 — Backlight is a design channel.** 100% for active interaction, 20–30% for idle or night,
ramped with `AnimationTimer` + `easeInOut` over 200–400 ms. Treat it as the global value axis the
framebuffer does not have. The hardware is a single white LED string driven active-high from GPIO25
through a DMG1012T-7 N-channel MOSFET; the vendor setup uses PWM slice 4 channel B with
`pwm_set_wrap(slice, 100)` and `pwm_set_clkdiv(slice, 50)`, making duty a plain 0–100 integer
percent.

### 3.6 Colour (AttributeMap)

**R24 — The 1-bit framebuffer stays the design.** The library's enforced invariant: identical
framebuffer bytes with or without colour. Never let a layout depend on colour to be readable.

**R25 — One accent per screen.** Colour is semantic — state, alert, one branded chrome element. Not
illustration. Anything else produces a bad colour UI instead of a good 1-bit one.

**R26 — Align `cellHeight` to the text cell.** Construct `AttributeMap(240, 280, /*cellHeight=*/13)`
to match `TERM_8X12`'s 13 px cell. Note that `TerminalRenderer::cellHeight()` returns
`glyph_height + 1` and is *independent* of `AttributeMap::cellHeight()` — set both. Cost:
30 × 22 = **660 bytes**. `cellHeight = 8` costs 1050 B; `cellHeight = 1` costs 8400 B and removes
vertical clash entirely if that is needed.

**R27 — Region-paint, don't pen, on the hot path.** `attrs.fillCells(rect, attr)` after drawing
costs nothing extra; `fb.setPen()` costs +28.8% on the drawing path.

**R28 — After a palette change, call `tracker.markAllDirty()`.** A palette change alters no
attribute bytes, so the dirty tracker correctly reports clean and the panel keeps the old colours.
This will look like a driver bug and isn't.

**R29 — `AttributeMap::consumeDirty()` clears on read.** Single consumer only. Two callers means a
silently dropped repaint.

---

## 4. Mapping onto `1bit-display`

§4.1 is existing API: every symbol below is declared in a header under
`third_party/1bit-display/include/1bit/` today. §4.2 is **new upstream work in the graphics
library**, not application code — none of it exists yet, and none of it can be written from the
application side.

### 4.1 Already exists — use these exact symbols

| Rule / technique | Library feature |
|---|---|
| Ordered dither, Bayer 4/8/16 (R3) | `bayer(density, tile_size = 8)` — `tile_size ∈ {4, 8, 16}` |
| Blue noise, matching Obra Dinn's 128×128 field (R3) | `blueNoise(density)` — void-and-cluster **128×128**, seamless at mid-density. Regenerable via `tools/gen_blue_noise.py` |
| Newsprint / halftone texture (R3) | `clusteredDot(density)` (spiral-from-centre 8×8), `halftone(density, cell_size = 6)`, `benDay(density, spacing = 4, radius = 1)` |
| Directional hatching along the motion axis (R7) | `hatch(angle_deg, spacing, thickness = 1)`, `lineScreen(density, angle_deg, spacing)`, `stripes(on, off, vertical = false)` |
| **Content-anchored patterns (R5)** — the 2D analogue of Obra Dinn's sphere | `PatternTransform{offset_x, offset_y, scale_shift, rotate_deg}` on every `Pattern`; the transform is a coordinate mutation applied *before* the predicate. `scale_shift` pixel-doubles (power of two); `rotate_deg` is 90° steps only in v1 |
| Applying a pattern to a shape (Pope's "masked pattern layer") | `fillPattern`, `fillPatternRect`, `fillPatternCircle`, `fillPatternPolygon` — all write **only `BLACK`**, so they compose as ink over existing paper |
| Wipes as the fade substitute (R8) | `Transition{kind, a, b, t, threshold_source, wipe_angle_deg, wipe_softness}` + `fillTransition` / `fillTransitionRect` / `fillTransitionCircle`. `TransitionKind::Wipe` gives a pattern-dithered soft edge; `Crossfade`, `Morph`, `DensityRamp` also present. `threshold_source` is a `PatternKind`, defaulting to `BlueNoise`. Stateless — the caller owns the timer |
| One-shot photographic dither (§1.1 caveat) | `ditherImage` / `ditherImageWithOptions` with `ImageDitherAlgorithm::{Threshold, Ordered, FloydSteinberg, Atkinson, SierraLite}`; `ImageDitherOptions{brightness, contrast, invert}`. **`Atkinson` is the Mac-1984 look (6/8 diffusion, 25% of error discarded)** and is the most 1-bit-native of the error-diffusion set |
| Dither with a *chosen* pattern as threshold source | `ditherImageOrdered(dst, x, y, src_gray, w, h, const Pattern& threshold_pattern)` — pass `blueNoise(0)` for Obra-Dinn-style organic thresholding |
| Knockout halo for rects (R11) | `drawDropShadow(fb, fg, dx, dy, pattern, halo_px)` — its doc comment names the invisible-overlap problem explicitly and clears a `halo_px` white band |
| Knockout halo for vector text (R11, R18) | `renderStringWithHalo(fb, text, x, y, charWidth, charHeight, spacing, strokeWidth, textColor, haloColor)` |
| Pattern-filled shadow following glyph shapes | `drawVectorTextShadow(...)` — renders glyphs into a temporary `DynamicMaskBuffer` with stroke padding, ORs at the offset using the pattern as fill source (≤ 4 KB typical, ~12 KB worst case at headline scale) |
| Stroke-vector headline font (R15) — Pope's exact Mars technique | `vector_font.hpp`: `renderChar`, `renderString`, `renderStringRight`, `getStringWidth`, `getCharWidthMultiplier`. Glyphs are packed polylines in a 0–100 coordinate space; **`strokeWidth` is the bolding axis, matching Pope's variable line width** |
| Bitmap body text (R14) | `drawBitmapText`, `getBitmapTextWidth` with `TERM_5X7`, `TERM_6X9`, `TERM_8X12` (full printable ASCII + box drawing), `VMS_5X7`, `TRANSIT_7X12`, `FLAP_13X26` |
| Custom font pipeline | `tools/generate_font.py` (BDF → C++ header, `--warn-blank` on by default) |
| Outlines (R12) | `drawRect`, `drawLine`, `drawThickLine`, `drawPolygon` |
| Masking / stencilling (Pope's masked pattern layers) | `MaskBuffer<W,H>` / `DynamicMaskBuffer` + `IFramebuffer::setMask()`. Semantics: **`BLACK` in the mask = drawing allowed**. Note that `setPixelDirect()` **bypasses the mask** — use `setPixel()` when masking matters |
| Clipping | ⚠ **Not at the pinned revision** — see the note below the table |
| Sprite composition, knockouts (R11) | ⚠ **Not at the pinned revision** — see the note below the table |
| Motion timing (R8, R23) | `AnimationTimer(duration_ms, looping)` with `t()` / `isComplete()`; `easeIn`, `easeOut`, `easeInOut`, `easeInOutSine`, `easeOutBounce` |
| Life without a brightness axis (R8) | `breathingOffset(t, amplitude, period)`, `breathingScale`, `breathingScaleWithPhase`, `wigglePoints(...)` (deterministic, seeded), `transitionPoints(...)` for shape morphs. `wigglePoints` is the direct analogue of Pope's "break out of cardinal alignment" |
| Partial update, no-tear strategy (R10) | `DirtyRectTracker::update(fb, attrs)` → `DirtyRectList`; `isClean(fb)`; `markAllDirty()`. `DisplayDriver::writeRegion` is the pure-virtual **primitive**; `push()` is derived from it |
| Per-cell colour (R24–R29) | `AttributeMap(pixelWidth, pixelHeight, cellHeight)`, `Attribute::make(ColorIndex ink, ColorIndex paper)`, `at` / `set` / `atPixel` / `stampPixel` / `stampSpan` / `fillCells` / `clear` / `consumeDirty`; `fb.setAttributeMap()`, `fb.setPen()`, `fb.clearPen()`; `Palette` |
| **Tinting via dither inside a coloured cell** (§2, R25) | Falls out for free: `fillPatternRect` writes only `BLACK`, so a Bayer fill in a cell with a coloured ink mixes ink and paper. Documented in `include/1bit/render/README.md` under *Colour* |
| Panel geometry for this exact board | **`PanelGeometry::st7789_240x280_1in69()`** in `include/1bit/hal/panel_geometry.hpp` — encodes 240×280 in a 240×320 GRAM with the +20 offset that migrates between the row and column axis on rotation |
| Colour reaching the glass | `WindowedDisplayDriver::setPalette(Palette)` with `PixelFormat::rgb565()`; the attribute map is read from the framebuffer being pushed, via `IFramebuffer::setAttributeMap()`. **Two caveats.** (1) `include/1bit/hal/README.md` names a `WindowedDisplayDriver::setAttributeSource(attrs, palette)` that the header does not declare — the header is the authority; code against `setPalette`. (2) `Expander::expandRectWithAttributes` carries colour for **RGB565 only** — configure RGB444 or Mono1 and every attribute is silently ignored, with no warning |
| Backlight as a design channel (R23) | `DisplayDriver::setBacklight(uint8_t)` and `DisplayCaps::backlight` exist as virtuals — the *interface* is there, defaulting to `return false` |
| Native 1-bit idioms (§1.6) | `signage/`: VMS dot-matrix, split-flap with flip animation, flip-dot with cascade wave. These are "enhanced by the limitation" by construction |
| Reviewing the actual pixels | `onebit::encodeBraille` + `tests/golden/` — braille is a lossless 2×4 bit packing, so a baseline **is** the framebuffer re-spelled. `ONEBIT_UPDATE_GOLDENS=1 ctest`, then read the diff. This is the design-review surface |

> ⚠ **Two entries above are not available yet.** Composable clipping
> (`IFramebuffer::setClip` / `pushClip` / `clearClip` / `ClipScope`) and the blit layer
> (`include/1bit/render/blit.hpp` — `BitmapView`, `RasterOp::{Copy, Or, And, Xor, AndNot}`,
> `blit`, `viewOf`) are both **absent from the revision this repository pins**. They exist
> on an unmerged upstream branch. Until that lands and the submodule is bumped:
>
> - For clipping, constrain geometry at the call site, or stencil with
>   `MaskBuffer` / `DynamicMaskBuffer` via `IFramebuffer::setMask()`.
> - For knockouts specifically, `drawDropShadow` and `renderStringWithHalo` already provide
>   the halo behaviour that `RasterOp::AndNot` would otherwise be used for.
>
> Verify availability before designing around either. Everything else in the table is
> present at the pinned revision.

### 4.2 Needs new work in the library

**N1 — There is no driver for this board.** `platform/pico-example/` targets the
**RP2350-Touch-LCD-2.8**, a different panel geometry. A `WindowedDisplayDriver` subclass is needed,
supplying `setWindow` / `writePixels` / `stripBuffer` / `clear`, plus:

- `PanelGeometry::st7789_240x280_1in69()` and `PixelFormat::rgb565()` in the constructor;
- **+20 on RASET in portrait** — the offset exists because the 240×320 GRAM centres a 280-row
  panel, `(320 − 280)/2 = 20`. In landscape the +20 **moves to CASET** instead;
- unconditional `INVON` (0x21) — every vendor driver sends it; omit it and black and white silently
  swap;
- **ping-pong strip buffers** re-acquired per chunk. `stripBuffer` is called once per chunk and the
  buffer must be safe to overwrite the moment it returns, so a single reused buffer with async DMA
  produces sparse static that *does not change when the SPI clock is halved*;
- `caps().backlight = true` and a real `setBacklight` override (PWM slice 4 channel B on GPIO25,
  wrap 100, so duty is a plain 0–100);
- `setLowPower` → `IDMON` / `IDMOFF` (0x39 / 0x38);
- SCK ≤ **62.5 MHz** — the controller's TSCYCW (serial clock cycle, write) minimum of 16 ns. The
  vendor LVGL demo runs SCK at 100 MHz, roughly 1.6× over that spec, and the MicroPython driver at
  75 MHz, roughly 1.2× over. Neither is a licence to exceed it.

Note also that **MISO is not wired**: the FPC carries no data-out line, no vendor driver configures
one, and the display is therefore write-only. There is no controller-ID read, no GRAM read-back and
no init verification — polarity and geometry bugs can only be caught by looking at the glass.

**N2 — Byte-hoisted pattern fills.** Per `include/1bit/render/README.md`, "the `fillSpan`
byte-aligned fast path is only taken for `Solid`. Every other kind writes per-pixel." For `Bayer`
(4/8/16), `Checker`, `Stripes` and `UserTile` with a width dividing 8, the predicate is periodic in
`x` with a period that divides the byte, so an 8-pixel mask can be precomputed per
`(y mod tile_h, byte_x mod period)` and the span filled a byte at a time. Given that patterned fills
are the bulk of a 1-bit UI's drawing cost, this is the highest-leverage optimisation in the codebase
and it is purely additive.

**N3 — Rounded-corner masking (R19 / R20).** Nothing in the library models corners; the only
occurrences of "corner" in the tree are box-drawing glyphs and VMS dot rounding. Two options:

- *Cheap and correct:* a **per-row `[xmin, xmax]` table** — 280 rows × 2 bytes = **560 bytes** —
  consumed by the driver's `writeRegion` to trim each row, or by a clip helper at draw time.
  Preferred.
- *General:* a `roundedCornerMask()` builder returning a `MaskBuffer<240,280>` (8400 B) for
  `fb.setMask()`. Simpler to use, 15× the RAM, and it silently does not apply to `setPixelDirect`.

Ship a `SAFE_RECT` constant alongside it. `TerminalRenderer` in particular needs to inset its text
area.

**N4 — A general knockout / outline operator.** `drawDropShadow` handles rects and
`renderStringWithHalo` handles vector text; there is nothing for an arbitrary sprite over a pattern.
Add `knockout(dst, x, y, const BitmapView& src, int16_t halo_px)`: dilate `src` by `halo_px` (OR the
source into a scratch mask at the 8 neighbour offsets per unit of dilation), `blit(...,
RasterOp::AndNot)` the dilated mask to clear the halo, then `blit(..., RasterOp::Or)` the original.
Same shape as `drawVectorTextShadow`'s existing scratch-mask approach.

**N5 — Spatiotemporal blue noise (STBN).** `blueNoiseAnim(density, frame)` exists but is documented
as "scrolling blue-noise (**not STBN**)" — it is a crawl *generator*, useful for deliberate static
and interference effects, and actively wrong for stabilising animated dither. A per-frame animated
dither that does not shimmer needs a real STBN volume: a stack of blue-noise slices that are blue in
time as well as space. Until that exists, obey R5 / R6 instead. Extend `tools/gen_blue_noise.py`.

**N6 — Vector-font rotation and wiggle wiring.** Pope's Mars strings *"scale, rotate and animate"*
and deliberately leave cardinal alignment. This library's strings scale (`charWidth` / `charHeight`)
and bold (`strokeWidth`) but do not rotate, and `wigglePoints` operates on `Point` / `PointF` arrays
that the vector-font API never exposes. Add a rotation angle to `renderString`, and an entry point
that hands the glyph polylines to a caller-supplied point transform.

**N7 — A framebuffer scroll / blit-shift helper.** The nearest existing thing is
`TerminalBuffer::setScrollRegion(top, bottom)` in
`include/1bit/terminal/terminal_buffer.hpp` (with private `scrollUp` / `scrollDown`), but that
scrolls the terminal's character cells, not framebuffer pixels; nothing in `render/` or `core/`
shifts pixel data. Once the blit layer lands, `blit(fb, x, y, viewOf(fb),
srcRect, RasterOp::Copy)` would cover overlapping copies, but a byte-aligned horizontal shift (a multiple
of 8) should degrade to a `memmove` per row, and a non-aligned shift needs a shift-and-merge byte
loop. Worth having as a pixel-space `scrollRegion(fb, rect, dx, dy, Color fill)` given that R6 / R7
make scrolling a first-class motion.

**N8 — Motion-stability golden tests.** The braille golden system is ideal for this: render N frames
of a scrolling patterned region and assert the pattern phase is content-locked, i.e. frames
identical modulo the shift. That turns R5 from a convention into a test.

**N9 — Backlight ramp helper.** `setBacklight` is a bare virtual returning `false` by default. Add a
small `BacklightRamp` combining `AnimationTimer`, an easing function and `setBacklight`, since R8 and
R23 make it a primary UI mechanism on this hardware.

**N10 — No asset pipeline.** Pope's PSD-as-scene-graph (`psd-tools` → `_def.json` + PNGs, "1 scene =
1 PSD") has no analogue here; `tools/generate_font.py` is the only asset tool. If the UI grows beyond
procedural drawing, an image → packed-1bpp header converter — with an `ImageDitherAlgorithm` choice
baked in at build time, not runtime — is the missing piece.

### 4.3 Packed-1bpp implementation notes

The framebuffer is **MSB-first, row-major**: `bitMask(x) = 1 << (7 - (x & 7))`,
`byteIndex(x,y) = y * BYTES_PER_ROW + (x >> 3)`, `BYTES_PER_ROW = (W+7)/8`. For 240×280 that is
**30 bytes/row, 8400 bytes total — 1.58% of the RP2350's 520 kB SRAM**. Because the library is
MSB-first and the panel scans left to right, the 1bpp→RGB565 LUT index is the source byte unchanged;
no bit reversal is needed.

- **Ordered dither → byte ops (N2).** Precompute `uint8_t tile_row[tile_h][8/tile_w or 1]`; a span is
  head-partial-byte, aligned run, tail-partial-byte. Same three-case structure `Framebuffer::fillSpan`
  already uses for solids.
- **Content-anchored pattern (R5).** Subtract the shape origin from the pattern offset. Because
  `PatternTransform` is applied before the predicate, this costs two integer adds per pixel and
  nothing else.
- **Tile-quantised motion (R6).** Snap the shape's screen position to a multiple of the tile size
  along the motion axis; the pattern phase is then bit-identical frame to frame.
- **Knockout (R11 / N4).** Dilation by 1 px = OR the mask into a scratch at 8 neighbour offsets.
  Then `AndNot` to clear, `Or` to draw. Three passes over a small rect; cheap.
- **Corner trim (N3).** For a rounded rect of radius `r` in a `W×H` buffer, row `y` has
  `xmin(y) = r - floor(sqrt(r² - (r-y)²))` for `y < r`, mirrored for the bottom. Precompute 280
  entries; at push time AND the first and last bytes of each row with the derived edge masks.
- **Region push alignment.** Snap `region.x` down to a multiple of 8 and `region.x + w` up to a
  multiple of 8 before expanding. That costs ≤ 7 px of overdraw per edge and removes the inner-loop
  shift entirely.
- **Never call error diffusion per frame.** `image_dither` is one-shot; it allocates a 2–3 row
  scratch via `onebit::alloc` for the duration of the call, and traversal is left-to-right,
  top-to-bottom (serpentine is explicitly reserved for future work).

---

## 5. Screen recipe (the default)

A concrete starting point that satisfies every rule above.

```
Polarity      light-on-dark: PixelFormat ink = white, paper = black  (R22)
Framebuffer   Framebuffer<240, 280>                                   (8400 B)
Corner        per-row [xmin,xmax] table, r = 44                       (N3, 560 B)
Safe box      Rect{16, 16, 208, 248} -> 26 x 19 cells at TERM_8X12    (R19)
Background    full-bleed 240x280, solid(false) or blueNoise(24)       (R20, R2)
Chrome        2 px drawRect just inside the corner arc                (R12, R20)
Body type     TERM_8X12 inside the safe box                           (R14)
Headline      renderString, charHeight 32, strokeWidth 3              (R15)
Type on tex.  renderStringWithHalo, haloColor = paper                 (R11, R18)
Accent fill   bayer(128, 4), content-anchored offsets                 (R3, R5)
Colour        AttributeMap(240, 280, cellHeight = 13), one accent     (R25, R26)
Transitions   TransitionKind::Wipe, 200 ms, easeInOut                 (R8)
Idle          backlight 100% -> 25% over 400 ms, easeInOut            (R23)
Update        DirtyRectTracker; full push only on scene change        (R10)
Review        braille goldens; read the diff                          (Section 4.1)
```

## 6. Pre-merge checklist

- [ ] Dithered area ≤ 25% of the frame.
- [ ] Every moving patterned element has a content-anchored `PatternTransform` offset, or is solid.
- [ ] No two adjacent regions distinguished by density alone.
- [ ] Every ink-over-pattern has a halo.
- [ ] Nothing readable or tappable outside `Rect{16,16,208,248}`; background bleeds full-frame.
- [ ] No text smaller than `TERM_6X9`; no anti-aliased or dithered glyphs; one font pairing.
- [ ] No fades implemented as density ramps.
- [ ] Typical frame pushes a dirty rect, not a full frame.
- [ ] `INVON` sent; polarity verified against a 1-px checkerboard canary (there is no MISO — the
      panel cannot be read back).
- [ ] `markAllDirty()` after any palette change.
- [ ] Braille goldens regenerated and the diff actually read.

---

## 7. Constraints at a glance

1. The panel is 240×280 portrait at 218 ppi (0.11655 mm pitch) on 913 mm² of glass — 70% of a
   Playdate's pixels on 44% of its area, with pixels 0.79× the linear size. Every Playdate
   type-size and density intuition must be scaled up one step.
2. Rounded corners of ~44 px radius physically clip the framebuffer. Minimum diagonal clearance is
   `44·(1 − 1/√2) ≈ 12.9 px`; use a 16 px inset. Content rect = `Rect{16,16,208,248}` = 26 × 19
   cells at `TERM_8X12`. Backgrounds still bleed full-frame.
3. The panel is **backlit and transmissive**, not reflective: white is emissive, not paper. Default
   to light-on-dark (`PixelFormat` ink = white, paper = black) or a full-white screen is a torch.
   Polarity lives in exactly one place, so the 1-bit design itself is unchanged.
4. Pope's pixel-persistence rule was justified by Sharp memory-LCD strobing, which the ST7789V2 does
   **not** have. Keep the rule anyway — perceptual dither crawl is universal, and is worse at 218 ppi
   with IPS contrast.
5. The fix for crawl is anchoring the pattern to the **content**, not the screen — the 2D analogue of
   Obra Dinn's camera-centred sphere. `PatternTransform{offset_x, offset_y}` already implements it;
   leaving it at zero on moving content is a bug.
6. Pope's dither doctrine is restrictive: use it as little as possible, and simulating 8-bit
   grayscale is the failure mode. Budget ≤ 25% of frame area under any pattern; reserve solid black
   and solid white for the two most important things on screen.
7. Ordered dither (Bayer 8×8, blue noise 128×128) is stateless and anchorable; error diffusion is
   sequential and temporally chaotic. Reserve `FloydSteinberg` / `Atkinson` / `SierraLite` for
   one-shot static photographic content — `image_dither` allocates scratch and must never run per
   frame.
8. In 1-bit there is no implicit edge: two regions differing only by density is a smudge. Every
   boundary needs a drawn outline, a paper gutter, or a change of pattern **kind**. Black ink over a
   dark pattern is invisible — knockout halos are mandatory (`drawDropShadow` `halo_px`,
   `renderStringWithHalo`).
9. No wire format below 12 bpp exists. RGB565 = 16× expansion = 134,400 B/frame = 17.2 ms at the
   safe 62.5 MHz SCK ceiling, against an 18.9 ms panel scan, with **no tearing-effect line wired**.
   Full-frame pushes tear; `DirtyRectTracker` partial updates are the design, not an optimisation.
10. No driver exists for this board — `platform/pico-example` targets the 2.8-inch panel.
    `PanelGeometry::st7789_240x280_1in69()` exists; the `WindowedDisplayDriver` subclass, the
    mandatory +20 RASET offset in portrait, unconditional `INVON`, ping-pong DMA strip buffers and
    the GPIO25 PWM backlight override all still need writing.
11. `AttributeMap` gives a per-8×N-cell colour channel the Playdate has no equivalent of, and
    dithering inside a coloured cell yields real tints for free — but a pen costs +28.8% on draw and
    +11.2% on push, `expandRectWithAttributes` carries colour for **RGB565 only**, a palette change
    dirties nothing (call `markAllDirty()`), and `consumeDirty()` clears on read.
12. Typography follows Pope's Mars answer: stroke-vector font for headlines (`strokeWidth` is the
    bolding axis), bitmap font for body, one family per screen, never anti-aliased, never dithered.
    `TERM_5X7` is only 0.82 mm tall on this glass — use `TERM_6X9` or `TERM_8X12` for body text.
13. Missing library work, in priority order: rounded-corner mask (per-row `xmin`/`xmax` table,
    560 B), byte-hoisted pattern fills (only `Solid` takes the fast path today), a general
    knockout/dilate operator, a pixel-space scroll/blit-shift helper, vector-font rotation, real
    STBN (`blueNoiseAnim` is explicitly not STBN), a backlight ramp helper, and motion-stability
    golden tests.

## 8. Open questions

- **Light-on-dark versus dark-on-light is unverified on hardware.** The reasoning — backlit IPS,
  high contrast, 218 ppi, glare — points strongly at light-on-dark, but it inverts the library's
  default sense of "ink" and should be confirmed on the glass before the whole UI is built around
  it.
- **The Obra Dinn dither mechanics rest on secondary sources.** Pope's primary TIGSource post and
  `dukope.com/devlogs` both serve HTTP 403 to automated retrieval. The 8×8 Bayer, 128×128 blue noise
  and camera-centred-sphere details come from three consistent secondary accounts, not the original.
  If the exact mechanics matter, retrieve the forum post by another route.
- **Quotations from *Working in One Bit* are second-hand.** They are short and consistent with the
  rest of the corpus, but verify wording against the original devlog before republishing them at
  length.
- **The ~44 px corner radius is inferred arithmetic**, from the sibling module's R5.15 mm drawing
  divided by the 0.11655 mm pitch. Measure the real clip on hardware — draw a 1 px full-perimeter
  border and photograph it — before freezing the safe box.
- **No timing figure here has been measured.** 17.2 ms full frame, 1.47 ms text line and 53 Hz
  refresh are all arithmetic from sourced clock numbers. This board has never been run with this
  library.
- **Landscape orientation is unresolved.** The two vendor demos disagree on the landscape MADCTL
  (0x78 in the basic C/Arduino driver, 0xA0 in the LVGL driver), and the +20 GRAM offset migrates
  between CASET and RASET on rotation. The LVGL landscape path is additionally suspect: it leaves
  width 240 / height 280 unswapped in both branches while setting MADCTL MV. Portrait MADCTL = 0x00
  is unanimous across all vendor drivers. If the design ever wants 280×240, resolve it on hardware
  first.
- **How visible dither crawl actually is at 218 ppi is unmeasured.** R6 (moving things are solid)
  may be relaxable to R6-quantised at this pitch; a scrolling-patterned-panel test on real glass
  would settle how much of Pope's caution is still needed.
- **Whether a real STBN volume is worth the flash cost is unknown** until there is a concrete
  animated-dither requirement. Until then R5 / R6 make it unnecessary.
- **No asset pipeline exists**, and no decision has been made about whether this UI is fully
  procedural or needs authored 1-bit bitmaps. That choice determines whether N10 is needed at all.

## 9. The hourglass face

A draining-sand hourglass is a planned timer face, and it is the hardest rendering problem in the
product, because it is the case where §1.2's anchoring question has no obvious answer.

Sand wants to be a texture — a solid black bulb reads as ink, not grain — but the sand body is not
a rigid object that translates. Its silhouette *changes shape* every frame: the upper cone drains
from a falling surface, the lower pile grows from a rising one. `PatternTransform{offset_x,
offset_y}` cancels a translation, and here there is no translation to cancel, so the R5 recipe does
not apply mechanically.

The choice, stated plainly:

- **Screen-anchored** (offset fixed at zero) means the pattern is a static field and the sand
  boundary sweeps through it. Interior pixels never change state; only the row at the moving
  boundary flips. That is the *minimum* possible pixel churn per frame, which is exactly what §1.4
  asks for — and it is the option the naive reading of R5 would reject.
- **Object-anchored** (offset tracking the sand body) means the pattern travels with the mass, which
  is intuitively "correct" for a material, but the mass has no single origin: anchoring to the
  bulb's top makes the pile crawl, anchoring to the pile's top makes the bulb crawl.

The likely resolution is a third framing: anchor the pattern to the **vessel**, which *is* rigid, and
let the sand boundary sweep through a vessel-local texture field. That collapses to the
screen-anchored case whenever the vessel is stationary and behaves correctly when the device is
flipped or tilted — the flip inverts the face through `rotate_deg = 180`, which is inside the 90°-step
limit the library supports. It also keeps the falling stream at the neck solid per R6, since that is
the one part of the image that genuinely translates.

None of this is settled. It is exactly the scenario N8's motion-stability goldens exist to
adjudicate, and it should be resolved with a rendered frame sequence before the face is built.
Watch the R2 budget while resolving it: a full bulb of dithered sand can exceed 25% of the frame on
its own.
