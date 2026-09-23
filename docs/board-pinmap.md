# M5Paper Color (SKU C151) — board pin map

**This whole file is the M5Paper Color's.** The project also builds for the Seeed reTerminal E1002
since 2026-09-15; that board's pin map is at the bottom, marked **read, not measured**, and the
authoritative copy is `src/board/reterminal_e1002/pins_reterminal_e1002.h` — a header, so it cannot
drift from what compiles.

Everything the digital-frame firmware needs to talk to, in one table. Compiled
2026-08-31 from three sources that agree with each other:

- `refs/M5Unified/src/M5Unified.cpp` — the board tables and the
  `board_M5PaperColor` cases. This is the authority for buttons, LED, audio and
  the SD bus; the numbers are literal `GPIO_NUM_*` constants, not inferred.
- `refs/M5Unified/src/utility/Power_Class.cpp:494` — the PM1 setup for this board.
- `refs/M5PaperColor-UserDemo/main/hal/hal.h` and `hal/storage/hal_storage.cpp` —
  the shipping application's own view of the same hardware.

Where a fact came from reading the schematic dump (`refs/_sch.txt`) rather than
from code it is marked as such — that dump is a PDF text extraction and its
net-to-pin association is not reliable.

## SoC GPIO

| Signal | GPIO | Notes | Source |
| --- | --- | --- | --- |
| SYS_SCL (internal I²C) | 2 | PM1, SHT40, RX8130, ES8311 | `M5Unified.cpp:110` |
| SYS_SDA (internal I²C) | 3 | | `M5Unified.cpp:110` |
| Grove SCL (external I²C) | 5 | Also the UserDemo's UART console TX | `M5Unified.cpp:110` |
| Grove SDA (external I²C) | 4 | Also the UserDemo's UART console RX | `M5Unified.cpp:110` |
| SPI SCLK | 15 | SPI2_HOST, shared | `M5Unified.cpp:215`, `epd_el040ef1.h:24` |
| SPI MOSI | 13 | | same |
| SPI MISO | 14 | | same |
| EPD CS | 44 | | `epd_el040ef1.h:28` |
| EPD DC | 43 | | `epd_el040ef1.h:27` |
| EPD RST | 12 | | `epd_el040ef1.h:26` |
| EPD BUSY | 11 | **BUSY_N: low = busy, high = idle** | `epd_el040ef1.h:25` |
| microSD CS | 47 | Same SPI2 bus as the panel | `M5Unified.cpp:215`, `hal_storage.cpp:82` |
| Button A | 10 | Active low, external pull-up | `M5Unified.cpp:3550-3555` |
| Button B | 9 | Active low | same |
| Button C | 1 | Active low | same |
| RGB LED data | 21 | WS2812B-4020, **2 LEDs**, driven over RMT | `M5Unified.cpp:250`, `:2217` |
| RTC IRQ (RX8130 → SoC) | 7 | `_rtcIntPin` | `Power_Class.cpp:495` |
| I²S MCLK | 42 | Shared by speaker (I2S0) and mic (I2S1) | `M5Unified.cpp:2967`, `:2608` |
| I²S BCK | 40 | | same |
| I²S WS / LRCK | 41 | | same |
| I²S DOUT → speaker | 38 | | `M5Unified.cpp:2971` |
| I²S DIN ← mic | 39 | | `M5Unified.cpp:2617` |
| Codec enable | 45 | Drive high before touching the ES8311 | `M5Unified.cpp:637` |
| Speaker PA enable | 46 | | `M5Unified.cpp:638` |

Buttons are read inverted (`!gpio_in(pin)`), so they are active-low against the
`BTN_PU` pull-up seen in the schematic.

## I²C devices (internal bus, SCL 2 / SDA 3)

Scanned on hardware 2026-09-01 (`.scratch/digital-frame/issues/17`):

