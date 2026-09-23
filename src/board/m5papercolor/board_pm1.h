// M5PM1 power manager and the shared I2C bus.
//
// The EPD rail (EPD_3V3_L3B) sits behind the PM1; the panel is dead until GPIO0 is
// raised. M5GFX does this itself during board autodetect (M5GFX.cpp:2244-2247), which
// is why code built on M5GFX never has to think about it and we do.
//
// Written as direct register pokes rather than pulling in the m5stack/m5pm1 registry
// component: it is four writes, and an instrument benefits from having no moving
// parts it does not control.

// M5PAPER-COLOR-PRIVATE. What survives here is the PM1 part itself -- its register map and
// its GPIO block. Everything that the layers above ask for by a board-neutral name
// (board_i2c_init, board_power_read, board_epd_power, board_card_*, board_state_*,
// board_rtc_*, board_sht40_read) is declared in board.h and implemented in board_pm1.c.
// Nothing outside src/board/m5papercolor/ should include this file; `git grep 'PM1_\|RX8130_'`
// outside that directory is the check, and it must come back empty.

#ifndef BOARD_PM1_H
#define BOARD_PM1_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "board.h" // the interface this board implements, and its pin header

#define PM1_I2C_ADDR 0x6E
#define SHT40_I2C_ADDR 0x44

// PM1 registers, confirmed against M5PM1_Datasheet_EN.pdf (research §12.3) and
// refs/_pm1.txt:340-380, 850-900.
#define PM1_REG_PWR_SRC 0x04         // [2:0] bit0 5VIN, bit1 5VINOUT, bit2 BAT
#define PM1_REG_GPIO_POWER_HOLD 0x07 // bit0: retain GPIO0 state after power-off
#define PM1_REG_VBAT_L 0x22          // battery mV, 2 bytes little-endian
#define PM1_REG_VIN_L 0x24           // input mV, 2 bytes little-endian
#define PM1_REG_GPIO_MODE 0x10       // [4:0] direction, 1 = output
#define PM1_REG_GPIO_OUT 0x11        // [4:0] output level
#define PM1_REG_GPIO_IN 0x12         // [4:0] input level, read-only
#define PM1_REG_GPIO_DRV 0x13        // [4:0] 0 = push-pull, 1 = open-drain
#define PM1_REG_GPIO_PUPD0 0x14      // 2 bits per pin, GPIO0-3
#define PM1_REG_GPIO_PUPD1 0x15      // 2 bits, GPIO4 in [1:0]
#define PM1_REG_GPIO_FUNC0 0x16      // 2 bits per pin, GPIO0-3; 00 = plain GPIO
#define PM1_REG_GPIO_FUNC1 0x17      // 2 bits, GPIO4 in [1:0]

// The PM1's GPIO block. GPIO0 and GPIO3 are confirmed by two sources each; GPIO1 and
// GPIO4 are transcribed from the UserDemo only -- see the note on
// board_pm1_gpio_read() and docs/board-pinmap.md.
#define PM1_GPIO_EPD_POWER 0 // PY_EPD_EN  -> EPD_3V3_L3B
#define PM1_GPIO_SD_DETECT 1 // CARD_DEC / SD_DEC, input, LOW = card present
#define PM1_GPIO_SD_POWER 3  // PY_SD_PWR_EN -> TF_3V3_L3B
#define PM1_GPIO_SD_DET_EN 4 // PY_SD_DET_EN, drive HIGH to arm the detect

typedef enum {
    PM1_PULL_NONE = 0,
    PM1_PULL_UP = 1,
    PM1_PULL_DOWN = 2,
} pm1_pull_t;

