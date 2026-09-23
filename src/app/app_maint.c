#include "app_maint.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "app_clock.h"
#include "app_display.h"
#include "app_settings.h"
#include "epd_cmds.h"
#include "epd_maint_course.h"

// How often the clock and the schedule are consulted. The decision is about one-hour periods, and
// the check copies settings and reads the wall clock, so ten seconds is the same cadence
// app_smb_sync_tick() gets and for the same reason. The STEP timer is checked on every tick, so a
// back-to-back course does not wait ten seconds per flat.
#define SCHED_POLL_MS 10000u

// The spread form puts the ten steps across one hour: 3600 / 10 = 360 s between starts.
//
// **Spread rather than back to back, and this is a measurement decision.** The colour figures this
// mode exists to be read against were taken at 40 s spacing at 34-40 C, and the panel's waveform is
// temperature dependent -- so a back-to-back cycle heats the panel and washed-out colour would read
// as a panel fault when it is heat. That is the false positive the mode exists to avoid producing.
// 360 s is also comfortably past the 180 s minimum interval that circulates for these panels, which
// appears in neither vendor manual and is cited here for completeness rather than relied on.
#define SPREAD_STEP_MS 360000u

// Back to back: due immediately, and gated by the panel being free rather than by a clock.
#define FAST_STEP_MS 0u

// **EVERY FIELD BELOW IS WRITTEN ONLY BY THE APPLICATION TASK**, inside app_maint_tick(). There is
// no mutex here and that is deliberate: a request arriving from the web server sets one of the
// `s_want_*` flags and the tick acts on it, which is exactly the shape service_buttons() already uses
// for the button callbacks in frame_main.c. A mutex would be a third thing holding internal RAM --
// the scarce resource -- to protect a state machine whose only writer is one task.
// Which of the two flat sequences is running. They share every mechanism here -- the step timer, the
// busy gate, the want-flags, the console line -- and differ only in their table and their purpose:
// the course is for a person to LOOK at, the clear is for a panel that has kept a trace.
typedef enum {
    SEQ_COURSE = 0,
    SEQ_CLEAR,
} maint_seq_t;

static bool s_course_active;
static maint_seq_t s_seq;
static bool s_spread;
static int s_step;            // the next step to request, 0..seq_steps()
static bool s_prime_white;    // a leading white is still owed before step 0
static uint32_t s_next_step_ms;
static uint32_t s_courses;

// The ACTIVE sequence's length. Held rather than derived, because a clear's length depends on the
// repeat count the request carried and the course's does not.
static int s_steps;

static int seq_steps(void)
{
    return s_steps;
}

static uint8_t seq_colour(int step)
{
    return s_seq == SEQ_CLEAR ? epd_maint_clear_colour(step, s_steps)
                              : epd_maint_course_colour(step);
}

static const char *seq_name(int step)
{
    return s_seq == SEQ_CLEAR ? epd_maint_clear_name(step, s_steps)
                              : epd_maint_course_name(step);
}

// The cross-task handoff. Written by whoever calls the two public verbs -- in practice the one httpd
// worker -- and cleared by the tick. `volatile` because the tick's loop must re-read them rather than
// hoist the load.
static volatile bool s_want_start;
static volatile bool s_want_spread;
static volatile bool s_want_clear; // which sequence the pending start asks for
static volatile int s_want_cycles; // its repeat count, for a clear
static volatile bool s_want_stop;

// The window edge. `s_sched_known` is separate from the boolean so that the FIRST observation is not
// mistaken for an edge: a frame that boots with the window closed must not be treated as one whose
// window just closed.
static bool s_sched_known;
static bool s_sched_active;
static uint32_t s_sched_checked_ms;
static bool s_parked;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------- the course

