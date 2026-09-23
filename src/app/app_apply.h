// A settings change, as a transaction.
//
// Persisting a setting is one third of the job. app_settings_set_*() writes NVS; the value then has
// to reach the module that acts on it (app_display's own copy, or the charger), and if the change
// alters the picture already on the glass the panel owes a re-render. app_setting_effect.h is the
// table of which setting owes which; this module is the one place that acts on it.
//
// **Why it exists.** That three-step protocol was written out by hand at both of its call sites:
//
//     app_server.c  h_mode_cfg_set()         six keys, each with its own read-compare, its own
//                                            double write and its own *_changed boolean, ending
//                                            in a five-term disjunction before one redraw
//     frame_main.c  the TOP-button cycle      the same triple again, for rotation alone
//
// Two adapters for one change, which is what makes this a real seam rather than a hypothetical
// one. The failure mode it removes is silent: omit the second step and the value persists, reads
// back correctly over HTTP, and nothing happens. app_settings.h records that for low_power_mode;
// ticket 69 records it for rotation, where the acceptance test became a power cycle rather than
// a GET.
//
// **What it deliberately does NOT do.**
//
//   * It does not validate. Ticket 12 put validation at the route AND the setter on purpose, and
//     tickets 68 and 71 are the two that say why -- a body with one bad field must change nothing,
//     and a charge cap of 90 must be refused by name rather than clamped. The route still refuses
//     before it opens a transaction, and the setters still refuse underneath it. A value that
//     reaches app_apply_set() has already been accepted twice.
//   * It does not batch the NVS writes. Ticket 12 requires one key, one entry, one commit: "a
//     full-struct write on every change wears the flash and turns one bad field into nine". The
//     transaction here is over the APPLY and the REDRAW, not over the storage.
//   * It does not cover every setting. Only the six keys that app_setting_effect.h gives a live
//     half go through it; the other thirty-four owe nothing but a persist, so their callers keep
//     calling app_settings_set_*() directly and there is no second half to forget. Passing one of
//     those here is ESP_ERR_INVALID_ARG rather than a silent no-op.
//   * It does not remove app_display's own copies of the five render settings. Those are not
//     redundant -- the display task reads them under app_display's own mutex, and
//     app_display.c:126-129 explains why the palette in particular is read once per render rather
//     than once per stage. Pushing the value into that copy IS the live half, not a workaround.
//
// **One behaviour is unified rather than preserved.** The two call sites disagreed about a failed
// persist: frame_main.c pushed the display only when app_settings_set_rotation() returned ESP_OK,
// and h_mode_cfg_set() ignored the return and pushed regardless. This module takes frame_main.c's
// discipline for both, because the other order produces exactly the divergence ticket 69 calls the
// nastiest of its seven sites -- a panel showing a rotation that NVS does not hold, which reverts
// at the next boot with nothing in the console. A caller that wants to report the failure has the
// esp_err_t; both of today's callers ignore it, as they did before.

#ifndef APP_APPLY_H
#define APP_APPLY_H

#include <stdbool.h>

#include "esp_err.h"

#include "app_setting_effect.h"

// The accumulated redraw decision. A body carrying four render settings owes ONE refresh, not
// four -- at 15 to 31 s a refresh on the two boards this project runs on, that distinction is the
// difference between a settings save and a minute of the panel being unavailable.
typedef struct {
    bool redraw;
} app_apply_txn_t;

// Opens a transaction. Cheap and allocation-free: the struct is meant to live on the caller's
// stack for the length of one request or one button press.
void app_apply_begin(app_apply_txn_t *txn);

// Persists `value` for `key`, applies its live half, and records any redraw the change owes.
//
// `value` is an int for every key, which covers the three booleans (0 or 1), the two small
// unsigned values and charge_limit_pct's own int. **The value is not range-checked here** -- see
// the header comment; the route and the setter are the two places that do that.
//
// Returns ESP_OK when the value was stored, or when it already equalled the stored one and so
// nothing was owed. Returns the setter's error when the persist failed, in which case the live
// half does NOT run and no redraw is recorded. Returns ESP_ERR_INVALID_ARG for a NULL
// transaction, a key outside app_setting_key_t, or a key that owes no live half.
esp_err_t app_apply_set(app_apply_txn_t *txn, app_setting_key_t key, int value);

// Performs at most one re-render, for the whole transaction. Safe to call when nothing changed --
// it then does nothing and returns ESP_OK, which is what lets a caller commit unconditionally
// instead of guarding the call.
esp_err_t app_apply_commit(app_apply_txn_t *txn);

#endif  // APP_APPLY_H
