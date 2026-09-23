// Seeed Studio reTerminal E1002 wiring.
//
// EVERY PIN BELOW IS CONFIRMED AGAINST SEEED'S OWN V1.2 SCHEMATIC (2026-09-17). It was
// transcribed from refs/esp32-photoframe (a third party's MIT firmware, HEAD bf02982) and the
// schematic's net labels agree with all of it -- see docs/board-pinmap.md, which also lists the
// five nets the reference does not name. The schematic itself is in .scratch/reterminal-e1002/.
//
// Two things here are still NOT measured, and neither is a pin: the 20 MHz panel clock and the
// 500 ms card-rail settle are both the reference's choices, and ticket 63 is where they get
// checked against hardware.
//
// Reached through board.h; never include it directly.

#ifndef PINS_RETERMINAL_E1002_H
#define PINS_RETERMINAL_E1002_H

#define BOARD_NAME "reterminal_e1002"

// ------------------------------------------------------------------ I2C buses
//
// TWO of them, unlike the M5Paper Color's one. I2C0 carries the SHT4x and a PCF8563 RTC; I2C1
// carries the charger and exists separately because the charger is only fitted from hardware
// revision V1.2. See board_e1002.c on why that revision is probed and not assumed.
#define BOARD_I2C_SDA 19
#define BOARD_I2C_SCL 20
#define BOARD_I2C_FREQ_HZ 100000

#define BOARD_CHARGER_I2C_PORT 1
#define BOARD_CHARGER_I2C_SDA 39
#define BOARD_CHARGER_I2C_SCL 40
#define SY6974B_I2C_ADDR 0x6B

// REG08 (STAT0): bits [7:5] BUS_STAT, [4:3] CHRG_STAT, [2] PG_STAT (power good).
//
// **THESE BIT POSITIONS ARE UNVERIFIED AGAINST HARDWARE AND THREE SOURCES DISAGREE** (ticket 71
// §9.2): this project and refs/esp32-photoframe both say CHRG_STAT is [4:3], the only public
// SY6974 driver on GitHub says [1:0], and the BQ2429x family this part clones puts it at [5:4].
// No datasheet for the part is on this disk. board_charger_read() therefore reports the raw
// bytes alongside the decode, so an analysis can disagree with this header without a reflash.
#define SY6974B_REG08_STAT0 0x08
#define SY6974B_STAT0_PG_BIT (1u << 2)
#define SY6974B_CHRG_STAT_SHIFT 3
#define SY6974B_CHRG_STAT_MASK 0x03

// The registers a charge cap needs, and the whole block this board dumps.
//
// REG04[7:3] is the charge (termination) voltage. **The encoding below is from a third-party
// ESPHome component, not a datasheet** -- base 3856 mV, step 32 mV -- and the one field that
// could be cross-checked (REG01 bit 4, charge enable) agrees across three independent sources,
// which is why it is used at all. The check that matters happens at run time: the value read
// before any write must decode to something near a cell's normal 4.2 V, or the encoding is
// wrong and nothing may be written.
#define SY6974B_REG01_PWRON 0x01
#define SY6974B_REG01_CHG_CONFIG_BIT (1u << 4)
#define SY6974B_REG01_OTG_CONFIG_BIT (1u << 5)
#define SY6974B_REG04_VREG 0x04
#define SY6974B_REG04_VREG_MASK 0xF8u
#define SY6974B_REG04_VREG_SHIFT 3
#define SY6974B_VREG_BASE_MV 3856u
#define SY6974B_VREG_STEP_MV 32u
#define SY6974B_REG_COUNT 12  // REG00..REG0B

// REG05's I2C watchdog. **MEASURED ON HARDWARE 2026-09-20, not read from a datasheet.** This unit
// boots with REG05 = 0x9F, and with a lowered REG04 in place the register was observed reverting to
// its default repeatedly (app_charge's `corrections` counter, ticket 71) -- so the watchdog is real
// and restoring defaults is what it does. Under the BQ2429x layout this part clones, bits [5:4] are
// the watchdog period and 01 is 40 s, which matches both the byte and the observed behaviour.
//
// Clearing those two bits is a ONE-BIT change here (0x9F -> 0x8F) and leaves EN_TERM (bit 7) and
// the safety timer (bit 3) alone, so the failure mode if the layout guess is wrong is a bit that
// does nothing rather than a charger with its protections off. It is verified by read-back and by
// `corrections` staying at zero afterwards, which is the only claim this project will make about it.
#define SY6974B_REG05_TIMER 0x05
#define SY6974B_REG05_WATCHDOG_MASK 0x30u

#define SHT40_I2C_ADDR 0x44
#define PCF8563_I2C_ADDR 0x51

// -------------------------------------------------------------------- SPI2 bus
//
// Shared by the panel and the microSD, exactly as on the M5Paper Color -- the reference board
// file drives BOTH chip selects high before spi_bus_initialize(). So ticket 43's seam 5 comes
// across intact: board_storage.c's lock still has a bus to take, and s_lock_took_bus is not
// dead weight here.
#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_SPI_PIN_MOSI 9
#define BOARD_SPI_PIN_MISO 8
#define BOARD_SPI_PIN_SCLK 7
#define BOARD_SPI_PIN_SD_CS 14
#define BOARD_SPI_MAX_XFER 32768