static void course_finish(const char *why)
{
    // Which sequence ended, because a log tells the two apart nowhere else.
    printf("# maint %s %s after %d/%d steps (courses=%u)\n",
           s_seq == SEQ_CLEAR ? "clear" : "course", why, s_step, seq_steps(),
           (unsigned)(s_courses));
    s_course_active = false;
    s_prime_white = false;
}

static void course_step(uint32_t now)
{
    // Nothing is stacked on the panel: one request replaces any pending one (app_display.h), so a
    // step posted while the previous refresh is still running would DISCARD a flat and the course
    // would silently show eight colours. app_display_busy() covers both the in-flight refresh and
    // the pending slot.
    if (app_display_busy()) {
        return;
    }
    if ((int32_t)(now - s_next_step_ms) < 0) {
        return;
    }

    // The leading white, owed only by a manually started course -- see app_maint.h. Requested through
    // the flat path rather than app_display_request_blank() so that it is the panel's own white index
    // and not the current palette's nearest match to (255,255,255), and so that it appears on the
    // console as `(flat:white)` like every other step.
    if (s_prime_white) {
        s_prime_white = false;
        const esp_err_t err = app_display_request_flat(EPD_COLOR_WHITE);
        printf("# maint prime colour=white spread=%d err=%s\n", s_spread ? 1 : 0,
               esp_err_to_name(err));
        s_next_step_ms = now + (s_spread ? SPREAD_STEP_MS : FAST_STEP_MS);
        return;
    }

    if (s_step >= seq_steps()) {
        s_courses++;
        course_finish("done");
        return;
    }

    const uint8_t colour = seq_colour(s_step);
    const esp_err_t err = app_display_request_flat(colour);
    // Raw, one line per step, and it carries the COLOUR rather than only the index: a course that
    // showed yellow twice reads exactly like one that showed yellow and green otherwise. No verdict
    // is computed here -- whether the flats look right is what a person or the flatbed answers.
    printf("# maint %s step=%d/%d colour=%s spread=%d next_s=%u err=%s\n",
           s_seq == SEQ_CLEAR ? "clear" : "course", s_step + 1, seq_steps(), seq_name(s_step),
           s_spread ? 1 : 0,
           (unsigned)((s_spread ? SPREAD_STEP_MS : FAST_STEP_MS) / 1000u), esp_err_to_name(err));
    s_step++;
    s_next_step_ms = now + (s_spread ? SPREAD_STEP_MS : FAST_STEP_MS);
}