| Device | 7-bit address | Answered? | Purpose |
| --- | --- | --- | --- |
| M5PM1 | `0x6E` | yes | PMIC, power rails, board GPIO expander, shutdown |
| SHT40 | `0x44` | yes | Ambient temperature / humidity |
| RX8130CE | `0x32` | yes | Calendar RTC, alarm wake, 4 bytes of battery-backed RAM at `0x20`–`0x23` |
| ES8311 | `0x18` | **no** | Audio codec |

**The RX8130 is at `0x32`.** The datasheet's quoted "0x32" is the 7-bit address as
written, not an 8-bit write byte that would have implied `0x19`. Settled by scan; the
ambiguity is closed. The guessed register map (time registers from `0x10`, BCD) is
correct too — it read back a plausible date consistent with the shipping firmware's
first-boot value plus uptime.

**The ES8311 not answering is expected, not a fault.** The codec is held in reset until
GPIO45 is driven high (`M5Unified.cpp:637`). Raise that pin before concluding it is
absent.

## M5PM1 GPIO

The PM1 has its own GPIO block; these are **not** SoC pins.

| PM1 GPIO | Function | Source | Confidence |
| --- | --- | --- | --- |
| 0 | EPD power (`PY_EPD_EN` → `EPD_3V3_L3B`) | `M5GFX.cpp:2244-2247`, `hal.h:136` | Confirmed by two sources |
| 1 | `CARD_DEC` / `SD_DEC` — card detect, **input, pulled up, LOW = card present** | `hal.h:135`, `hal.cpp:181-183`, `:481-483` | Confirmed against a fitted card 2026-09-04 |
| 2 | RTC wake line | `hal.h:149` | UserDemo only |
| 3 | microSD power (`PY_SD_PWR_EN` → `TF_3V3_L3B`) | `Power_Class.cpp:499-502` | Confirmed |
| 4 | `PY_SD_DET_EN` — card-detect enable, drive **HIGH** to arm GPIO1 | `hal.h:137`, `hal.cpp:181-182` | Arming requirement observed on hardware |
| LDO | RGB LED power | `Power_Class.cpp:497` | **The LED stays dark until this is on** |

PM1 registers used: `0x07` GPIO power hold, `0x09` I²C idle-sleep, `0x0A` watchdog,
`0x10` direction, `0x11` output level, `0x12` **input level** (read-only, bits [4:0] =
GPIO4–0), `0x13` drive (`0` = push-pull; the reset value `0x1F` is all open-drain),
`0x14`/`0x15` pull-up/pull-down, `0x16`/`0x17` pin function (2 bits per pin, must be
`00` for plain GPIO). Write order matters — see `board_pm1.h`.

> **The two-bit registers are split and it is not symmetric.** `0x16` covers GPIO0–3
> and `0x17` covers GPIO4 alone in bits [1:0]; likewise `0x14` and `0x15`
> (`refs/_pm1.txt:350-372`). Indexing GPIO4 into `0x16` does not fail — it silently
> reconfigures GPIO1, which is the card-detect line.

**Card detect, observed both ways** (`.scratch/digital-frame/issues/08`): cardless on
2026-09-02, with a 2 GB card fitted on 2026-09-04. Reading `GPIO_IN` under four
conditions:

| SD power (G3) | DET_EN (G4) | `GPIO_IN` cardless | with card | GPIO1 |
| --- | --- | --- | --- | --- |
| off | low | `0x05` | `0x05` | 0 → 0 |
| off | high | `0x17` | `0x15` | 1 → **0** |
| on | low | `0x0D` | `0x0D` | 0 → 0 |
| on | high | `0x1F` | `0x1D` | 1 → **0** |

Only bit 1 moves, and only in the two conditions where the detect is armed, so
**"low = present" is confirmed** and so is the arming: with GPIO4 low, GPIO1 reads low
whether or not a card is in, which under "low = present" is a false positive. GPIO1
tracks GPIO4 exactly and is independent of the card rail. The firmware still treats
detect as advisory and lets the mount attempt be authoritative, because falling back is
correct either way.