// ------------------------------------------------------------------ power off
//
// SYS_CMD. Writing 0xA1 here powers the board off -- and nothing brings it back
// except a finger on the power button. VBUS does not boot this device: esptool
// connects to, flashes and dumps a board that is switched off, the console stays
// silent, and E Ink holds its last image with no power at all, so every available
// signal says "alive" (issues/17). There is no software recovery, local or remote:
// HOLD_CFG (0x07) has hold bits for 5VIN/OUT, the LDO and GPIO0-4, but none for
// DCDC_3V3, the SoC's own rail (refs/_pm1.txt:747-770, :1216).
//
// So while BOARD_NO_POWER_OFF is defined -- it is, for every device env in
// platformio.ini -- board_pm1_write_reg() refuses this register outright. Tickets 15
// (low-power cycle) and 16 (first boot, factory reset) are the two that legitimately
// need it, and they remove the flag deliberately, in one place, with someone standing
// next to the board.
#define PM1_REG_SYS_CMD 0x0C
#define PM1_SYS_CMD_SHUTDOWN 0xA1

// Returns ESP_ERR_NOT_SUPPORTED for PM1_REG_SYS_CMD while BOARD_NO_POWER_OFF is set.
// Every other register is written verbatim.
esp_err_t board_pm1_write_reg(uint8_t reg, uint8_t value);
esp_err_t board_pm1_read_reg(uint8_t reg, uint8_t *value);

// The shared I2C bus, for another device on it that this board's own files talk to -- the ES8311
// in board_audio_m5.c, which adds a short-lived device handle per use the way the RTC code does.
// NULL before board_i2c_init().
i2c_master_bus_handle_t board_pm1_bus(void);

// ---------------------------------------------------------------- power status
//
// board_power_read() (board.h) is this board's implementation of the interface, and its
// board_power_t fields are PM1 concepts that were generalised rather than invented:
//
// `vin_present` is the one that matters for an unattended run. The PM1 forcibly
// powers the system off when the battery falls below BATT_LVP (0x08, default 0x40 =
// 2.0 V + 64 x 7.81 mV = 2.50 V) -- but only "when neither 5VIN nor 5VINOUT is
// inserted" (refs/_pm1.txt:1239-1240). On USB power that cannot fire. On battery, a
// long run has a deadline nobody can see.
//
// That particular power-off does recover on its own: 5VIN insertion is listed as a
// power-on condition (:777-780). A commanded power-off does not.
// The mapping onto PWR_SRC (0x04): vin_present is bit0 (5VIN valid), vinout_present is bit1
// (only meaningful while the 5V boost is off, and a concept no other board here has),
// bat_present is bit2.

// Configure one PM1 pin as a push-pull output at the given level. Writes GPIO_FUNC,
// GPIO_MODE, GPIO_DRV, GPIO_OUT in that order -- GPIO_MODE and GPIO_OUT only take
// effect once the pin's GPIO_FUNC field reads 00, so the order is load-bearing.
//
// Pins 0-3 take their function bits from GPIO_FUNC0, pin 4 from GPIO_FUNC1. Getting
// that wrong does not fail, it silently reconfigures a neighbouring pin.
esp_err_t board_pm1_gpio_output(uint8_t pin, bool level);

// Configure one PM1 pin as an input with the given pull.
esp_err_t board_pm1_gpio_input(uint8_t pin, pm1_pull_t pull);

// Read one pin's input level from GPIO_IN.
//
// UNVERIFIED for GPIO1 and GPIO4. The card-detect scheme is transcribed from
// refs/M5PaperColor-UserDemo/main/hal/hal.cpp:181-183 and :481-483 -- drive GPIO4
// high, pull GPIO1 up, and LOW means a card is fitted -- and the UserDemo is its only
// source; M5Unified does not configure either pin for this board. Nothing here should
// branch on the result until a card has been held against it. See issues/08.
esp_err_t board_pm1_gpio_read(uint8_t pin, bool *high);

// The raw GPIO_IN byte, for logging all five pins at once during the probe.
esp_err_t board_pm1_gpio_read_all(uint8_t *bits);

// board_epd_power() / board_epd_power_state() (board.h) are thin wrappers over
// board_pm1_gpio_output() and GPIO_OUT bit 0. Pair the read-back with a DMM on EPD_3V3_L3B
// the first time -- that makes issues/02 pass or fail independently of whether the panel then
// responds.