// `cycles` is read only for a clear; the course has one length. A cycles count this refuses leaves
// nothing started, which is what makes the route's 400 honest.
static esp_err_t course_start(maint_seq_t seq, bool spread, bool prime, int cycles)
{
    if (s_course_active) {
        return ESP_ERR_INVALID_STATE;
    }
    const int steps = seq == SEQ_CLEAR ? epd_maint_clear_steps(cycles) : EPD_MAINT_COURSE_STEPS;
    if (steps <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_course_active = true;
    s_seq = seq;
    s_steps = steps;
    s_spread = spread;
    s_step = 0;
    s_prime_white = prime;
    s_next_step_ms = now_ms();
    printf("# maint %s start spread=%d prime=%d steps=%d cycles=%d\n",
           seq == SEQ_CLEAR ? "clear" : "course", spread ? 1 : 0, prime ? 1 : 0, steps,
           seq == SEQ_CLEAR ? cycles : 1);
    return ESP_OK;
}

// ---------------------------------------------------------------------- the window edge

// The window has just closed. Park the panel on white if that is switched on, and start the weekly
// course if this is its day.
//
// **The hour is what tells a real closing from a clock that has only just arrived.**
// app_clock_schedule_active() fails open in every direction, so a frame that boots at 03:00 inside a
// closed window reports "active" until SNTP lands and then produces a perfectly genuine falling edge
// -- at 03:00, with most of the night already gone. Parking white then is right and is the whole
// point of the standby. Starting a one-hour course then is not, so the course is gated on the edge
// being observed in the hour the window actually ends. No uptime heuristic, no boot flag: the
// discriminator is exact, and it is readable on the console because both numbers are printed.
static void on_window_closed(void)
{
    int hour = -1;
    const bool have_hour = app_clock_local_hour(&hour);
    uint8_t start_h = 0, end_h = 0;
    app_settings_active_hours(&start_h, &end_h);
    const bool at_end_hour = have_hour && hour == (int)end_h;

    const bool park = app_settings_standby_white();
    int wday = -1;
    const bool have_wday = app_clock_local_wday(&wday);
    const uint8_t day = app_settings_maint_day();
    // A weekly schedule on an unsynced clock must be "not today" rather than day 0, or the course
    // would run at every edge. app_clock_local_wday() returning false is that case.
    const bool course_due = at_end_hour && have_wday && wday == (int)day;

    // Ticket 68 §15: the standby can be a whole clear cycle rather than one white. Default OFF, and
    // the number is why -- see app_settings.h.
    const bool deep = park && app_settings_standby_deep();

    printf("# maint window closed hour=%d end=%u wday=%d maint_day=%u park=%d deep=%d course=%d\n",
           hour, (unsigned)end_h, wday, (unsigned)day, park ? 1 : 0, deep ? 1 : 0,
           course_due ? 1 : 0);

    // **THE COURSE WINS ON ITS OWN NIGHT, and the deep standby is skipped rather than queued.**
    // Only one sequence can hold the panel, so a deep clear started here would make `course_start()`
    // refuse and the weekly diagnostic would vanish silently once a week -- the worst kind of
    // interaction, since nothing would report it. Skipping the clear costs nothing real: the course
    // is ten flats that drive every pigment through a full cycle and it ends on white, so it already
    // does everything the clear does and more.
    if (course_due) {
        if (park) {
            s_parked = true;
            const esp_err_t err = app_display_request_blank();
            printf("# maint park white err=%s\n", esp_err_to_name(err));
        }
        // prime = !park: the park's own white IS the leading clear, so a parked panel needs no
        // eleventh refresh. With the standby switched off the course has to clear the photograph
        // itself or its first flat is read through one.
        course_start(SEQ_COURSE, true, !park, 1);
        return;
    }

    if (deep) {
        s_parked = true;
        // One cycle, not the repeat count -- an unattended nightly action takes the smallest form of
        // the thing, and the repeat count is for a person who is looking at a panel and chose to
        // escalate. No prime: a clear's own first step is white.
        const esp_err_t err = course_start(SEQ_CLEAR, false, false, 1);
        printf("# maint park deep err=%s\n", esp_err_to_name(err));
        return;
    }

    if (park) {
        s_parked = true;
        const esp_err_t err = app_display_request_blank();
        printf("# maint park white err=%s\n", esp_err_to_name(err));
    }
}

// ------------------------------------------------------------------------------- public

esp_err_t app_maint_init(void)
{
    s_course_active = false;
    s_seq = SEQ_COURSE;
    // So that a status read before anything has run reports the course's length rather than 0, which
    // a client would divide by.
    s_steps = EPD_MAINT_COURSE_STEPS;
    s_step = 0;
    s_courses = 0;
    s_sched_known = false;
    s_parked = false;
    s_want_start = false;
    s_want_clear = false;
    s_want_cycles = 1;
    s_want_stop = false;
    return ESP_OK;
}

void app_maint_tick(void)
{
    const uint32_t now = now_ms();

    // The requests first, so a stop posted during a course takes effect before another step goes out.
    if (s_want_stop) {
        s_want_stop = false;
        if (s_course_active) {
            course_finish("stopped");
        }
    }
    if (s_want_start) {
        s_want_start = false;
        // A manually started COURSE is always primed with white: a person triggering it has a
        // photograph on the glass, and the first flat read through it would show the photograph's
        // ghost -- which is the one thing that mode must not produce, since its whole value is that
        // what a person sees is the ink.
        //
        // A CLEAR needs no prime, because its own step 0 is white. Priming it would be an eighth
        // refresh that repeats the first.
        const bool clear = s_want_clear;
        course_start(clear ? SEQ_CLEAR : SEQ_COURSE, s_want_spread, !clear, s_want_cycles);
    }

    if (!s_sched_known || (uint32_t)(now - s_sched_checked_ms) >= SCHED_POLL_MS) {
        s_sched_checked_ms = now;
        const bool active = app_clock_schedule_active();
        if (!s_sched_known) {
            // The first observation establishes the state and is not an edge. In practice it is
            // almost always `true`, because the schedule fails open until the clock syncs.
            s_sched_known = true;
            s_sched_active = active;
        } else if (s_sched_active && !active) {
            s_sched_active = false;
            on_window_closed();
        } else if (!s_sched_active && active) {
            s_sched_active = true;
            s_parked = false;
        }
    }

    if (s_course_active) {
        course_step(now);
    }
}

esp_err_t app_maint_start_course(bool spread)
{
    // Refused here rather than on the tick, because the caller is an HTTP handler that has to answer
    // 503 now. `s_want_start` is checked as well as `s_course_active`: a second POST arriving inside
    // the same 200 ms window would otherwise be accepted and then silently do nothing.
    if (s_course_active || s_want_start) {
        return ESP_ERR_INVALID_STATE;
    }
    s_want_spread = spread;
    s_want_clear = false;
    s_want_cycles = 1;
    s_want_start = true;
    return ESP_OK;
}

esp_err_t app_maint_start_clear(int cycles)
{
    // **The cycles count is validated HERE and not on the tick**, because the route has to answer
    // 400 now -- a bad count that reached the tick would be refused where nobody is listening.
    if (epd_maint_clear_steps(cycles) <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Same door, same refusal, same reason as the course above -- the two sequences share every
    // mechanism in this file and only one can hold the panel.
    if (s_course_active || s_want_start) {
        return ESP_ERR_INVALID_STATE;
    }
    // Back to back, and unlike the course that is not a compromise: §4's thermal caveat exists
    // because a heated panel shows washed-out COLOUR and that would read as a fault on a mode whose
    // output a person judges. A clear is white and black, nobody judges its colour, and a continuous
    // cycle is what a clear is for.
    s_want_spread = false;
    s_want_clear = true;
    s_want_cycles = cycles;
    s_want_start = true;
    return ESP_OK;
}

void app_maint_stop_course(void)
{
    s_want_stop = true;
}

int app_maint_sequence_steps(bool clearing, int cycles)
{
    return clearing ? epd_maint_clear_steps(cycles) : EPD_MAINT_COURSE_STEPS;
}

bool app_maint_course_running(void)
{
    return s_course_active;
}

void app_maint_get_status(app_maint_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    // Read from another task without a lock, which is safe because every field is a word or smaller
    // and the only string is derived from `s_step` by a pure function rather than copied out of a
    // buffer this module writes. The worst case is a readout that mixes two moments 200 ms apart,
    // which is what any poll of a running course gets anyway.
    out->running = s_course_active;
    out->clearing = s_seq == SEQ_CLEAR;
    out->spread = s_spread;
    out->step = s_step;
    // The ACTIVE sequence's length, not the course's: a client drawing "step 3 of 10" while a
    // seven-step clear runs would be wrong in the one place the two differ.
    out->steps = seq_steps();
    if (s_step > 0) {
        snprintf(out->colour, sizeof(out->colour), "%s", seq_name(s_step - 1));
    }
    out->courses = s_courses;
    out->window_closed = s_sched_known && !s_sched_active;
    out->parked = s_parked;
    if (s_course_active) {
        const int32_t left = (int32_t)(s_next_step_ms - now_ms());
        out->next_step_s = left > 0 ? (uint32_t)(left / 1000) : 0u;
    }
}
