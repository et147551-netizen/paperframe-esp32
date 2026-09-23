// The board interface: what the application and panel layers are allowed to know about the
// hardware under them.
//
// It is short because the exploration for ticket 43's refactor found only four places in the
// whole application that reached past it, and each was one call. This header is the list of
// what those places actually needed, not a guess at what a board might offer.
//
// WHAT SELECTING A BOARD MEANS. One build flag -- BOARD_M5PAPER_COLOR or
// BOARD_RETERMINAL_E1002 -- decides three things at once: which pin header this file
// includes, which panel epd_panel.h includes, and which of the per-board .c files compile to
// something rather than to nothing. There is no default and no fallback: the #else below is
// an #error, because a board picked by accident is a class of bug that shows up as wrong
// GPIO numbers on working-looking firmware.
//
// Why an #error and not a default: `compile_commands.json` omits PlatformIO's build_flags
// and will tell you a flag did not arrive when it did (docs/agents/build-system.md), so the
// only cheap way to know a board was selected is for the build to fail when none was. Every
// env that builds has selected one.
//
// WHY EACH per-board .c IS WRAPPED IN ITS OWN #ifdef rather than filtered out of the build:
// `build_src_filter` does nothing under framework = espidf -- the sources come from
// src/CMakeLists.txt, which globs, and never sees PlatformIO's filter. This project already
// selects among its ten entry points the same way, with -DBUILD_*.

#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#if defined(BOARD_M5PAPER_COLOR)
#include "pins_m5papercolor.h"
#elif defined(BOARD_RETERMINAL_E1002)
#include "pins_reterminal_e1002.h"
#else
#error "No board selected: define BOARD_M5PAPER_COLOR or BOARD_RETERMINAL_E1002"
#endif

// --------------------------------------------------------------------------- bring-up

// The board's own buses and devices: I2C, sensors, a charger probe -- whatever this board
// needs before anything else will work. Idempotent.
esp_err_t board_i2c_init(void);

// Probes every 7-bit address and prints what answered, naming the ones we expect. Makes "the
// shared I2C bus is healthy" a concrete observation rather than an inference from one device
// replying -- and on a board whose charger is present on some hardware revisions and not
// others, it is how you find out which unit you have.
esp_err_t board_i2c_scan(void);

// ----------------------------------------------------------------------- panel power

// Raise or drop the panel's rail. Returns ESP_OK and does nothing on a board where the panel
// has no switchable rail; the caller cannot tell, and does not need to.
esp_err_t board_epd_power(bool on);

// Read the rail back, so a caller can confirm the write landed rather than assuming it.
esp_err_t board_epd_power_state(bool *on);

// ------------------------------------------------------------------- power status

// What the board can say about where its power is coming from. Fields a given board cannot
// answer read false or zero -- so a caller must not treat `!vin_present` as "on battery"
// without checking that the board can tell. app_server.c's /api/battery uses `vbat_mv`
// alone, which every board here can supply.
typedef struct {
    bool vin_present;    // external 5 V supply valid, i.e. USB power
    bool vinout_present; // the 5 V boost output; a PM1 concept, false elsewhere
    bool bat_present;
    uint16_t vbat_mv;
    uint16_t vin_mv;
} board_power_t;

esp_err_t board_power_read(board_power_t *out);

// ----------------------------------------------------------------------- charger
//
// **Only one of the two boards has a charger software can see, and that is a fact about the
// PCB rather than about this driver.** The M5Paper Color's charge-enable pin is not connected,
// its charger's I2C pins are not connected, and its system rail IS the battery node through a
// 0 ohm link -- so there is nothing to read and nothing that could be told. It returns
// ESP_ERR_NOT_SUPPORTED, permanently. `docs/board-pinmap.md` §"Charging, and why only one
// board can control it" is the table, and ticket 71 is the account.
//
// A pre-V1.2 E1002 also answers ESP_ERR_NOT_SUPPORTED: that revision carries a non-I2C
// ETA6003, and board_i2c_init() probes rather than assumes.
typedef struct {
    // Raw REG00..REG0B, straight off the wire and undecoded. Here because the field layout of
    // REG08 has THREE mutually inconsistent readings among the sources available to this
    // project and no datasheet for the part is on this disk (ticket 71 §9.2) -- so the bytes
    // are reported and the verdict is computed at analysis time, which is this project's rule.
    uint8_t regs[12];

    // The two decoded fields, from the bit positions pins_reterminal_e1002.h currently uses.
    // **Unverified against hardware** -- read `regs` when the answer matters.
    uint8_t chrg_stat;  // 0 not charging, 1 pre-charge, 2 fast charge, 3 done
    bool power_good;

    // REG04[7:3] decoded as 3856 mV + n*32 mV. **The encoding is from a third-party driver,
    // not a datasheet** (ticket 71 §9.1), so a caller that writes must check this against the
    // value read before any write rather than trusting the arithmetic.
    uint16_t vreg_mv;
} board_charger_t;

esp_err_t board_charger_read(board_charger_t *out);