// -------------------------------------------------------------- the panel's pins
#define BOARD_EPD_PIN_BUSY 13
#define BOARD_EPD_PIN_RST 12
#define BOARD_EPD_PIN_DC 11
#define BOARD_EPD_PIN_CS 10

// The reference clocks this panel at 20 MHz against the M5Paper Color's 4 MHz. Both are that
// firmware's choice rather than a measured optimum here, and research §3 caps the total win
// from a faster panel clock at ~190 ms of a 15 s refresh -- so this is transcribed, not tuned,
// and ticket 63 should confirm a frame transfers cleanly at it before anyone reads anything
// into the difference.
#define BOARD_EPD_SPI_FREQ_HZ 20000000

// --------------------------------------------------------------- buttons and LED
//
// The board's own labels are green / left / right; board_buttons.h's roles are TOP / UP / DOWN.
// THE MAPPING IS A DECISION, not a transcription: green is the wake button and the one on its
// own, so it takes TOP's special action (orientation, and the 5 s access-point hold), and left
// and right become UP and DOWN. Left = UP = previous reads correctly for a row of pictures.
#define BOARD_BTN_TOP_GPIO 3  // green, also the deep-sleep wake pin
#define BOARD_BTN_UP_GPIO 5   // left
#define BOARD_BTN_DOWN_GPIO 4 // right

// One monochrome LED, ACTIVE LOW, on a plain GPIO -- not the M5Paper Color's two WS2812B over
// RMT. board_led.c's patterns survive; its colour coding does not. See board_led_e1002.c.
#define BOARD_LED_GPIO 6
#define BOARD_LED_ACTIVE_LOW 1

// An MLT-8530 magnetic buzzer on ESP_IO45/BUZZER_EN (V1.2 schematic). It is a transducer and not
// a self-oscillating buzzer, so it wants a square wave, and it is loudest near its ~2.7 kHz
// resonance. GPIO45 is a strapping pin; board_audio_e1002.c leaves it alone until after boot.
#define BOARD_BUZZER_GPIO 45

// ------------------------------------------------------------------ battery ADC
//
// GPIO1 = ADC1_CH0 behind a 2:1 divider (100k/100k), gated by GPIO21 so the divider does not
// drain the battery when nothing is reading it. 12 dB attenuation, eFuse calibration, 8 samples
// averaged -- all from the reference's battery_adc.c.
#define BOARD_BAT_ADC_UNIT ADC_UNIT_1
#define BOARD_BAT_ADC_CHANNEL ADC_CHANNEL_0
#define BOARD_BAT_EN_GPIO 21
#define BOARD_BAT_DIVIDER 2.0f
// MEASURED, 2026-09-21 (ticket 73), where the other settle times in this file are guesses: the
// divider node is within 1 LSB of its final value by ~2 ms (mean 4131.4 mV over 20 reads that
// settled 2-3 ms, against 4118.3 mV over 21 that settled 1-2 ms, on a cell at 4132 mV). So 10 ms
// carries 5x margin and does not want raising. **What DID want fixing is the delay call** --
// pdMS_TO_TICKS(10) is one tick at CONFIG_FREERTOS_HZ=100, and a one-tick vTaskDelay() is 0-10 ms;
// see battery_mv() in board_e1002.c, which adds the tick this number always assumed.
#define BOARD_BAT_SETTLE_MS 10
#define BOARD_BAT_SAMPLES 8

// ------------------------------------------------------------------ microSD rail
//
// GPIO16, and the reference waits 500 MS after raising it -- ten times the M5Paper Color's 50 ms
// guess. Both are somebody's choice rather than a measurement; this one is at least a choice
// made against real hardware, so it is kept as written.
#define BOARD_CARD_POWER_GPIO 16
#define BOARD_CARD_POWER_SETTLE_MS 500

// THERE IS A CARD-DETECT LINE: ESP_IO15/SD_DET on the V1.2 schematic. The reference's board header
// omits it, which is why this file and board_card_present() were both written as though the board
// had none. No define yet, deliberately: using it needs the polarity, and that needs a reading with
// a card fitted (env:bringup_e1002 prints one) AND a reading without, which needs a hand at the
// slot. Until both exist, board_card_present() keeps answering "a card may be fitted".

// -------------------------------------------------------------- persistent state
//
// FOUR BYTES, matching the M5Paper Color's, but backed by NVS rather than RTC RAM: the PCF8563
// has no general-purpose RAM. board.h's board_state_*() explains the cost, and ticket 39's
// finding is the input -- NVS commits per key, so this is ~288 single-key commits a day at the
// shipping navigation rate, not 288 whole-struct writes.
#define BOARD_STATE_BYTES 4

#endif // PINS_RETERMINAL_E1002_H
