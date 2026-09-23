#include "app_charge.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"

#include "app_settings.h"

// 80 % of a single lithium cell is ~4.0 V at rest. The charger's step is 32 mV off a 3856 mV base,
// so the two settings either side of 4.0 V are 3984 mV (n=4) and 4016 mV (n=5); 4016 is the nearer
// and is what 80 % means here. **The number is a cell-chemistry convention, not a measurement
// taken on this bench** -- it is what laptop and phone "battery limit" features implement -- and
// nothing here should claim otherwise until a capacity check has been run against it.
#define CHARGE_CAP_MV 4016u

// 100 % asks for more than any cell would take and lets board_charger_set_vreg_mv() clamp it to
// the value the part booted with. That is deliberate: the default belongs to the hardware, so
// restoring it should not need this file to know what it is.
#define CHARGE_FULL_MV 0xFFFFu

// Short enough that a reverted register is corrected well inside one charge step, long enough that
// it is 3 I2C reads a minute rather than 300. Also a long way under the 40 s a watchdog would be
// if REG05's 0x9F means what the BQ2429x layout says -- see the header.
#define APP_CHARGE_REASSERT_MS 20000u

static app_charge_state_t s_state;
// Separate from s_state.supported, which stays false until something answers: this stops a board
// with no charger from printing the same line every 20 s forever.
static bool s_unsupported_reported;
static int64_t s_last_apply_us;
// The setting the previous apply acted on, so that a write caused by the USER changing the setting
// is not counted as the register moving on its own. 0xFF = no apply yet.
static uint8_t s_prev_pct = 0xFF;

static uint16_t target_for(uint8_t pct)
{
    switch (pct) {
    case 80:
        return CHARGE_CAP_MV;
    case 100:
        return CHARGE_FULL_MV;
    default:
        return 0; // unmanaged
    }
}

// Returns true when a write was actually issued -- i.e. the register was not already where we
// want it. That single bit is what distinguishes "holding" from "being reverted under us", so it
// is returned rather than logged here.
static bool apply_once(void)
{
    const uint8_t pct = app_settings_charge_limit_pct();
    const uint16_t target = target_for(pct);

    s_last_apply_us = esp_timer_get_time();
    s_state.limit_pct = pct;
    s_state.target_mv = (target == CHARGE_FULL_MV) ? 0 : target;

    // Read first, unconditionally, even when unmanaged: the raw block is this module's instrument
    // and /api/battery serves it, so "do not manage the charger" must not also mean "stop looking
    // at it". The read is also what captures the ceiling inside the board layer.
    board_charger_t now;
    const esp_err_t rerr = board_charger_read(&now);
    if (rerr == ESP_ERR_NOT_SUPPORTED) {
        if (!s_unsupported_reported) {
            s_unsupported_reported = true;
            printf("# charge: no charger this firmware can reach -- charge_limit_pct=%u is stored "
                   "and does nothing here (ticket 71)\n",
                   (unsigned)pct);
        }
        return false;
    }
    if (rerr != ESP_OK) {
        printf("# charge: charger read failed: %s\n", esp_err_to_name(rerr));
        return false;
    }
    s_state.supported = true;
    s_state.last = now;
    s_state.last_valid = true;

    if (target == 0) {
        s_state.applied_mv = now.vreg_mv;
        return false;
    }

    // BEFORE the voltage, and every time rather than once: this is what stops the part restoring
    // its own defaults, and a chip reset puts the watchdog back. Idempotent -- one read when it is
    // already off. A failure here is reported and then the voltage is set anyway, because a leaky
    // cap is still better than none and the tick's `corrections` will show how leaky.
    const esp_err_t werr = board_charger_watchdog_disable();
    const bool was_off = s_state.watchdog_off;
    s_state.watchdog_off = (werr == ESP_OK);
    if (werr != ESP_OK) {
        if (was_off || s_state.writes == 0) {
            printf("# charge: could not disable the charger's I2C watchdog: %s -- the cap will "
                   "leak and `corrections` will count it (ticket 71)\n",
                   esp_err_to_name(werr));
        }
    } else if (!was_off && s_state.writes > 0) {
        printf("# charge: charger I2C watchdog disabled again (it had come back)\n");
    }

    const uint16_t before = now.vreg_mv;
    uint16_t applied = 0;
    const esp_err_t err = board_charger_set_vreg_mv(target, &applied);
    s_state.applied_mv = applied;
    // **RE-READ THE WHOLE BLOCK, not just the decoded field.** Patching `vreg_mv` alone left the
    // raw `regs` that /api/battery serves showing the PRE-write REG04 beside a post-write decode --
    // caught on hardware 2026-09-20, where the response read `vreg_mv: 4016` and `REG04 = 0x58`
    // in the same object. A raw block exists here precisely so a later reading can disagree with
    // this build's decode; one that disagrees with the same build's own write is a trap.
    board_charger_t after;
    if (board_charger_read(&after) == ESP_OK) {
        s_state.last = after;
    } else {
        s_state.last.vreg_mv = applied;
    }
    if (err != ESP_OK) {
        printf("# charge: set %u mV failed: %s (read back %u mV)\n", (unsigned)target,
               esp_err_to_name(err), (unsigned)applied);
        return false;
    }
    if (applied != before) {
        s_state.writes++;
        return true;
    }
    return false;
}

void app_charge_apply(void)
{
    const uint8_t pct_before = s_prev_pct;
    const bool wrote = apply_once();
    s_prev_pct = s_state.limit_pct;
    if (wrote) {
        printf("# charge: limit %u%% -> vreg %u mV%s\n", (unsigned)s_state.limit_pct,
               (unsigned)s_state.applied_mv,
               (pct_before == 0xFF) ? " (first apply)" : "");
    }
}

void app_charge_tick(void)
{
    if (s_unsupported_reported) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (s_last_apply_us != 0 &&
        (now_us - s_last_apply_us) < (int64_t)APP_CHARGE_REASSERT_MS * 1000) {
        return;
    }

    const uint8_t pct_before = s_prev_pct;
    const bool wrote = apply_once();
    s_prev_pct = s_state.limit_pct;

    // A write on a re-assert, with the SETTING UNCHANGED, means the register moved on its own --
    // which is the watchdog hypothesis showing itself. Counted rather than only logged, because
    // the count over a long run is the measurement: zero settles the question one way, and the
    // interval between corrections settles it the other (ticket 71 §9.3).
    if (wrote && pct_before == s_state.limit_pct) {
        s_state.corrections++;
        printf("# charge: REG04 had moved; re-applied %u mV (correction %lu)\n",
               (unsigned)s_state.applied_mv, (unsigned long)s_state.corrections);
    } else if (wrote) {
        printf("# charge: limit now %u%% -> vreg %u mV\n", (unsigned)s_state.limit_pct,
               (unsigned)s_state.applied_mv);
    }
}

void app_charge_get(app_charge_state_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_state;
    out->last_age_ms = (s_last_apply_us == 0)
                           ? 0
                           : (uint32_t)((esp_timer_get_time() - s_last_apply_us) / 1000);
}