> **Corrects `docs/research/reference-source-review.md:424`** only in emphasis, not
> in fact: that line's "GPIO0 = EPD power, GPIO3 = SD power" is right, and
> `Power_Class.cpp:499-502` independently confirms the SD half. What the research
> document does not mention is PM1 GPIO1, GPIO2 and GPIO4, or that the RGB LED
> hangs off the PM1's LDO.

## Bus sharing

SPI2_HOST carries both the panel (CS 44) and the microSD (CS 47). The measurement
harness sidestepped this by never mounting the card; the digital-frame firmware
cannot. Since ticket `03` the bus belongs to `src/board/m5papercolor/board_spi_m5.c` rather than to
`epd_init()`, and both devices join it there.

Three things about sharing this bus that were not obvious:

- **It was an undriven input in every build before ticket 03.** Nothing in `src/`
  configured it. A fitted card on a floating chip select is free to read the panel's
  240 ms frame transfer as its own traffic. `board_spi_init()` now parks it high.
- **A refresh holds the panel's CS low for the whole 15.6 s**, not just for the
  transfer, so the exclusion has to cover the entire operation. It is two layers:
  `spi_device_acquire_bus()` inside the driver, which is what actually stops the SPI
  peripheral interleaving an SD transaction issued from FATFS, and a mutex above it
  so application-level accesses queue at a named place. Measured: a guarded read
  issued 1.5 s into a refresh waits 14.1 s on internal flash, against 15 ms with no
  refresh running — and 13.61 s on a fitted card, where the **unguarded** read waits
  exactly as long (2026-09-04, ticket `08`). That last case is the one that proves
  `spi_device_acquire_bus()` is doing the work; the mutex alone could not have.
- **The panel does not release SI0 (= MOSI) when its CS goes high.** A temperature read
  therefore leaves the card unable to answer any sdspi command until the panel is reset,
  which is why `epd_read_temperature()` ends in `epd_reset()`. Tickets `08` and `18`.

The internal I²C bus carries four devices including the PMIC that can cut power to
the panel. `board_i2c_scan()` exists to make "the bus is healthy" an observation
rather than an inference.

## Seeed reTerminal E1002 — FROM THE SCHEMATIC SINCE 2026-09-17

**A board arrived on 2026-09-17 and Seeed's own V1.2 schematic is now the source**, which replaces
the previous status of this section: it was transcribed from `refs/esp32-photoframe` (MIT, HEAD
`bf02982`), a third party's firmware that ships an E1002 build. The schematic is
`202004321_reTerminal_E1002_V1_2_SCH_251120.pdf`, downloaded from the Seeed wiki to
`.scratch/reterminal-e1002/`; its net labels are readable as text (`pdftext.py` beside it, because
there is no poppler and no PyPDF on this machine). **It agreed with every pin the reference names
and it names five the reference does not** — the rows below say which is which.

The panel and the timings are still not measured; ticket `63` is where those become facts. The
header `src/board/reterminal_e1002/pins_reterminal_e1002.h` is what the build actually uses.

Measured on the unit with `esptool flash_id`, 2026-09-17: **ESP32-S3 (QFN56) rev v0.2, 8 MB
embedded PSRAM (octal, matching `CONFIG_SPIRAM_MODE_OCT`), 32 MB flash.**