// Prints the whole register block and both readings of REG08's contested field, so one boot log
// settles which bit positions track the cell. Prints one "not readable" line and returns on a
// board with no reachable charger, because that is an answer too.
void board_charger_dump(void);

// Lower the charge termination voltage, which is what a "charge to 80 %" cap actually is: the
// charger keeps doing constant-voltage regulation, just at a lower voltage. Nothing here loops,
// counts or decides -- compare app_charge.c, which owns the policy.
//
// **THIS FUNCTION CANNOT RAISE THE TERMINATION VOLTAGE ABOVE THE VALUE THE PART BOOTED WITH**,
// and that is the one safety property in it. Writing a HIGHER voltage to a lithium cell is the
// way this feature could damage hardware, so the ceiling is captured on the first successful
// read and `target_mv` is clamped to it; a caller asking for more gets the ceiling and ESP_OK,
// not a fault. `target_mv` is also rounded DOWN to a representable step, never up.
//
// Writes, then reads back and reports what the part actually holds in `applied_mv` -- because
// the register encoding here is derived from a third-party driver rather than a datasheet, and
// because this family is believed to have an I2C watchdog that may restore defaults on a
// timeout (ticket 71 §9.1). A caller that does not re-read has not verified anything.
//
// ESP_ERR_NOT_SUPPORTED where there is no reachable charger; ESP_ERR_INVALID_STATE if the
// read-back does not match what was written.
esp_err_t board_charger_set_vreg_mv(uint16_t target_mv, uint16_t *applied_mv);

// Stop the charger's I2C watchdog from restoring its register defaults, which is what makes a
// lowered termination voltage HOLD rather than leak.
//
// **This is needed, and that is measured rather than assumed.** With the cap applied and no
// watchdog handling, REG04 was observed reverting to its default and the cell charging back up in
// the windows between re-asserts (ticket 71 §10). Re-asserting faster narrows those windows; only
// this closes them.
//
// Idempotent, verified by read-back, and safe in the direction that matters: with the watchdog off
// the part keeps the LOWER voltage it was given. ESP_ERR_NOT_SUPPORTED where there is no reachable
// charger, ESP_ERR_INVALID_STATE if the read-back still shows the watchdog enabled.
esp_err_t board_charger_watchdog_disable(void);

// ------------------------------------------------------------------ microSD rail

// Power the card's rail, waiting BOARD_CARD_POWER_SETTLE_MS the first time it goes up.
esp_err_t board_card_power(bool on);

// Whether a card is fitted. Returns false on any bus error as well as on no card, which is
// why frame_main.c debounces it rather than acting on one reading.
bool board_card_present(void);

// ---------------------------------------------------------------------- sensors

// Ambient temperature and humidity from the board's own sensor -- distinct from the panel's
// internal sensor, and the cross-check for it (issues/04). ESP_ERR_NOT_SUPPORTED if the
// board has none.
esp_err_t board_sht40_read(float *temp_c, float *humidity_pct);

// -------------------------------------------------------------------------- RTC
//
// NOT here for precision. These are calendar RTCs with one-second read resolution and no
// readable sub-second counter, and a 32.768 kHz tuning-fork crystal is in the same
// tens-of-ppm class as the XTAL that already feeds esp_timer. They earn their place by
// giving every run a wall-clock provenance that survives a reflash, and by catching a
// gross error -- if the main crystal failed to start, esp_timer would be wrong by percent.
//
// The system clock does NOT come from here; app_clock.c sets it from SNTP.
typedef struct {
    int year; // 2000-2099
    int month, day, hour, minute, second;
} board_datetime_t;

esp_err_t board_rtc_read(board_datetime_t *dt);

// Seconds since midnight, for drift arithmetic without date handling.
esp_err_t board_rtc_seconds(int64_t *secs);

// ------------------------------------------------------- small persistent state
//
// BOARD_STATE_BYTES bytes that survive a full power cut, for FR-5.6's slideshow index and
// the catalogue epoch cursor (app_slideshow.c). This is the interface app_slideshow.c used
// to reach past, straight into RX8130 registers -- ticket 43 seam 4.
//
// THE TWO BOARDS IMPLEMENT IT DIFFERENTLY AND THE DIFFERENCE MATTERS. The M5Paper Color has
// four bytes of battery-backed RAM in its RTC, chosen over NVS precisely to avoid ~288 flash
// writes a day (ticket 37). A board whose RTC has no general-purpose RAM has to fall back to
// NVS, and ticket 39's finding is the input: NVS commits per key, so the cost is ~288
// single-key commits a day rather than 288 whole-struct writes, which makes the fallback more
// defensible than ticket 37's arithmetic made it look. Neither figure has been measured as a
// flash-wear outcome.
//
// So: `index` is 0..BOARD_STATE_BYTES-1 and a caller must not assume the store is free to
// write. app_slideshow.c writes it on navigation, which is the rate both figures above are
// about.
esp_err_t board_state_write(uint8_t index, uint8_t value);
esp_err_t board_state_read(uint8_t index, uint8_t *value);

#endif // BOARD_H
