#include "smb_window.h"

bool smb_window_is_due(const smb_window_state_t *state, const smb_window_timing_t *timing,
                       int64_t now_us)
{
    if (!state || !timing) {
        return false;
    }

    // A window already open: only the gap rule applies. A run in progress finishes rather than being
    // abandoned mid-window with httpd down, which is why none of the branches below can stop it.
    if (state->run_active) {
        return (now_us - state->last_window_end_us) >= timing->window_gap_us;
    }

    // A user at the UI, and it must always work. Above the schedule hold on purpose.
    if (state->manual_request) {
        return true;
    }

    // Nothing has run yet. `now_us` is uptime, so this is an absolute comparison and not a delta.
    if (state->last_run_end_us == SMB_WINDOW_NEVER) {
        return now_us >= timing->first_delay_us;
    }

    // ON-DEMAND HAS TWO REASONS TO OPEN A WINDOW and the period is only one of them: something the
    // selector asked for, or the catalogue being stale. The gap is still enforced, so a burst of
    // asks cannot open windows back to back and hold httpd down.
    if (state->on_demand) {
        if ((now_us - state->last_window_end_us) < timing->window_gap_us) {
            return false;
        }
        if (state->want_count > 0) {
            return true;
        }
        // Short-circuits on NEVER before the subtraction, which is what keeps `now_us - INT64_MIN`
        // from happening.
        return (state->last_catalog_us == SMB_WINDOW_NEVER) ||
               ((now_us - state->last_catalog_us) >= timing->period_us);
    }

    // TICKET 42'S HOLD, and it is deliberately only on this branch -- the periodic mirror run, which
    // fetches a whole folder on its own clock and is the write-rate term ticket 39 names.
    //
    // The three paths above are left alone, each for its own reason. A manual request is a user at
    // the UI and must always work. An in-progress run finishes rather than being abandoned
    // mid-window with httpd down. And the two on-demand reasons stay: the want list is fed by
    // slideshow advances, which the schedule has already stopped, so it self-suppresses without
    // stranding a want that was declared before the window closed -- while the catalogue keeps its
    // hourly clock ON PURPOSE, so the morning's catalogue is current. That is one listing an hour,
    // no fetches and no eviction.
    if (!state->schedule_active) {
        return false;
    }
    return (now_us - state->last_run_end_us) >= timing->period_us;
}