| Signal | GPIO | Schematic net | Notes |
| --- | --- | --- | --- |
| SPI2 SCLK / MOSI / MISO | 7 / 9 / 8 | `SCK` / `MOSI` / `MISO` | shared by panel and microSD, as on the M5Paper Color |
| EPD CS / DC / RST / BUSY | 10 / 11 / 12 / 13 | `SCREEN_CS#` / `SCREEN_DC#` / `SCREEN_RST#` / `SCREEN_BUSY#` | clocked at **20 MHz** in the reference, against 4 MHz here |
| microSD CS | 14 | `SD_CS` | driven high before `spi_bus_initialize()`, same discipline as ticket `03` |
| microSD power | 16 | `SD_EN` | **500 ms** settle in the reference, against this board's 50 ms guess |
| **microSD detect** | **15** | **`SD_DET`** | **the reference's header omits it and the PCB has it.** See the corrected bullet below |
| I²C0 SDA / SCL | 19 / 20 | `I2C0_SDA` / `I2C0_SCL` | SHT4x at `0x44`, PCF8563 RTC at `0x51` |
| I²C1 SDA / SCL | 39 / 40 | `I2C1_SDA` / `I2C1_SCL` | SY6974B charger at `0x6B` — **V1.2 and later only** |
| Buttons | 3 / 4 / 5 | `KEY0` / `KEY1` / `KEY2` | the header maps 3 = green (wake), 5 = left, 4 = right. **The schematic gives the net names, not which key cap is which**, so that mapping is still the reference's |
| LED | 6 | `LED` | one monochrome LED, **active low**, plain GPIO |
| Battery ADC / enable | 1 / 21 | `VBAT_ADC` / `VBAT_EN` | ADC1_CH0 behind a 2:1 divider, gated by 21. **FIXED 2026-09-21, ticket `73`** — this path returned a plausible wrong answer in **11-13 % of all reads** (a continuum from 172 to 3994 mV off a cell at 4130, each turned into 0 % by `battery_percent_from_mv()`). Neither candidate on file was the cause: `pdMS_TO_TICKS(BOARD_BAT_SETTLE_MS)` with the constant at 10 is **one tick** at `CONFIG_FREERTOS_HZ=100`, so the settle was 0-10 ms; `+ 1` tick fixes it. **The node is measured to settle in ~2 ms**, the only settle time on this board that is a measurement — so 10 ms is 5x margin and the constant was never the problem. The missing mutex shipped as a fix anyway (§4 A1) and has still never been seen to fire. The median-of-several rule is lifted for firmware from 2026-09-21 |
| **Buzzer** | **45** | **`BUZZER_EN`** | an MLT-8530 magnetic buzzer. The wiki says it beeps on a manual refresh. **Driven since 2026-09-22** by `board_audio_e1002.c` (LEDC square wave near its ~2.7 kHz resonance; boot chime and press tones, ticket `16`). The idle-LOW polarity is not in the net list; it was checked by ear and touch on the unit, with no idle whine and no heat |
| **Microphone** | **38 / 41 / 42** | **`PDM_EN` / `PDM_DATA` / `PDM_CLK`** | an MSM261DHP006 PDM microphone. Unused here; the wiki's "voice input" is a future feature of Seeed's own firmware |
| **Touch** | **47 / 48** | **`TOUCH_INT` / `TOUCH_RES`** | net names only — whether this unit's glass is actually touch-fitted is not established |
| **UART1** | **17 / 18** | **`TX1` / `RX1`** | free, and the equivalent of the M5Paper Color's Grove trace port |
| spare ADC | 2 | `ADC1_CH4` | unused |

**Four differences that are not pin numbers**, and each one has a consequence in the code:

- **No power-manager IC.** No PM1, so no mediated rails: the panel has no switchable rail at all
  (`board_epd_power()` is a no-op that returns `ESP_OK`, and the reference's own board file sets
  `.pin_enable = -1` where its E1003 sibling names an enable pin), the card's rail is a plain GPIO,
  and battery voltage comes from the SoC's ADC rather than a register.
- **There IS a card-detect line, and this file said there was not.** `ESP_IO15/SD_DET` is on the
  V1.2 schematic. The claim came from `refs/esp32-photoframe`'s board header, which names every
  other pin and omits this one — so it was an absence of evidence read as evidence of absence, and
  the third party simply does not use the line. `board_card_present()` still returns `true`
  unconditionally, which is now a **gap rather than a hardware limit**; wiring it up needs the
  polarity, which needs one reading with a card fitted (`env:bringup_e1002` prints it) and one
  without, and the second needs a hand at the slot. Ticket `63` arm 5 was written on the old
  premise and says so.
- **No battery-backed RTC RAM.** The PCF8563 has no general-purpose RAM, so `board_state_*()` is
  NVS-backed here. Ticket `43` seam 4.
