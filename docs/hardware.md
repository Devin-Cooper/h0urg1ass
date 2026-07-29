# Hardware

Reference for firmware work on the **Waveshare RP2350-Touch-LCD-1.69**. It assumes a competent
embedded engineer who has never seen this particular board.

**Validation status.** No unit of this board has been powered up. Its sibling, the
RP2350-Touch-LCD-2.8, was hardware-validated on 2026-07-28 and shares the MCU, the SDK, the
PL022+DMA transport path and the ST7789 controller family. Everything transferred from the 2.8
is marked *validated elsewhere*; everything panel-specific to this board (280 rows, the +20 GRAM
offset, the rounded corners, the CST816 touch controller) is **unverified on hardware**.

Citations use `[S#]` tags resolved in [Sources](#sources) at the foot of the document. Every
`[S#]` is a primary artifact — a datasheet, the board schematic, a vendor demo archive or a
vendor wiki page.

Repository-relative paths: the reference firmware for the sibling 2.8" board lives in the
submodule at `third_party/1bit-display/platform/pico-example/`. Unless stated otherwise, file
paths in §4 are relative to that directory; portable-library paths (`include/…`, `src/…`,
`tests/…`) are relative to `third_party/1bit-display/`.

---

## 1. What this board is

A 1.69-inch wearable-format RP2350A module: dual Cortex-M33 *or* dual Hazard3 RV32 (architecture
selected at boot), 520 kB SRAM, **no PSRAM**, 16 MB discrete QSPI NOR flash (W25Q128JVSIQ)
[S3][S9]. The display is a 240 × 280 IPS TFT with **physically rounded corners**, driven by a
Sitronix ST7789V2 over write-only 4-wire SPI on **SPI1**; there is no MISO and no TE line, so the
controller can neither be read back nor synchronised to scan-out [S3][S4]. A CST816-family
single-point touch controller, a QMI8658C IMU and a PCF85063A RTC share one 400 kHz I²C bus
(`i2c1`). The board also carries a buzzer, a PWM backlight, a Li-ion charger with ADC battery
sense, and a **soft power latch that firmware must hold** to stay alive on battery. Vendor demos
overclock the part to 200 MHz against a 150 MHz rating [S6][S9].

| Block | Part | Bus / interface | Address or pins | Notes |
|---|---|---|---|---|
| MCU | RP2350A, QFN-60 | — | GPIO0–29, ADC on 26–29 | 150 MHz rated; 16 DMA ch; 3 PIO blocks; **stepping unknown (A2 vs A3)** [S9] |
| Flash | W25Q128JVSIQ | QSPI CS0, XIP | — | 16 MB; `pico2.h` defaults to 4 MB [S15] |
| Display | ST7789V2, 240×280 IPS | 4-wire SPI1, 8-bit frames, mode 0 | SCK 10 / MOSI 11 / DC 8 / CS 9 / RST 13 | RGB565 (COLMOD 0x05); **write-only**; **no TE**; ~53 Hz panel refresh [S4][S8] |
| Backlight | 1 LED string via DMG1012T-7 FET | PWM, active high | GPIO25 (PWM slice 4, ch B) | No enable pin, no constant-current driver; R1 = 10 Ω ballast [S3] |
| Touch | CST816T/S/D (one part, three names) | i2c1 @ 400 kHz, 8-bit regs | **0x15**, INT 21, RST 22 | **1 contact max**; ChipID reg 0xA7 == 0xB5; auto-sleeps after 2 s [S4][S11][S12] |
| IMU | QMI8658C | i2c1 (shared) | **0x6A** (SDO tied GND), INT1 23, INT2 24 | 6-axis [S3][S6] |
| RTC | PCF85063ATL | i2c1 (shared) | **0x51**, INT 18 | Own 32.768 kHz crystal (Y1) + SH1.0 backup header (BAT1, no cell fitted); **rechargeable cells only** [S3][S5] |
| Buzzer | SS8050 NPN via 4.7 kΩ | PWM | GPIO2 (PWM slice 1, ch A) | Vendor: wrap 2000, clkdiv 200, 50 % duty [S6] |
| Power | RT9193-33PB LDO + ETA6096 charger | — | SYS_EN 15, PWR key 14, BAT_ADC 29 | Divider R11 200 kΩ / R12 100 kΩ → V_batt = V_adc × 3. Topology partly disputed — see §3 [S3] |
| Wireless / SD / audio | **none** | — | — | [S3] |

---

## 2. Complete pin map

Every row is corroborated by the schematic net labels **and** at least one vendor pin header; five
independent vendor artifacts agree with each other and with the schematic [S3][S4][S6][S7].

| Function | GPIO | Direction / mode | Notes |
|---|---|---|---|
| **Display — ST7789V2 on SPI1** | | | |
| LCD_DC | **8** | Push-pull out | 0 = command, 1 = data. Also the only alternate QMI CS1n pin that exists on-board besides GPIO0 |
| LCD_CS | **9** | Push-pull out | **Plain GPIO, NOT the PL022 hardware CSn.** Never `GPIO_FUNC_SPI` |
| LCD_CLK / SCK | **10** | `GPIO_FUNC_SPI` | SPI1 SCK; CPOL=0 / CPHA=0 |
| LCD_MOSI / DIN | **11** | `GPIO_FUNC_SPI` | SPI1 TX, the only data lane |
| LCD_MISO | *(none)* | — | `LCD_MISO_PIN (12)` in the vendor headers is a **phantom** — GPIO12 is not routed anywhere on this board |
| LCD_RST | **13** | Push-pull out, active low | Vendor reset: high 100 ms → low 100 ms → high 100 ms |
| LCD_BL | **25** | `GPIO_FUNC_PWM`, active high | PWM slice 4 channel B |
| **Touch / IMU / RTC — shared i2c1 @ 400 kHz** | | | |
| I2C1_SDA | **6** | `GPIO_FUNC_I2C` + internal pull-up | Shared by all three devices |
| I2C1_SCL | **7** | `GPIO_FUNC_I2C` + internal pull-up | Shared |
| TP_INT | **21** | Input, **internal pull-up**, active-low pulse | CST816 |
| TP_RST | **22** | Push-pull out, active low | CST816 |
| IMU_INT1 | **23** | Input | QMI8658C |
| IMU_INT2 | **24** | Input | QMI8658C |
| RTC_INT | **18** | Input, internal pull-up | PCF85063A, open-drain /INT |
| **Power / misc** | | | |
| SYS_EN / BAT_PWR | **15** | Push-pull out, **drive HIGH early** | Soft power latch — see §3 and trap #4 |
| PWR_KEY (Key1) | **14** | Input, internal pull-up | Power button; reads LOW while pressed |
| BAT_ADC | **29** | ADC channel **3** | V_batt = V_adc × 3 via R11 200 kΩ / R12 100 kΩ. **Whether the divider is gated is disputed — see §3** |
| BUZZER | **2** | `GPIO_FUNC_PWM` | PWM slice 1 channel A |
| BOOT (Key2) | *(QSPI_SS_N)* | — | Not a GPIO; pulls flash CS low, sampled by boot ROM |
| RESET (Key3) | *(RUN)* | — | Not a GPIO |
| **Broken out on test points TP1–TP11** (pad pitch **2.154 mm**, not 2.54 mm) | | | |
| Free user I/O | **0, 1, 16, 17, 20, 26, 27, 28** | any | 26/27/28 = ADC0/1/2. GPIO0 is the **only** free QMI CS1n candidate if PSRAM is ever bodged on. TP1–TP3 are VBUS / GND / 3V3 |
| **Present on die, NOT routed — unusable without rework** | | | |
| — | **3, 4, 5, 12, 19** | — | Net-labelled in the RP2350A symbol, connected to nothing, not on any pad [S3] |

Two consequences worth internalising:

* **HSTX is useless here.** HSTX is hard-wired to GPIO12–19 and output-only; the LCD clock and
  data are on GPIO10/11 [S9]. PIO is the acceleration path if PL022+DMA is not enough.
* **GPIO12 is not a pin.** Do not configure it, do not enable its input buffer (see trap #9).

---

## 3. Power path

### Disputed: the power path

Two readings of this board's power topology are in circulation. They differ on three points, and
**neither is settled**. Both are stated in full below, followed by the firmware consequence they
share and the measurements that would decide between them.

**Reading A — the commonly-documented description.**

1. The RT9193-33PB 3.3 V LDO (U1) has its **EN pin driven by the SYS_EN net, i.e. GPIO15**.
   Firmware holding GPIO15 high is what keeps the LDO enabled; dropping it disables the regulator
   and the board powers down.
2. **Q3 (AO3401 P-FET) gates the battery-sense divider**, so the R11/R12 divider does not drain
   the cell continuously.
3. No statement is made about a VBUS-to-VSYS path.

**Reading B — from a direct reading of the board schematic PDF** (`RP2350-Touch-LCD-1.69.pdf`,
md5 `905f980ed9df9692c25f705c49573984`, rendered at 600 dpi and inspected block by block) [S3].

1. **U1 pin 3 (EN) is wired directly to VSYS/VIN.** The LDO self-enables the instant VSYS
   appears; SYS_EN/GPIO15 never touches it. The latch is not the LDO.
2. **GPIO15 drives a transistor stage, not a regulator enable.** GPIO15 → R3 1 kΩ → base of
   **T1 (SS8050 NPN)**, with R4 10 kΩ base-to-ground. T1's collector holds the gate of
   **Q3 (AO3401 P-FET)** at Vce(sat) ≈ 0.1 V.
3. **Q3 is the battery pass FET**, in the KEY block: source = `B+`, drain = `VBAT`, gate pulled
   to `B+` by R9 10 kΩ (so the default state is off). Key1 (PWR) also pulls that gate down
   through D1 (RB521S-30), landing it at ≈0.25 V — Vgs ≈ −3.45 V, Q3 on. Key1 additionally
   reaches GPIO14 via D2 (RB521S-30) → node M → R8 10 kΩ → 3V3, which is why GPIO14 reads LOW
   while pressed.
4. **The R11 200 kΩ / R12 100 kΩ divider is hard-wired straight across `B+` to GND**, with C10
   and C12 (100 nF each) on the tap. Nothing gates it. It therefore drains **~12.3 µA
   continuously at 3.7 V**, even with the board apparently off. Combined with the ETA6096's
   20 µA quiescent draw at BAT, "off" is roughly **32 µA, not zero**.
5. **VBUS reaches VSYS through D4** (MBR230LSFT1G, 2 A / 30 V Schottky). The board therefore
   **cannot power itself off while USB is connected**, and every bench test of the off path with
   a cable attached is misleading.

Topology as drawn under reading B:

```
                    ┌──────────────────────────────────────────────┐
   USB-C VBUS ──────┤ D4  MBR230LSFT1G  (2A/30V Schottky)          ├──┐
        │           └──────────────────────────────────────────────┘  │
        │                                                             │
        └──► ETA6096 (U2)  VIN … SW ─ L1 2.2µH ─ BATS ──► B+          │
             ISET ─ R7 160k ─ GND  →  ≈1.03 A charge                  │
             STAT (pin 9) = NOT CONNECTED                             │
                                                                      ▼
   J1 PH1.25 pin1 ── B+ ──┬── R11 200k ──┬── R12 100k ── GND      ┌─ VSYS ─┐
                          │   (always connected!)  │              │        │
                          │                    BAT_ADC ─► GPIO29  │        │
                          │                                       │        │
                          │  ┌─────────────────────────────┐      │        │
                          └──┤ Q3  AO3401  P-FET           ├─ VBAT ┘        │
                             │  S=B+  D=VBAT  G=node N     │  │             │
                             └─────────────┬───────────────┘  │             │
                                     R9 10k│ (gate → B+, default OFF)       │
                                           │                  │             │
   GPIO15 ─ R3 1k ─┬─ base T1 (SS8050) ────┤ collector        │             │
       (SYS_EN)    └─ R4 10k ─ GND         │                  │             │
                                           │              ┌───▼──────────┐  │
   Key1 (PWR) ─┬─ D1 RB521S-30 ────────────┘              │ Q2 AO3401    │  │
               │                                          │ G ← R10 100k │  │
               └─ D2 RB521S-30 ─ node M ─ R8 10k ─ 3V3    │   ← VBUS     │  │
                                    │                     └───┬──────────┘  │
                                 GPIO14 (SYS_OUT, reads LOW when pressed)   │
                                                                 VSYS ◄─────┘
                                            │
                                  ┌─────────▼──────────┐
                                  │ U1 RT9193-33PB     │
                                  │ EN ── tied to VIN  │   ← not GPIO15
                                  └─────────┬──────────┘
                                           3V3  →  RP2350, LCD, touch, IMU, RTC, flash
```

**Argument advanced for reading B, recorded but not decisive:** under reading A the board could
never cold-boot on battery, because GPIO15 is an output of a chip that only has power once the
LDO is enabled. Something other than a GPIO must therefore start the rail. That is an argument
from internal consistency, not a measurement, and reading A could still be recovered if some
other element (a button path directly into EN, for instance) were present and unrecorded.

**The firmware consequence is identical under both readings.** GPIO15 must be driven high as the
literal first statement of `main()` — before `stdio_init_all()`, before the startup idle, before
anything that could hang. Under reading A that holds the regulator enabled; under reading B it
holds the battery pass FET on. In both cases, on battery the board is alive only while a finger
is on Key1 until the latch closes, and in both cases driving GPIO15 low is the power-off action.
The same follows for resets: anything that resets `IO_BANK0` / `PADS_BANK0` releases the pad and
therefore releases the latch, so on battery a reset is a power-off rather than a reboot,
whichever reading is correct.

```c
int main(void) {
    // MUST be first. Nothing before this — not stdio_init_all(), not the
    // startup idle, not a printf. On battery the board is alive only
    // because a finger is on the button, and fingers leave.
    gpio_init(15);                 // SYS_EN / BAT_PWR
    gpio_set_dir(15, GPIO_OUT);
    gpio_put(15, 1);               // latch closed

    sleep_ms(4000);                // now the startup idle (trap #10) is safe
    stdio_init_all();
    ...
}
```

There is real margin (10–20 ms of boot against a 100–300 ms human press, both estimates), but the
margin evaporates the moment a slow init is added ahead of the latch, and the failure is
invisible over USB.

**What to measure to settle it.** Each of these is a single bench measurement:

1. **LDO enable source.** On battery, no USB, board running: meter U1 pin 3 (EN) against VSYS and
   against the GPIO15 pad. EN sitting at VSYS while GPIO15 is low ⇒ reading B. EN following
   GPIO15 ⇒ reading A.
2. **GPIO15's load.** Check continuity/DC from the GPIO15 pad through R3 to a transistor base,
   and from that transistor's collector to Q3's gate. A transistor stage between GPIO15 and Q3
   confirms reading B item 2.
3. **Quiescent battery current with the board "off".** Break `B+` and meter it, no USB attached.
   Reading B predicts ≈32 µA (12.3 µA divider + ≈20 µA ETA6096). A materially lower figure —
   around 20 µA, i.e. charger quiescent alone — supports the divider being gated (reading A).
4. **Divider tap with the board off.** Probe the BAT_ADC node (GPIO29). Reading B predicts
   B+/3 present continuously; reading A predicts ≈0 V.
5. **Off-while-charging.** With USB and a cell attached, drive GPIO15 low. Reading B predicts the
   board keeps running (VBUS → D4 → VSYS). If it powers down, D4 either is not fitted or does not
   feed VSYS.
6. **First ADC read.** On battery, USB out, read GPIO29 before asserting any other GPIO. A
   reading that already tracks the cell means the divider is ungated.

Until these are taken, treat the battery-sense enable question as open (see
[Open items](#open-items)) and write the firmware so it is correct either way: assert GPIO15
first, and do not assume the ADC divider needs enabling — but log the raw code so a zero reading
is visible rather than silently interpreted as a flat cell.

### Undisputed schematic detail

These are read off the same schematic [S3] and are not in contention:

* **Charge current ≈ 1.03 A.** R7 = 160 kΩ on the ETA6096's ISET pin. The ETA6096 table gives
  82 kΩ → 2 A and 150 kΩ → 1.2 A, i.e. I ≈ 164 / R(kΩ) A → **1.03 A at 160 kΩ** [S3][ETA]. The
  commonly-quoted "1 A" figure is therefore backed by a datasheet relation, not just by a
  silkscreen note — but it is a calculated value and has not been measured.
* **No charge-status signal exists.** ETA6096 STAT (pin 9) is an unterminated stub, and VBUS is
  not routed to any GPIO. Firmware cannot detect charging directly; it can only infer a host from
  CDC enumeration, or suppress the state-of-charge display when V_batt > 4.22 V (only reachable
  under charge).
* **Screen-dark is the power-on default.** The backlight ballast is R1 = 10 Ω from LEDK to Q1's
  drain. Q1's gate carries both R2 100 kΩ to 3V3 *and* R6 10 kΩ to GND, so an undriven GPIO25
  sits at 0.30 V — below the DMG1012T-7 threshold. The backlight is off until firmware drives it.
* **ADC_AVDD (U5 pin 44) is tied straight to the 3V3 rail** — no filter, no external reference.
  VREG_AVDD (pin 46) *does* get an R16 33 Ω + C30 4.7 µF filter; the ADC does not. The ADC
  reference is therefore the RT9193's output, ±2 %, and it carries the PWM-chopped backlight
  ripple.
* **X1 is specified `12 MHz (2520) 12 pF ±10 ppm`** on the schematic BOM note; loading caps
  C27/C29 = 15 pF ±5 %.
* **RTC backup topology:** `3V3 → D3 (B5819WS Schottky) → VDD_RTC`, BAT1 pin 1 on the same node,
  C15 1 µF, **no series resistor in the charge path**. VDD_RTC ≈ 3.3 − 0.25 ≈ **3.05 V** while
  the board is on. That float voltage suits an ML-series rechargeable; it under-charges a
  LIR2032; and it is a charging path into a primary cell — **never fit a CR2032**. No cell is
  fitted from the factory.
* **PCF85063A CLKOUT (pin 9) is explicitly marked no-connect**, as is pin 8. CLKOE (pin 3)
  terminates in a bare stub and appears to be floating — worth confirming on hardware, since the
  datasheet requires inputs to sit at a defined level.
* **RT9193-33PB:** 300 mA, Iq 90 µA typ / 130 µA max at zero load, output accuracy ±2 %, dropout
  220 mV @ 300 mA, shutdown < 0.01 µA [RT].
* **ETA6096:** CV 4.21 V typ, quiescent at BAT 20 µA @ 3.6 V, termination 130 mA, recharge
  threshold −160 mV, pre-condition below 2.9 V at 200 mA [ETA].
* **Battery connector family is ambiguous:** the product page says MX1.25 [S5], the schematic
  footprint says PH1.25 — same 1.25 mm pitch, different mating part. Verify before ordering
  cables.

### Battery sense arithmetic

```
V_batt = V_adc × (R11 + R12) / R12 = V_adc × 3          (nominal)
V_adc  = raw × VREF / 4096
VREF   = ADC_AVDD = the 3V3 rail  (RT9193, ±2 %)

V_batt(mV) = raw × 3300 × 3 / 4096 = raw × 2.4170 mV
```

| Battery | V_adc | Nominal raw code |
|---|---|---|
| 4.20 V (full) | 1.400 V | 1738 |
| 3.70 V (nominal) | 1.233 V | 1531 |
| 3.40 V (practical cutoff) | 1.133 V | 1407 |
| 3.00 V (cell floor) | 1.000 V | 1241 |

Resolution at the battery is **2.417 mV/LSB** and the useful 3.40–4.20 V span is 331 codes, so
resolution is not the limiting factor. Accuracy is:

| Error source | Magnitude | At 3.7 V |
|---|---|---|
| R11 and R12 both ±5 % [S3] | ratio ranges 2.81 … 3.21, i.e. **−6.3 % / +7.0 %** | **±0.26 V** |
| VREF = 3V3 rail, RT9193 ±2 % [RT] | ±2 % | ±0.074 V |
| ADC noise: ENOB 9 min / 9.5 typ [S9] | ±4–6 LSB | ±10–15 mV |
| Residual INL/DNL | **unpublished** — the RP2350 datasheet §12.4.5 reads, in full, "INL and DNL — Details to follow" [S9] | unknown |
| **Worst case combined** | **≈ ±9 %** | **≈ ±0.33 V** |

±0.33 V spans the entire useful discharge curve, so an uncalibrated reading is not a fuel gauge.
The dominant term is a *gain* error (the resistor ratio), so a **single-point calibration fixes
it**: measure `B+` with a meter, enter it once, store the correction, and the residual drops to
the ±2 % reference term. Two further notes: the divider's Thévenin source impedance is
R11∥R12 = **66.7 kΩ** against an ADC input impedance specified as min 100 kΩ [S9], so expect a
systematic under-read that the same one-point calibration absorbs; and RP2040-E11 (the missing-code
DNL spikes) **is fixed on RP2350** [S9], though community measurement still reports residual jumps
at multiples of 512.

---

## 4. Delta from the validated 2.8 board

Reference implementation: `third_party/1bit-display/platform/pico-example/`. Copy that directory
wholesale, then apply the deltas below.

### 4a. Carries over unchanged

| File | What transfers |
|---|---|
| `pico_sdk_import.cmake` | Verbatim |
| `flash.sh` | Verbatim except `UF2=build/onebit_pico_lcd28.uf2` → the new target name. The 1200-baud CDC touch, the `picotool load -x` preference, the `/Volumes/RP2350` fallback with its retry loop, and the `-DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350` cmake args are all board-independent RP2350 mechanics |
| `CMakeLists.txt` | Verbatim except `project(onebit_pico_lcd28 …)` / `add_executable` name. Keep `PICO_FLASH_SIZE_BYTES=16777216`, `pico_enable_stdio_usb(…,1)` + `pico_enable_stdio_uart(…,0)`, and the link list `pico_stdlib hardware_spi hardware_dma hardware_pwm hardware_vreg`. **Add** `hardware_i2c` (touch/IMU/RTC) and `hardware_adc` (battery) |
| `main/main.cpp` prologue | The `vreg_set_voltage(VREG_VOLTAGE_1_20)` → `set_sys_clock_khz()` → `clock_configure(clk_peri, …CLKSRC_PLL_SYS…)` sequence (lines 177–191), the 5-second countdown idle (198–201), `freeHeap()` via `extern "C" char __StackLimit / __bss_end__` (36–37, 59–63), and the whole measurement harness |
| `main/st7789_pico.cpp` | **Every method transfers unchanged**: `pickFormat()`, `setFrameBits()`, `releaseCs()`, `hardReset()`, `sendCmd()`, `waitIdle()`, `setWindow()`, `writePixels()`, `stripBuffer()`, `clear()`, `setBacklight()`. `setWindow()` already reads its `colStart/colEnd/rowStart/rowEnd` straight out of `onebit::Window`, so the +20 offset arrives as data — no code change. `init()` already guards INVON with `if (geometry().invert)` |
| Portable library layer | `WindowedDisplayDriver`, `Expander`, `PanelGeometry::window()` (`src/hal/panel_geometry.cpp`), `DirtyRectTracker`, `Palette`, `AttributeMap`, `demo/demo_pages.hpp` — all unchanged. The 2.8 bring-up needed **zero** changes above the transport; expect the same |
| `waitIdle()` discipline | `dma_channel_wait_for_finish_blocking()` **then** `while (spi_is_busy(spi_))` **then** drop CS. Do not shortcut this — DMA "done" only means the TX FIFO is fed |
| Ping-pong strips | `stripBuffer()` flipping `cur_ ^= 1` across two `malloc`'d buffers. 40 rows still works: 280 = 7 × 40 exactly, 19,200 B per strip |
| Backlight code | `pwm_gpio_to_slice_num()` + `pwm_set_gpio_level()` resolve the slice **and channel** from the GPIO number, so moving from GPIO16 (PWM0 A) to GPIO25 (PWM4 B) needs no code change beyond the pin |

### 4b. What must change

**1. Panel geometry factory.** In `main/st7789_pico.cpp:28`:

```cpp
: WindowedDisplayDriver(onebit::PanelGeometry::st7789_240x320(),   // 2.8
```
becomes
```cpp
: WindowedDisplayDriver(onebit::PanelGeometry::st7789_240x280_1in69(),
```

That preset already exists at `include/1bit/hal/panel_geometry.hpp:76` and is exercised by
`tests/hal/test_panel_geometry.cpp:71,82,171`. Its definition encodes the offset migration as
data:

```cpp
return PanelGeometry{240, 280, {0, 20, 0, 20}, {20, 0, 20, 0},
                     {0x00, 0x60, 0xC0, 0xA0}, true};
//                    W    H    colOffset[4]   rowOffset[4]   madctl[4]  invert
```

Rot0/Rot180 put +20 on **rows** (RASET); Rot90/Rot270 move it to **columns** (CASET).
`invert = true` drives the mandatory INVON.

**2. Resolution.** `main/main.cpp:41-42`: `constexpr int16_t H = 320;` → **`280`**. Framebuffer
becomes `Framebuffer<240,280>` = **8,400 B** (1.58 % of SRAM); RGB565 full frame =
**134,400 B**.

**3. Pin numbers — and this is where hardware can be damaged.** The `Pins` struct at
`main/st7789_pico.hpp:21-29` must change on **every field**, and the 2.8's numbers are *not
harmless* on this board:

| Field | 2.8 value | 1.69 value | What the 2.8 value does if left in place |
|---|---|---|---|
| `sck` | 10 | **10** | same |
| `mosi` | 11 | **11** | same |
| `miso` | 12 | **delete the field** | GPIO12 is unrouted; `gpio_init(12); gpio_set_dir(12, GPIO_IN)` at `st7789_pico.cpp:82-83` enables an input buffer on a floating die pad — the exact RP2350-E9 trigger on A2 silicon |
| `cs` | 13 | **9** | GPIO13 is **LCD_RST** here — the panel would be held in reset or reset randomly |
| `dc` | 14 | **8** | GPIO14 is **PWR_KEY** — this drives a push-to-ground button as an output |
| `rst` | 15 | **13** | GPIO15 is **SYS_EN**. `hardReset()` drives it low for 20 ms → **the board powers itself off** on battery |
| `backlight` | 16 | **25** | GPIO16 is a bare test point — the backlight silently never lights |

**4. Init-sequence register values.** `init()` in `st7789_pico.cpp:95-131` is tuned for the 2.8's
ST7789T3. Adopt the 1.69 vendor's values for the two that matter [S4][S6]:

| Command | 2.8 code | 1.69 vendor | Effect |
|---|---|---|---|
| PORCTRL `0xB2` | `{0x0C,0x0C,0x00,0x33,0x33}` | `{0x0B,0x0B,0x00,0x33,0x35}` | Porch timing |
| FRCTRL2 `0xC6` | `0x0F` | **`0x13`** | Frame rate: 0x13 → RTNA 0x13 → **~53 Hz** per [S8 §9.2.18] |
| COLMOD `0x3A` | `0x55` | `0x05` | Both are 16 bpp RGB565; `0x55` additionally sets the (unused) RGB-interface field. Either works over SPI |

Keep GCTRL `0xB7=0x35`, VCOMS `0xBB=0x19`, LCMCTRL `0xC0=0x2C`, VDVVRHEN `0xC2={0x01,0xFF}`,
VRHS `0xC3=0x12`, VDVS `0xC4=0x20`, PWCTRL1 `0xD0={0xA4,0xA1}`, then INVON / NORON / DISPON.

**5. Touch controller — a rewrite, not a port.** The 2.8's CST328 and this board's CST816 share
nothing but the bus:

| | 2.8 (CST328) | 1.69 (CST816) |
|---|---|---|
| I²C address | 0x1A | **0x15** |
| Register addressing | **16-bit** (`0xD000`–`0xD204`) | **8-bit** |
| Contacts | 5, with per-point pressure | **1** (`FingerNum @0x02`: "0: no finger 1: one finger") |
| RST / INT | 17 / 18 | **22 / 21** |
| Gestures | standby wake-up only | real: `GestureID @0x01` |
| Coordinates | `0xD000` block | `XposH/L @0x03/0x04`, `YposH/L @0x05/0x06`, high nibbles masked `0x0F` (12-bit) |

There is nothing to copy: **the 2.8 bring-up never exercised touch at all.** Model this as one
contact plus a gesture enum, not a multitouch array. Gate init on ChipID `0xA7 == 0xB5`. Write
`DisAutoSleep (0xFE) = 0x07` if a response faster than the 2 s auto-sleep is needed.

**6. Peripherals the 2.8 example has no code for at all:**

* **Buzzer** GPIO2 — no equivalent on the 2.8 (which has an I²S DAC instead).
* **IMU address differs**: QMI8658C at **0x6A** here (SDO tied to GND) vs **0x6B** on the 2.8.
  The vendor driver probes both; hard-coding 0x6B will fail.
* **RTC** PCF85063A 0x51 — same part, same address, but INT moves from GPIO5 to **GPIO18**.
* **Battery ADC**: GPIO**29** = ADC channel **3**, ×3 divider. The 2.8 used GPIO27/ADC1 with an
  explicit `BAT_EN` enable on GPIO26; **this board has no documented enable GPIO**, and whether
  the divider is gated at all is disputed (§3). Assume the ADC is readable directly, log the raw
  code, and verify on hardware.
* **Power latch** GPIO15 and **PWR key** GPIO14 — no counterpart on the 2.8.
* **No microSD, no I²S audio** — delete any of that if copying from the 2.8 BSP rather than from
  `platform/pico-example/`.

---

## 5. Traps, ranked by cost if missed

**Tier 1 — hours, and the symptom points somewhere else.**

1. **The `clk_peri` re-parenting trap.** `set_sys_clock_khz()` re-parents `clk_peri` to
   **PLL_USB at 48 MHz**, capping SPI at 24 MHz — *slower than doing nothing*. Measured on the
   sibling: naively overclocking to 250 MHz took the full-frame push from **36.01 ms to
   56.11 ms** (1.6× regression) *while CPU-bound expansion got faster* (1.332 → 0.791 ms), which
   reads exactly like a signal-integrity problem at the higher clock. This board is squarely in
   scope: its own vendor LVGL demo overclocks to 200 MHz and reparents explicitly [S6]. Fix,
   immediately after the clock change:
   ```c
   set_sys_clock_khz(250000, true);
   clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                   250000 * 1000ull, 250000 * 1000ull);
   ```
   **Always print `clock_get_hz(clk_peri)` and the value `spi_init()` returns.** `spi_init()`
   requests are silently clamped to an integer prescale × postdiv of `clk_peri`; a 62.5 MHz
   request lands on 37.5 MHz at clk_peri 150 MHz, 50 MHz at 200 MHz, and exactly 62.5 MHz at
   250 MHz **or 125 MHz**.
2. **`PICO_BOARD` unset.** The vendor's own `C/CMakeLists.txt` never sets it (its project name is
   even a copy-paste leftover, `project(Pico_ePaper_Code)`) [S4], so pico-sdk defaults to
   `PICO_BOARD=pico` = RP2040 and the build produces an **ARMv6-M binary that will not boot, with
   no diagnostics whatsoever**. The inherited `platform/pico-example/CMakeLists.txt` *also* does
   not set it — it relies on `flash.sh` passing `-DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350`. A
   bare `cmake -S . -B build` therefore yields the wrong architecture. Either always use
   `flash.sh`, or add `set(PICO_BOARD pico2 CACHE STRING "Board type")` before
   `include(pico_sdk_import.cmake)`.
3. **The DMA strip-buffer race.** `writePixels()` queues and returns. Expanding into one buffer
   repeatedly overwrites bytes the in-flight transfer has not read yet. **Symptom: sparse static
   scattered across the frame that does *not* change when the SPI clock is halved.** That
   clock-independence is the whole diagnosis — signal-integrity faults scale with clock, buffer
   races do not. Halving the clock is a two-minute test; do it before going near signal
   integrity. `stripBuffer()` must re-acquire per chunk and ping-pong. (Corollary for tests: a
   mock whose write is synchronous models a transport that does not exist — 542 host tests passed
   while the real driver corrupted every frame.)
4. **GPIO15 (SYS_EN) must be asserted to stay powered on battery.** Easy to miss because the
   board behaves perfectly over USB and only dies when unplugged, which looks like a battery or
   charger fault. Drive it high immediately after entry to `main()`, before anything else can
   hang. See §3 for the topology and §4b item 3 for the related hazard: mistakenly using GPIO15
   as LCD_RST makes `hardReset()` a power-down.

**Tier 2 — 30–60 minutes, but obvious once the failure mode is known.**

5. **The +20 GRAM offset moves between CASET and RASET on rotation.** The panel is 240×280 in a
   240×320 GRAM, centred: (320−280)/2 = 20. Portrait → **RASET** +20, CASET untouched.
   Landscape → the +20 **migrates to CASET** and RASET is untouched [S4][S6][S7]. Getting it
   backwards shifts the image 20 px and drags garbage in from the unused GRAM rows. Use
   `PanelGeometry::st7789_240x280_1in69()` rather than hand-rolling it. Related: **the two vendor
   demos disagree on the landscape MADCTL** (0x78/BGR vs 0xA0/RGB) and the LVGL landscape path
   double-rotates; **portrait (MADCTL 0x00) is the only sourced, self-consistent orientation.**
   Rotate in the blitter instead.
6. **INVON (0x21) is mandatory.** Every vendor driver sends it unconditionally [S4][S6][S7]. Omit
   it and the image is photo-negative — which for a 1-bit pipeline means black and white
   **silently swap** and look plausible. An inversion mistake and an ink/paper swap are
   indistinguishable on the bench, which is why `PixelFormat` and `PanelGeometry::invert` are the
   single place each is expressed.
7. **CS is a plain GPIO, not the PL022 hardware CSn.** GPIO9 is raised and lowered in software
   around every command and every data burst. Setting it to `GPIO_FUNC_SPI` makes the PL022 pulse
   it per frame and the controller loses the RAMWR stream. Corollary for performance: **hold CS
   low across the whole window setup + pixel stream** (`setWindow()` sets `pixelMode_ = true` and
   only `releaseCs()` drops it). For glyph-sized rects the per-transaction preamble otherwise
   dominates the payload.

**Tier 3 — minutes each, but each bites exactly once.**

8. **SPI is write-only — there is no electrical integrity check.** No MISO on the FPC [S3];
   `LCD_MISO_PIN (12)` in the vendor headers is a phantom. No controller-ID read, no GRAM
   read-back, no init verification. On the sibling ESP32 board, which *does* wire MISO, `RDDID`
   returned `00 00 00` at every clock tried. **Consequence: corruption can only be judged by
   eye.** Render a **1-pixel checkerboard band** as a canary — it alternates the wire between
   `0x0000` and `0xFFFF` every two bytes, the worst case for MOSI toggling, so marginal
   signalling shows there first. Put it in the middle of the glass, not near a corner.
9. **`PICO_FLASH_SIZE_BYTES` defaults to 4 MB.** `pico2.h` hard-codes `(4 * 1024 * 1024)` [S15]
   and this board has 16 MB. Only bites when data is placed past 4 MB or picotool address ranges
   are used, but it is a one-line fix already present in the inherited `CMakeLists.txt`:
   `PICO_FLASH_SIZE_BYTES=16777216`.
10. **The 1200-baud BOOTSEL reboot, and the idle-at-top-of-`main()` rule.** `pico_stdio_usb`
    reboots into BOOTSEL when the host opens its CDC port at 1200 baud
    (`stty -f /dev/cu.usbmodemXXXX 1200`), so iteration never needs the BOOT button — **but only
    while the firmware is alive with USB up**. Hence the 3–5 second idle with visible output at
    the very top of `main()`, before touching any hardware (and after the GPIO15 assertion, §3).
    Cost of the delay: five seconds per iteration. Cost of not having it: someone has to walk to
    the board and press BOOT+RESET. Also: **macOS frequently fails to mount `/Volumes/RP2350`
    after a *software* reset** even with the device plainly enumerated as `RP2350 Boot` — so
    flash over PICOBOOT with Homebrew's `picotool load -x` (the picotool that pico-sdk builds is
    compiled without libusb and cannot talk to a device). `flash.sh` already does touch →
    picotool → volume-copy-with-retries in that order.

**Also worth knowing:** SPI mode 0 works despite vendor drivers disagreeing (validated on both
2.8 boards). Three devices share `i2c1`, so a blocking IMU read stalls touch latency — serialise
deliberately. Pure black and white are byte-order-invariant, so a byte-swap bug is **invisible**
on 1-bit content; any endianness change needs a deliberately asymmetric test. The vendor LVGL
demo runs SPI at 100 MHz, **1.6× the ST7789V2's TSCYCW ceiling of 62.5 MHz** — start at or below
62.5 MHz.

---

## 6. Rounded-corner geometry and safe area

The glass is physically rounded. The sibling module's 2D drawing gives the viewing area as
28.27 × 32.93 mm with **R5.15 mm** corner radius [S13]; at the panel's 0.11655 mm pitch that is
**R5.15 / 0.11655 ≈ 44 pixels** of corner radius. The sibling wiki states it plainly: *"due to
the four round corners, some parts of the input images may not be displayed"* [S10]. Note that
the radius figure comes from the *module's* mechanical drawing, not this board's, and the two are
assumed to share a panel on the strength of matching active area, pixel pitch and outline —
inferred, not confirmed.

**The mask.** Treating the top-left corner pixel as (0,0), the arc centre sits at (44, 44) in
continuous coordinates. A pixel is off-glass iff, folding to the nearest corner,

```
u = 44 - min(x, 239 - x) - 0.5     v = 44 - min(y, 279 - y) - 0.5
hidden  ⇔  u > 0 and v > 0 and u² + v² > 44²
```

That hides **≈ 416 pixels per corner** (44² − π·44²/4 = 415.5), **≈ 1,662 px total = 2.5 %** of
the 67,200-pixel panel. Cost of a corner mask in `include/1bit/render/mask_buffer.hpp`: under
1 KB, or generate it procedurally.

**What it forbids**

* No text, glyph, status icon, clock, battery indicator or touch hit-target may sit within the
  44 × 44 corner boxes. Anything there is *partially* clipped, which is worse than fully hidden —
  a half-visible character reads as a rendering bug.
* Full-bleed borders drawn at `drawRect(fb, 0, 0, 240, 280)` will have their four corners eaten.
  Expect it; do not chase it as an offset bug.
* Progress bars, sliders and scroll tracks must not run edge-to-edge across the top or bottom
  44 rows.
* Do not put the checkerboard signal-integrity canary near a corner.

**Safe rectangles — actual numbers**

| Shape | Rect (x, y, w, h) | Corner clearance | Use for |
|---|---|---|---|
| **Mathematical minimum uniform inset** | (13, 13, 214, 254) | 0.16 px — knife-edge | Nothing. `44 × (1 − 1/√2) = 12.89`; a 12 px inset *is* clipped |
| **Recommended UI safe area** | **(16, 16, 208, 248)** | 4.4 px | All chrome, text, hit targets. 16 and 208 are **multiples of 8**, so the byte-packed framebuffer needs no partial-byte handling and `pushRegion` snapping is a no-op |
| Full-width band | (0, 44, 240, 192) | edge-to-edge | Anything that must span the whole width — status bars, list rows, terminal lines |
| Full-height column | (44, 0, 152, 280) | edge-to-edge | Vertical rules, side rails |

With the 8×12 terminal font, the recommended safe area gives **26 columns × 20 rows** (208/8,
248/12 = 20.67 → 20). Use that as `terminal_renderer`'s inset rather than the raw 240×280.

---

## 7. Timing budget

Full frame = 240 × 280 × 2 = **134,400 B** = 1,075,200 bits on one data lane. Wire time =
`bytes × 8 / baud`. "Actual" applies the **91 % bus efficiency measured on the sibling** at both
37.5 and 62.5 MHz — the remainder is per-chunk DMA reconfiguration.

### Which SPI clocks are actually reachable

`spi_init()` divides `clk_peri` by an integer prescale × postdiv and returns the achieved rate:

| `clk_peri` | Request 62.5 MHz → achieved | Verdict |
|---|---|---|
| 150 MHz (stock) | **37.5 MHz** (÷4) | In spec, slow |
| **125 MHz** | **62.5 MHz** (÷2) | Exactly the panel ceiling with the **MCU still in spec** — CPU 17 % slower, but expansion is only ~4 % of push time |
| 200 MHz (vendor demo) | **50 MHz** (÷4) | In spec, decent |
| 250 MHz | **62.5 MHz** (÷4) | Validated on the sibling; MCU overclocked 1.67× |

The ST7789V2's `TSCYCW` = 16 ns → **62.5 MHz is the controller's rated ceiling** [S8]. The
sibling ESP32 board ran the same controller family cleanly at 80 MHz including the checkerboard
canary, so there is headroom — but that is works-but-unvalidated, and untested warm and over long
runs.

### Push costs

| Update | Bytes | @37.5 MHz wire / actual | @62.5 MHz wire / actual |
|---|---:|---:|---:|
| **Full frame, RGB565** | 134,400 | 28.67 / **31.5 ms** | 17.20 / **18.9 ms** |
| Full frame, RGB444 (COLMOD 0x03) | 100,800 | 21.50 / 23.6 ms | 12.90 / 14.2 ms |
| 40-row strip (240×40) | 19,200 | 4.10 / 4.5 ms | 2.46 / 2.7 ms |
| One 24 px text line (240×24) | 11,520 | 2.46 / 2.7 ms | 1.47 / 1.6 ms |
| One 16 px row band (240×16) | 7,680 | 1.64 / 1.8 ms | 0.98 / 1.1 ms |
| 48×48 dirty rect | 4,608 | 0.98 / **1.14 ms** (measured) | 0.59 / **0.685 ms** (measured) |
| One 8×12 glyph cell | 192 | 0.041 / ~0.16 ms | 0.025 / **~0.12 ms** |

The 48×48 figures are measured on the sibling — a rect size is board-independent, so they
transfer directly. Back-solving them gives a **fixed per-window overhead of ~70–95 µs** (CASET +
RASET + RAMWR, GPIO toggles, DMA reconfiguration). That is why the 8×12 glyph costs ~5× its wire
time and why CS must be held across a multi-rect flush (`beginFrame()`/`endFrame()`).

Expansion is cheap and not the bottleneck: 1bpp→RGB565 for a full 134,400 B frame costs ~1.17 ms
at 150 MHz sys and ~0.69 ms at 250 MHz — **3.7 % of push time**, scaling cleanly with clock. A
`uint16_t lut[256][8]` (4,096 B) makes each source byte a 16-byte copy; the library is MSB-first
and the panel scans left-to-right, so the LUT index is the source byte unchanged, no bit reversal.

### What this means for frame rate

* **Panel refresh is ~52.8 Hz = 18.94 ms** (FRCTRL2 0x13 → 53 Hz; the datasheet formula
  `10 MHz / ((320+FPA+BPA)×(250+RTNA×16))` gives 10e6/(342×554) ≈ 52.8) [S8]. That is a hard
  ceiling on *visible* update rate regardless of push speed.
* At **62.5 MHz a full-frame push costs 18.9 ms — almost exactly one panel frame.** Full frames
  can free-run at panel rate and no faster. **60 fps with full-frame pushes is impossible**
  (18.9 > 16.67 ms budget).
* At **37.5 MHz** (stock 150 MHz sys, no `clk_peri` work) a full frame is 31.5 ms → **31.7 fps
  ceiling**, and 60 fps is 2× out of reach.
* **Partial updates are the primary path**, 16–32× cheaper. Measured on the sibling: a static page
  with a dirty tracker costs **0.31 ms/frame with 0 rects**; animated content runs
  **1.9–2.5 ms with 6–8 small rects**. Scaled to this panel, a dirty-rect UI leaves 85–98 % of a
  16.67 ms budget free.
* Practical rule: **budget 19 ms for any full repaint at 62.5 MHz.** To hold 60 fps, keep
  per-frame dirty area under ~50 % of the panel (67,200 B ≈ 9.5 ms push), which leaves ~7 ms for
  rendering.
* **Strip size has a knee around 40 rows.** On the sibling, 16 rows cost 40.00 ms where 40 rows
  cost 33.00; 80 rows bought nothing. Use 40 rows = 19,200 B × 2 ping-pong = 38,400 B; 280
  divides by 40 exactly.
* **No TE line**, so tear-free presentation is impossible. Mitigate by flushing top-to-bottom,
  keeping rects small, or free-running near 53 Hz.
* Memory is a non-issue: expect **≈ 506 kB free heap** with the 8,400 B framebuffer and both
  19,200 B strips allocated. A full 134,400 B RGB565 shadow would fit (25 % of SRAM) — don't; the
  1-bit buffer is already the source of truth.

---

## 8. Bring-up order

Each step is independently verifiable and adds exactly one unknown. Do not skip ahead — the
ordering exists so that when something fails, only one thing has changed.

**Step 0 — toolchain.** `PICO_SDK_PATH` pointing at a pico-sdk checkout; Homebrew `picotool`; an
`arm-none-eabi-gcc` on `PATH`.
**Verify:** `picotool version` prints, and it is the Homebrew build (the SDK's own is compiled
without libusb).

**Step 1 — skeleton that never touches the panel.** Copy `pico_sdk_import.cmake`,
`CMakeLists.txt` and `flash.sh` from `third_party/1bit-display/platform/pico-example/`, and
rename the target (e.g. `onebit_pico_lcd169`) in both `CMakeLists.txt` and `flash.sh`'s `UF2=`
line. Write a `main()` containing **only**: the GPIO15 assertion (§3), the
vreg/`set_sys_clock_khz`/`clock_configure(clk_peri, …PLL_SYS…)` prologue, `stdio_init_all()`, the
5-second countdown, and a print of `clock_get_hz(clk_sys)` / `clock_get_hz(clk_peri)` /
`freeHeap()`.
**Verify:** `picotool info -a build/*.uf2` reports target RP2350 / Cortex-M33 (this catches the
`PICO_BOARD` trap before any hardware is involved); after `./flash.sh --monitor` the countdown
appears on `/dev/cu.usbmodem*`; `clk_peri` reads the configured value, not 48,000,000. Then run
`picotool info` against the **live device** to read the chip revision and close the A2/A3 open
question (`SYSINFO->CHIP_ID.REVISION`: 0x2 = A2, 0x3 = A3).

**Step 2 — power latch.** Confirm the GPIO15 assertion is the first statement in `main()`:
`gpio_init(15); gpio_set_dir(15, GPIO_OUT); gpio_put(15, 1);`. Also configure GPIO14 as an input
with pull-up and print its state.
**Verify:** GPIO14 reads 1 idle and 0 while the PWR button is held. (The battery half of this is
verified at step 4, when there is something visible to keep alive.) This is also the natural
point to take the §3 dispute measurements, since the board is powered and minimally loaded.

**Step 3 — backlight only.** `gpio_set_function(25, GPIO_FUNC_PWM)`, slice from
`pwm_gpio_to_slice_num(25)`, ramp the level 0 → 255 → 0 in a loop. No SPI, no reset, no init
sequence.
**Verify by eye:** the glass glows and dims smoothly. This proves the board is powered, the FPC is
seated, and the backlight pin number is right, with zero display-controller involvement. If this
fails, nothing after it can work.

**Step 4 — SPI up, controller initialised, one solid colour.** Port `St7789Pico` with the new
`Pins`, the `st7789_240x280_1in69()` geometry and the 1.69 PORCTRL/FRCTRL2 values. Call `init()`
then `clear(WHITE)`, then `clear(BLACK)`, alternating every second. Print `panel.actualBaud()`
and `panel.stripBytes()`.
**Verify:** the whole glass goes uniformly white, then uniformly black. If it is inverted (white
where black was requested), INVON is missing or doubled. If it stays backlit-but-blank, check
that CS is a plain GPIO and RST is GPIO13. Now unplug USB with a battery attached — **the
alternating fill must continue**, proving the SYS_EN latch.

**Step 5 — geometry canary, before any real content.** Draw: a 1 px border rect at exactly
`(0, 0, 240, 280)`; a 1-pixel checkerboard band across the middle; single-pixel ticks at rows 0,
20, 279 and columns 0, 239.
**Verify:** (a) no 20 px band of garbage or blank at the top or bottom — that is the +20 RASET
offset landing correctly; (b) the border is flush with the visible glass on all four sides;
(c) the four corners of the border are visibly eaten by the arcs — that confirms the ~44 px radius
rather than an addressing fault; (d) the checkerboard band is clean, with no speckle. If there
**is** speckle, halve the SPI clock and look again: unchanged speckle = strip-buffer race,
reduced speckle = signal integrity.

**Step 6 — pin the clocks and measure.** Print `clock_get_hz(clk_peri)` and `spi_init()`'s return
alongside the request. Run the full-frame push benchmark from `main/main.cpp:232-248` with
`frameBytes = 240*280*2`.
**Verify numerically:** ~31.5 ms at 37.5 MHz or ~18.9 ms at 62.5 MHz, at ~91 % of the wire-time
floor. A number *above* the wire-time floor means the clock was stopped before DMA drained; a
full frame 1.6× slower than predicted means `clk_peri` is still on PLL_USB.

**Step 7 — partial update.** `panel.writeRegion(fb, Rect{96, 116, 48, 48})` in a loop, 200
iterations.
**Verify:** the rect lands where requested (not 20 px off), and costs ≈ 0.685 ms at 62.5 MHz /
1.14 ms at 37.5 MHz. Then `pushDirty()` with a `DirtyRectTracker` on an animated page and confirm
6–8 small rects rather than full frames.

**Step 8 — safe area.** Draw the recommended inset rect `(16, 16, 208, 248)` and the full-width
band `(0, 44, 240, 192)`, plus a line of 8×12 text at the top-left of the inset.
**Verify:** nothing is clipped at any corner, and the text baseline is comfortably clear of the
arc. Adopt these as constants before writing any UI.

**Step 9 — I²C bus scan, before any device driver.** `i2c_init(i2c1, 400*1000)`, `GPIO_FUNC_I2C` +
`gpio_pull_up` on 6 and 7, then probe every 7-bit address with a zero-length write.
**Verify:** exactly three devices respond — **0x15** (touch), **0x51** (RTC), **0x6A** (IMU). A
missing 0x15 usually means TP_RST (GPIO22) was never released. A device at 0x6B instead of 0x6A
means the SDO-to-GND reading is wrong; record it.

**Step 10 — touch.** Reset via GPIO22 (low ≥10 ms, then high, then wait), read register `0xA7` and
require **0xB5**. Then poll `0x01`–`0x06` on a GPIO21 falling edge (input, **internal pull-up**),
masking the X/Y high bytes with `0x0F`.
**Verify:** touching the four extremes of the *visible* glass reports x ∈ 0…239 and y ∈ 0…279,
with the origin at the same corner as the display origin. Separately, log `GestureID @0x01` for a
deliberate swipe up and a swipe down — **the vendor header and the CST816S register document
disagree** (header: `Down 0x01` / `UP 0x02`; register document [S12]: 0x01 = slide *up*, 0x02 =
slide *down*). Resolve it here and record the answer.

**Step 11 — the rest, in any order, each with its own verification:** buzzer on GPIO2 (PWM slice 1
A, wrap 2000, clkdiv 200, 50 % duty → audible chirp); battery ADC on GPIO29 / ADC channel 3
(`adc_select_input(3)`, ×3 divider → compare the printed volts against a meter on the cell, which
doubles as the one-point gain calibration of §3); RTC at 0x51 (set the time, power-cycle, read it
back); IMU at 0x6A (accelerometer magnitude ≈ 1 g at rest, and it moves when the board is tilted).

---

## Open items

Unresolved questions that need the bench. Record answers against this document.

* **The power-path dispute of §3** — three separate claims (LDO enable source, what Q3 gates,
  whether VBUS feeds VSYS through D4), each with a stated measurement.
* **Whether the battery-sense divider on GPIO29 needs a GPIO to enable it**, as the 2.8's did. No
  control net is documented, and this is a direct consequence of the §3 dispute.
* **Silicon stepping (A2 vs A3)** — read at step 1. A2 carries erratum RP2350-E9 (Bank 0
  input-buffer leakage overpowering internal pull-downs); prefer pull-**ups** on every input
  added. E9 does not bite as this design is configured (GPIO29 goes through `adc_gpio_init`,
  which disables the input buffer and the pulls; GPIO14 and GPIO18 are pulled up), but confirm
  before adding any pull-down input.
* **Whether 62.5 MHz — and any overclock above it — stays clean warm and over long runs** on this
  board's shorter FPC.
* **Rot90 / Rot180 / Rot270 MADCTL values.** The `st7789_240x280_1in69()` preset's quarter turns
  are inferred, and the two vendor demos disagree (0x78/BGR vs 0xA0/RGB), with the LVGL landscape
  path double-rotating on top of that.
* **CST816 gesture codes 0x01 / 0x02** — the vendor header says Down/Up, the CST816S register
  document [S12] says slide up / slide down. One is wrong.
* **Charge current.** 1.03 A is now a datasheet relation (I ≈ 164 / R(kΩ) A at R7 = 160 kΩ)
  rather than a silkscreen inference, but it has not been measured.
* **Battery connector family** — MX1.25 per the product page [S5], PH1.25 per the schematic
  footprint. Same pitch, different mating part; verify before ordering cables.
* **Whether this board's panel is byte-for-byte the sibling 1.69-inch Touch LCD Module's panel.**
  Inferred from matching active area, pixel pitch and outline; the R5.15 mm corner radius comes
  from the module's mechanical drawing [S13], not from this board's.
* **PCF85063A CLKOE (pin 3)** appears to terminate in an unconnected stub on the schematic — a
  floating input, contrary to the datasheet's requirement that inputs sit at a defined level.
  Confirm on the board; a floating input can cost quiescent current.
* **The ETA6096's VIN net is labelled `VSYS1`**, a net label that occurs exactly once in the
  schematic's text layer, drawn on the wire into U2 pin 1 alongside C3. Almost certainly `VSYS`
  (i.e. VBUS through D4). Verify with a meter if charge behaviour looks odd.

---

## Sources

| Tag | Artifact |
|---|---|
| **[S1]** | Board wiki — <https://www.waveshare.com/wiki/RP2350-Touch-LCD-1.69>. FAQ (PWR button = GPIO14, pad spacing 2.154 mm, the RP2350-E9 pointer), the generic `PICO_BOARD` CMake warning, the BOOTSEL procedure |
| **[S2]** | Board wiki raw wikitext — <https://www.waveshare.com/w/index.php?title=RP2350-Touch-LCD-1.69&action=raw>. Spec table: Touch Chip CST816T, Display Chip ST7789V2, Resolution 240(H)RGB × 280(V), Display Size 27.972 × 32.634 mm, Panel IPS, Pixel Pitch 0.11655 mm, IMU QMI8658 |
| **[S3]** | **Board schematic, primary** — <https://files.waveshare.com/wiki/RP2350-Touch-LCD-1.69/RP2350-Touch-LCD-1.69.pdf> (185,819 B, md5 `905f980ed9df9692c25f705c49573984`). U5 = RP2350A; U4 = W25Q128JVSIQ (only QSPI device → no PSRAM); U3 = PCF85063ATL + Y1 + BAT1; U6 = QMI8658C with SDO/SA0 to GND; U2 = ETA6096 + L1 2.2 µH + R7 160 kΩ; U1 = RT9193-33PB; buzzer via R22 4.7 k + T2 SS8050; backlight via R5 1 k → Q1 DMG1012T-7 with R1 10 Ω; BAT_ADC divider R11/R12; Q2/Q3 AO3401; FPC H1 pin list (no TE, no MISO); the GPIO↔net table; TP1–TP11; Key2→QSPI_SS_N, Key3→RUN; X1 12 MHz |
| **[S4]** | **Vendor basic demo, primary** — <https://files.waveshare.com/wiki/RP2350-Touch-LCD-1.69/RP2350-Touch-LCD-1.69-Code.zip> (md5 `68d081a5ea9a743aaf98e5a73032ff5f`). Pin map, `spi_init(spi1, 40000*1000)`, `i2c_init(i2c1, 400*1000)`, no DMA; LCD init sequence with MADCTL 0x00/0x78, COLMOD 0x05, FRCTRL2 0x13, PORCTRL 0x0B/0x0B, INVON, the +20 offset; touch register map at 0x15 with ChipID 0xA7 == 0xB5; `C/CMakeLists.txt` missing `PICO_BOARD` |
| **[S5]** | Product page — <https://www.waveshare.com/rp2350-touch-lcd-1.69.htm>. "MX1.25 battery header", rechargeable-only RTC battery, charge-while-run |
| **[S6]** | **Vendor LVGL demo, primary** — <https://files.waveshare.com/wiki/RP2350-Touch-LCD-1.69/RP2350-Touch-LCD-1.69-LVGL.zip>. Extended pin map (`PWR_KEY_PIN 14`, `BAT_PWR_PIN 15`, `BEEP_PIN 2`, `RTC_INT_PIN 18`, `DOF_INT1/2 23/24`, `BAT_ADC_PIN 29`, `Touch_INT_PIN 21`, `Touch_RST_PIN 22`, `PLL_SYS_KHZ 200000`); `set_sys_clock_khz(200000,true)` **plus** the explicit `clock_configure(clk_peri, …CLKSRC_PLL_SYS, 200e6, 200e6)`; `spi_init(spi1, 200*1000*1000)`; MADCTL 0xA0 landscape; QMI8658 at 0x6a/0x6b; PCF85063A at 0x51 |
| **[S7]** | MicroPython driver inside [S4] — pin constants, `width 240 / height 280`, `SPI(1, 100_000_000, polarity=0, phase=0, bits=8, …, miso=None)`, COLMOD 0x05, INVON, RASET +20, `address=0x15`, `I2C(id=1, freq=400_000)` |
| **[S8]** | **ST7789V2 datasheet, primary** — <https://files.waveshare.com/wiki/common/ST7789V2.pdf>. §1/§7.1 GRAM 240×320×18 bits; §9.1.32 COLMOD (3Ah) pixel-format table; §9.1.31 IDMON (39h); §9.1.30 IDMOFF (38h); §8.17.1 Power Level Definition; §9.1.24 PTLAR (30h); §9.1.27 TEON (35h); §9.2.18 FRCTRL2 (C6h) frame-rate table (0x13 → 53 Hz); AC characteristics **TSCYCW = 16 ns min**, TSCYCR = 150 ns min |
| **[S9]** | **RP2350 datasheet, primary** — <https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf>. Ch.1 feature list; Table 1 / §1.2.1.1 RP2350A = QFN-60, GPIO0–29, ADC on 26–29; §4.2 SRAM; §4.4 XIP cache; Bank 0 GPIO function table (QMI CS1n only on GPIO0/8/19/47; GPIO10 = SPI1 SCK, GPIO11 = SPI1 TX; GPIO6/7 = I2C1 SDA/SCL; GPIO25 = PWM4 B; GPIO2 = PWM1 A; GPIO12–19 HSTX-capable, output-only); ch.11 (3 PIO blocks); ch.12 (16 DMA channels); §12.4 (RP2040-E11 fixed) and §12.4.5 ("INL and DNL — Details to follow"); Table 1438 (ENOB 9 min / 9.5 typ, 500 kS/s, ADC input impedance min 100 kΩ); Appendix C hardware revision history (A2, A3; A3 fixes E3 and E9); Appendix E RP2350-E9 |
| **[S10]** | Sibling module wiki (same panel), independent corroboration — <https://www.waveshare.com/w/index.php?title=1.69inch_Touch_LCD_Module&action=raw>. "the internal RAM of the LCD is not fully used"; "due to the four round corners … some parts of the input images may not be displayed"; spec table says CST816S, prose says CST816D; FAQ: max 41.3 mA @ 3.3 V, max brightness 423 cd/m² |
| **[S11]** | CST816S datasheet — <https://files.waveshare.com/wiki/common/CST816S_Datasheet_EN.pdf>. "The 7-bit device address of the chip is generally 0x15"; self-capacitance sensing |
| **[S12]** | CST816S register declaration — <https://files.waveshare.com/wiki/common/CST816S_register_declaration.pdf>. `FingerNum @0x02` ("0: no finger 1: one finger"); `GestureID @0x01`: 0x00 none, 0x01 slide up, 0x02 slide down, 0x03 left, 0x04 right, 0x05 single click, 0x0B double click, 0x0C long press; XposH/L @0x03/0x04, YposH/L @0x05/0x06; ChipID @0xA7 |
| **[S13]** | Sibling module 2D drawing — <https://files.waveshare.com/wiki/1.69inch-Touch-LCD-Module/1.69inch_Touch_LCD_Module_2D_Drawing_V2.pdf>. Outline 41.13 ± 0.05 × 33.13 ± 0.05 (TP) with 4-R7.0 glass corners; viewing area 32.93 × 28.27 with **R5.15** corner radius; thickness 9.30 mm; 4 × M2.00 mounting holes |
| **[S14]** | GitHub REST API search — `api.github.com/search/repositories?q=RP2350-Touch-LCD-1.69` → `total_count: 0`; `?q=user:waveshareteam+rp2350` → 8 repos, none of them the 1.69. **There is no vendor GitHub repository for this board** |
| **[S15]** | pico-sdk `pico2.h` — <https://raw.githubusercontent.com/raspberrypi/pico-sdk/master/src/boards/include/boards/pico2.h>. `#define PICO_RP2350A 1`, `#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)`, `PICO_RP2350_A2_SUPPORTED 1` |
| **[RT]** | RT9193 datasheet — <https://www1.futureelectronics.com/doc/RICHTEK/RT9193-33PB.pdf> |
| **[ETA]** | ETA6096 datasheet v1.4 — <http://www.eta-semi.com/wp-content/uploads/2022/03/ETA6096_V1.4.pdf> |
| **[PCF]** | PCF85063A datasheet rev 7 — <https://files.waveshare.com/wiki/common/PCF85063A.pdf> |
| **[W25]** | W25Q128JV datasheet — <https://cdn.sparkfun.com/assets/5/b/2/a/6/W25Q128JV_Datasheet.pdf> |
| **[QMI]** | QMI8658C datasheet rev 0.6 — <https://files.waveshare.com/upload/5/5f/QMI8658C.pdf> |
