// "May the mirror open a window right now?" -- the SMB mirror's schedule, as a predicate.
//
// **Why it is here and not in app_smb_sync.c, where it lived until 2026-09-21.** It was already a
// pure function of file statics and `now_us`, with one impure term (`app_clock_schedule_active()`,
// now an input). It is also the most-revised rule in that module: ticket 28 gave it the deadline,
// ticket 42 the active-hours hold, ticket 37 Phase 3 the two on-demand reasons, and ticket 59 spent
// five arms and two wrong explanations on it. It had never executed on the host.
//
// The project states the criterion for this move twice, in its own words:
//
//     board_smb_classify.h:1  "Split out of board_smb.c for one reason: **it is a pure function, so
//                              it can be linked and tested under env:native**."
//     test_schedule:1         app_schedule_active() is "worth its own suite for one reason: the
//                              WRAPPING window is where the obvious implementation is wrong, and
//                              **it is wrong SILENTLY**."
//
// This predicate has exactly that property. A wrong branch gives a mirror that never opens a window,
// or one that opens them back to back and holds httpd down, and **both read from the console as a
// network or a storage problem** -- which is what ticket 59 spent three arms discovering.
//
// **The honest limit: this buys the caller nothing.** No leverage, no new capability. The whole
// return is locality -- one rule, one place, with tickets 28, 42, 37 and 59 argued in a test instead
// of in a comment. The rest of window_task() genuinely needs the session, the task and httpd, and
// stays where it is.
//
// **Not moved, deliberately:** the periods. SMB_SYNC_PERIOD_MS is `#ifndef`-guarded in
// app_smb_sync.h so a build can override it, and app_smb_sync.h carries the ticket references that
// explain each value. They arrive here as smb_window_timing_t, which also lets a test use small
// round numbers instead of an hour.

#ifndef SMB_WINDOW_H
#define SMB_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "Never" for last_run_end_us and last_catalog_us. Matches app_smb_sync.c's own initialisers.
#define SMB_WINDOW_NEVER INT64_MIN

// The three periods, in MICROseconds -- app_smb_sync.h declares them in milliseconds and the caller
// multiplies, which is where the * 1000 used to sit at each of the six comparisons.
typedef struct {
    // The floor between two windows, so a burst of asks cannot open them back to back and hold
    // httpd down.
    int64_t window_gap_us;
    // Measured from BOOT, not from anything else -- see `now_us` below.
    int64_t first_delay_us;
    // The periodic mirror run's clock, and separately the catalogue's staleness horizon.
    int64_t period_us;
} smb_window_timing_t;

typedef struct {
    // A window is open. Its own gap rule applies and nothing else does: a run in progress finishes
    // rather than being abandoned mid-window with httpd down.
    bool run_active;
    // When the last window ENDED. **Zero, not SMB_WINDOW_NEVER, before the first one has ended** --
    // that is app_smb_sync.c's initialiser and it is relied on here: the gap between windows has no
    // meaning before there has been one, and zero makes the comparison pass rather than overflow,
    // which SMB_WINDOW_NEVER would (now_us - INT64_MIN).
    int64_t last_window_end_us;
    // A user at the UI. Always wins, on every branch.
    bool manual_request;
    // When the last RUN ended, or SMB_WINDOW_NEVER if none has.
    int64_t last_run_end_us;
    // On-demand fetching rather than the whole-folder mirror. Two reasons to open a window, not one.
    bool on_demand;
    // How many photographs the selector has asked for.
    size_t want_count;
    // When the catalogue was last listed, or SMB_WINDOW_NEVER.
    int64_t last_catalog_us;
    // app_clock_schedule_active(). The one term that was a function call rather than a static, and
    // the reason this predicate could not be host-tested where it was.
    bool schedule_active;
} smb_window_state_t;

// True when a window may open. `now_us` is time since boot (esp_timer_get_time()'s domain), which
// matters for one branch: the first-delay test compares `now_us` against first_delay_us as an
// ABSOLUTE, not as a delta from anything.
//
// False for a NULL state or NULL timing: no state means no window.
bool smb_window_is_due(const smb_window_state_t *state, const smb_window_timing_t *timing,
                       int64_t now_us);

#endif  // SMB_WINDOW_H