- **The hardware revision matters and cannot be chosen at purchase** (operator, 2026-09-15). V1.2
  and later carry the I²C SY6974B; earlier units carry a non-I²C ETA6003 and cannot report external
  power or charge state. `board_i2c_init()` **probes** `0x6B` on I²C1 and prints which it found, so
  the boot log answers it — check each unit on arrival rather than assuming. A pre-V1.2 unit costs
  external-power detection and nothing else.

**Flash is 32 MB** against the M5Paper Color's 16 MB, which is why `partitions_e1002.csv` exists;
PSRAM is 8 MB octal on both, and `esptool` reports both figures off the unit.

**And the console is not where this project assumes it is.** The USB-C goes to a **CH340C**, whose
`RXD`/`TXD` are wired to the SoC's `U0RXD`/`U0TXD` and whose `DTR#`/`RTS#` drive the reset-and-boot
pair (schematic sheet 6, "USB TO UART / RST & BOOT / USER KEY"). Two consequences, and the first
has already cost a session elsewhere:

- **`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` prints nothing on this board.** `sdkconfig.defaults`
  forces it for the M5Paper Color, whose console really is native, so every E1002 env has to
  override it in its own generated `sdkconfig.<env>` — `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` with
  `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`. A board built without that override runs
  perfectly and looks dead.
- **`esptool`'s automatic reset works here** (`Hard resetting via RTS pin...`, measured
  2026-09-17), which the M5Paper Color's recovery ladder cannot rely on. But **921600 baud
  corrupts a read**: `read_flash` at that rate died with `Corrupt data, expected 0x1000 bytes but
  received 0xfa0 bytes` about 24 KB in, twice the M5Paper Color's habitual rate working fine over
  its native peripheral. 460800 completes. Do not carry a baud rate across boards either.

Sheet 6 also carries `K1`, `K4` and `K5` (`TC-3X4X2.5` tact switches) beside the `RST & BOOT` and
`USER KEY` labels, so BOOT and RESET buttons exist on the PCB. Whether any of them is reachable
without opening the case is not established, and with RTS reset working it has not needed to be.

## Charging, and why only one board can control it

Read this before proposing any battery-protection work. **The asymmetry is the whole content: the
board that needs a charge cap cannot have one, and the board that can have one needs it less.** The
account and the schematic readings are ticket `71`; the crops are `.scratch/chg/`.

| | M5Paper Color | reTerminal E1002 V1.2 |
| --- | --- | --- |
| Charger | **IP2315** (U3), Type-C PD in, "PD Only" | **SY6974B** (U2) at `0x6B` on I²C1 |
| Enable/disable from software | **NO.** PM1 pin 2 `CHG_EN_PP` carries a no-connect cross and no net; the charger's own `SCL`/`SDA` (pins 8/9) are crossed out too | **YES**, `REG01` bit 4 — **and I²C only.** `/CE` (pin 9) is strapped on by R7 10K to GND; R8 and R9 are not fitted, so **no GPIO reaches it** |
| Charge current | **Resistor-fixed:** `ICHGSET` (27) → **R13 22K** → GND | **Register**, `REG02` mask `0x3F`. Sheet is annotated `I=1000/R=500mA`, and the pack is 2000 mAh, so **0.25 C** |
| Termination voltage | **Not settable** | `REG04` bits [7:3], base 3856 mV, step 32 mV |
| Charge status to the SoC | **NONE.** `LED3` (10) → **R14, not fitted**; `HLED` (16) → R11 NC → `LED1` NC | `REG08`, already read by `board_power_read()`; `STAT` (4) also drives D1 (red) via R15 10K |
| Battery connector | **J2 `CON2_SMD`, 2 pins** — `VBAT`, `GND`. **No thermistor pin exists** | **J7 `PH-3A-WT`, 3 pins** — 3 `V_BAT`, **2 `BAT_NTC`**, 1 `GND` |
| Cell thermal protection | **NONE.** `NTC` (26) → R12 51K → GND, a fixed resistor | **REAL.** `BAT_NTC` reaches `NTC` (11), biased by R106 3.3K / R107 4.7K / R113 51K / R2 1K off `REGN`, annotated **"0°–45° NTC probe hole position"** |
| Pass-through (power path) | **NO.** `VBAT` --**R20, 0R/1%**-- `SYS_VBUS`, and `SYS_VBUS` feeds the `JW5712` buck (U6, EN from `PY_MPWR_EN`) → `3V3_L2` | **YES.** `SYS1`/`SYS2` (15/16) are separate nodes from `BAT1`/`BAT2` (13/14); SYS → `VSYS` → `SYS_3V3` |