// --------------------------------------------------------------------------- RTC
//
// U17 is an Epson RX8130CE on SYS_SCL / SYS_SDA -- the same bus as the PM1 and the
// SHT40 (schematic, page 3).
//
// It is NOT here for precision. Both this and the Capsule's BM8563 are calendar
// RTCs with one-second read resolution and no readable sub-second counter, so
// neither can time an 80 ms interval at all. And a 32.768 kHz tuning-fork crystal
// sits in the same tens-of-ppm class as the 40 MHz XTAL that already feeds
// esp_timer, so it is no more accurate either.
//
// It has two jobs, both worth having:
//   1. Wall-clock provenance on every run. A full FRS sweep is 5+ hours; the
//      pre-registration requires reporting session conditions and run order, and a
//      real timestamp makes that automatic and survives a reflash mid-session.
//   2. A gross-error tripwire. If the main crystal failed to start and the SoC fell
//      back to an RC oscillator, esp_timer would be wrong by PERCENT, not ppm.
//      Comparing elapsed esp_timer against elapsed RTC seconds catches that, and
//      replaces the manual "time 1000 s against an external clock" step.
//
// The datasheet quotes the slave address as 0x32, which is ambiguous: if that is the
// 8-bit write byte then the 7-bit address is 0x19. Rather than guess, board_i2c_scan()
// probes the bus and board_rtc_read() uses whichever of the two answered.
#define RX8130_ADDR_CANDIDATE_A 0x32
#define RX8130_ADDR_CANDIDATE_B 0x19

// The RX8130CE time registers start at 0x10 (SEC, MIN, HOUR, WEEK, DAY, MONTH, YEAR) in
// BCD. If that register map were wrong this returns ESP_ERR_INVALID_RESPONSE from the BCD
// plausibility check rather than reporting a plausible-looking wrong time.
//
// **VERIFIED FOR READING, 2026-09-10** (ticket 41), which this comment said was unverified
// for eight days longer than it had to be. The map decodes and the crystal runs at the right
// rate; what is wrong is only the EPOCH, because nothing has ever set it.
//
// Two absolute comparisons against a known host clock, five days apart, give the same
// constant offset: **226 days 11:04:43** (RTC `2026-01-26 12:22:01` against host
// `2026-09-09 23:26:44` UTC, read in the same minute) and **226 days 11:04:50**
// (`.scratch/captures/bringup-dma-bounce-20260904-092007.log`). **Seven seconds apart over
// five days**, and the older figure's host time is inferred from its filename, which carries
// several seconds of slop on its own — so the agreement is at least that good and the
// apparent 13.6 ppm is mostly the measurement. Two matching absolute offsets cannot come
// from a wrong register base, which is what the earlier interval-only evidence could not say.
//
// **WRITING IS STILL UNVERIFIED, and there is no board_rtc_write() to verify.** Ticket 41
// deliberately did not add one: a wrong write on the PM1's shared I2C bus is a different risk
// class from a wrong read, and it needs the owner present. So the wall clock this board
// reports is not the system clock — app_clock.c sets that from SNTP and never touches these
// registers.
//
// board_rtc_read() and board_rtc_seconds() are declared in board.h.

// Battery-backed RAM, four bytes at registers 0x20-0x23. This is what backs board.h's
// board_state_read/write() on this board, and it is where FR-5.6 keeps the displayed image
// index: it survives a full power cut, which NVS also would, but the RTC keeps it without a
// flash write per navigation and it is what the shipping firmware uses
// (local_photo_slideshow.cpp:25, 437-438, hal.cpp:518-545).
//
// Requires board_i2c_scan() to have resolved the RTC address first; without it those calls
// return ESP_ERR_INVALID_STATE rather than writing to whatever is at 0x32.
//
// RX8130_RAM_SIZE and BOARD_STATE_BYTES are the same number in two places, which is exactly
// the shape of ticket 43's seam 1. board_pm1.c asserts they agree at compile time so that
// changing one and not the other cannot produce a build.
#define RX8130_RAM_BASE 0x20
#define RX8130_RAM_SIZE 4

#endif // BOARD_PM1_H
