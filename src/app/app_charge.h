// The battery charge cap: ticket 71, the operator's longevity request of 2026-09-20.
//
// **This module is policy only. `board_charger_set_vreg_mv()` is the mechanism** and owns the one
// safety property that matters -- it cannot raise the termination voltage above the value the part
// booted with. Nothing here computes a register value.
//
// WHAT A CAP IS HERE. Not a control loop. The charger keeps doing its own constant-voltage
// regulation; we lower the voltage it regulates to. That is why there is no hysteresis, no
// percentage estimated from a voltage, and no toggling of the charge-enable bit:
//
//   * `battery_percent_from_mv()` is a DISCHARGE curve (src/core/battery.h says so) and reads high
//     under charge, so "stop at 80 %" computed from it would stop somewhere else. Here no
//     percentage is computed at all.
//   * A bang-bang cap around 80 % on a mains-powered frame would make thousands of shallow cycles
//     at high state of charge, which is the ageing this feature exists to avoid.
//
// ON ONE BOARD ONLY, permanently, and it is copper rather than unfinished work: the M5Paper
// Color's charge-enable pin is a no-connect, its charger's I2C is unwired, and its system rail IS
// the battery node through a 0 ohm link -- so stopping its charge would start a discharge. The
// board layer answers ESP_ERR_NOT_SUPPORTED and this module reports that once rather than
// retrying. docs/board-pinmap.md §"Charging" is the table; ticket 71 §1 and §8.1 the account.
//
// THE WATCHDOG IS REAL, AND THAT IS MEASURED (2026-09-20, ticket 71 §10). It was written here as a
// hypothesis with `corrections` as its instrument, and the instrument answered on the first run:
// **2 corrections in the first two minutes, and the cell charged back up to 4132 mV in the windows
// between them.** A 20 s re-assert narrows the leak; it does not close it. So the order is now
//
//   1. `board_charger_watchdog_disable()` -- stop the part restoring its own defaults, then
//   2. `board_charger_set_vreg_mv()` -- set the voltage, then
//   3. the tick re-reads anyway and still counts corrections.
//
// Step 3 is kept even though step 1 should make it unnecessary, because it is the instrument that
// found the problem and it is what will notice if a future unit, revision or reset path behaves
// differently. **`corrections` staying at zero is the only evidence that step 1 works**, and it
// costs three I2C reads a minute to keep collecting it.

#ifndef APP_CHARGE_H
#define APP_CHARGE_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

// Reads the setting and writes the charger. Call at boot and after a setting change; the tick
// calls it too. Cheap and idempotent: with the register already right it is two I2C reads.
void app_charge_apply(void);

// Rate-limits itself, so it is safe to call from the 200 ms application loop.
void app_charge_tick(void);

typedef struct {
    bool supported;   // a reachable charger answered at least once
    uint8_t limit_pct;    // the setting, as last applied
    uint16_t target_mv;   // what was asked for; 0 while unmanaged
    uint16_t applied_mv;  // what the part read back -- the only figure that is evidence
    uint32_t corrections; // times a re-assert found REG04 had moved under us
    uint32_t writes;      // times a write was actually issued, first apply included
    bool watchdog_off;    // the part's I2C watchdog was disabled and read back disabled

    // The last whole register block and its age. **Served to /api/battery from here rather than
    // read in the handler**: twelve more I2C transactions per request, from a task that is already
    // holding the server's only worker, is not worth a fresher battery icon. The voltage in that
    // response is live; this block is up to APP_CHARGE_REASSERT_MS old and the field says so.
    //
    // This used to cite a bus "taking turns by luck", **which was wrong** -- the IDF i2c_master
    // driver serialises transactions on a bus with its own mutex (ticket 76,
    // docs/agents/board-and-storage.md). The reason above is cost, not safety, and it still holds.
    board_charger_t last;
    bool last_valid;
    uint32_t last_age_ms;
} app_charge_state_t;

void app_charge_get(app_charge_state_t *out);

#endif // APP_CHARGE_H