**Three traps in that table, each of which has already misled a reading:**

- **The M5's block diagram and sheet title say the opposite of the M5's detail sheet.** Page 1 reads
  `VUSB_IN → AW32901 → IP2316 → SYS_VBUS` and the charge sheet is titled **"CHARG PowerPath"**. Both
  are true at the block level *because the charger drives VBAT* — and both invite "the load is fed
  from the input", which R20 refutes. **The detail sheet wins.**
- **`PWR_CFG` (PM1 `0x06`) bit 0 `CHG_EN` exists, is R/W, and does nothing here.** The bit is real,
  the datasheet documents it (`refs/_pm1.txt:746`), the shipping firmware writes it
  (`refs/M5PaperColor-UserDemo/main/hal/hal.cpp:186` calls `pm1.setChargeEnable(true)`) and the write
  succeeds. The pin it gates is unconnected. **A green `setChargeEnable()` is not evidence.**
  M5Unified agrees and says so in code: `Power_Class.cpp:2393-2396` returns early for
  `board_M5PaperColor` under the comment *"M5PaperColor does not support charge control"*.
- **The M5's absence of `0x75` from every I²C scan is corroboration, not the proof.** The same
  IP2315 *is* reached over I²C at `0x75` on the M5Paper **Mono** (`Power_Class.cpp:72`), where an IO
  expander gates it first — so a scan there would also read empty. The crosses on pins 8/9 are the
  evidence; this board has no IO expander for there to be a gate on.

**The E1002's charger has an I²C watchdog and it restores register defaults — measured, not read.**
`REG05` boots at **0x9F**. With a lowered `REG04` in place and nothing handling the watchdog, the
register was observed reverting to its default and the cell charging back up between re-asserts
(ticket `71` §10.3: two corrections in about two minutes, `vbat` 4020 → 4132 mV). Clearing `REG05`'s
`[5:4]` — the single-bit change **0x9F → 0x8F** — stops it, verified by read-back and by the
correction counter staying at zero. **The bit positions are the BQ2429x layout applied to this part,
so the mechanism is inferred while the effect is measured**; what makes the write safe either way is
that it leaves `EN_TERM` (bit 7) and the safety timer (bit 3) alone. The period is *not* measured —
two corrections in two minutes is consistent with the 40 s that `[5:4]` = 01 would mean, and nobody
timed it. **Anything that writes a charger register here must re-read it**, which is why
`board_charger_set_vreg_mv()` reports what read back rather than what it wrote.

**`REG04` decodes exactly, and that is what confirmed the encoding.** 0x58 on a factory unit →
3856 + 11×32 = **4208 mV**, the canonical lithium termination voltage to the millivolt. The base and
step came from a third-party driver rather than a datasheet; landing precisely on 4.2 V is not a
coincidence.

**And one register on the M5 that IS writable and does matter:** `BATT_LVP` (PM1 `0x08`, R/W,
default `0x40` = **2.50 V**), the forced-power-off threshold, against this project's own curve calling
**3.2 V** empty. `0xA6` = 3.30 V. It is PM1 RAM, so it resets when the cell is removed and wants
writing every boot, and per the datasheet 5VIN insertion recovers a board set too high — **read, not
measured**, so raise it only with the operator present. Ticket `71` §7.5.
