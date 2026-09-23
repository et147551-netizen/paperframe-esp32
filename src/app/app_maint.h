// Panel maintenance: a colour course a person looks at, and an inactive window that rests on white.
//
// Ticket 68, the owner's request of 2026-09-20. Two things, one module, because both are edits to
// the same seam -- the edge of the active-hours window -- and because the second makes the first
// cheaper.
//
//   (a) THE COURSE. Ten full-screen flats in the panel's own native colours, palette and quantiser
//       bypassed: K W R W Y W G W B W (src/core/epd_maint_course.h owns the order and the argument
//       for it). Run by hand from the Web UI at any hour, and once a week spread across one hour
//       inside the closed window. It is DIAGNOSTIC and PREVENTATIVE -- no ghosting complaint
//       prompted it, and photograph-to-photograph ghosting has never been looked at here and must
//       not be assumed either way.
//
//   (b) THE WHITE STANDBY. The panel holds its image with no power, so the window's `hold=1` leaves
//       the last photograph on the glass for the whole eight hours -- exactly the image-sticking
//       condition GooDisplay's precautions warn about. White instead, which also clears the previous
//       render completely (docs/measurements.md:123-133) so every morning starts clean.
//
// **NO TASK OF ITS OWN, and that is a memory decision rather than a style one.** Internal RAM is the
// scarce resource on this board and two small tasks were once enough to make httpd_start() return
// ESP_ERR_HTTPD_TASK -- a frame that booted with a panel, a slideshow and no web UI at all
// (docs/board-and-storage.md, ticket 21). So this is a tick from the application task, the
// same place ticket 55's LED poll lives. A course spends its whole length waiting, which is what
// makes that affordable: ten refreshes and nine gaps, and nothing in between.
//
// **IT IS NOT A NEW SUPPRESSION.** The schedule suppresses the automatic advance and nothing else,
// deliberately -- a button press, an API call and a manual next all still work while the window is
// closed, because a frame that goes unresponsive at 23:00 is one the owner cannot fix at 23:01
// (app_slideshow.c). Both halves here are ONE-SHOT ACTIONS taken at a window edge or on request, so
// nothing in app_slideshow.c changes.
//
// **It is also not a power feature.** The window does not sleep the SoC -- esp_sleep appears nowhere
// in this application, and Wi-Fi, the web server, the heartbeat and the catalogue all keep running
// through it. Against that baseline ten refreshes are noise. The refresh COUNT is the real cost
// (~16/day becomes ~18.4), which is why the course is weekly and not nightly; ticket 68 §5 has the
// arithmetic, and the caveat that power cannot be measured on this bench at all.

#ifndef APP_MAINT_H
#define APP_MAINT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool running;
    // Which sequence is running: the ten-flat COURSE, or the white/black CLEAR. They share every
    // field below, so `steps` is the active one's length — ten for a course, and `6N+1` for a clear
    // of N cycles. **A client must not assume ten, and must not assume seven either.**
    bool clearing;
    bool spread;          // one hour between first and last step, rather than back to back
    int step;             // steps requested so far, 0..steps
    int steps;            // the ACTIVE sequence's length, so a client needs no constant of its own
    char colour[8];       // the colour last requested, "" before the first step
    uint32_t next_step_s; // seconds until the next step; 0 when due, idle, or waiting on the panel
    uint32_t courses;      // courses completed since boot
    bool window_closed;   // the active-hours window is closed as far as this module has seen
    bool parked;          // the panel was parked white at the last window edge
} app_maint_status_t;

// Latches only -- no allocation, no NVS, no panel. Call after app_settings_init() and
// app_display_init(); safe to call once.
esp_err_t app_maint_init(void);

// From the application task's 200 ms loop, beside app_slideshow_update(). Cheap when idle: the
// clock and the settings are consulted once every 10 s, because the decision is about hours.
void app_maint_tick(void);

// Starts a course now. `spread` puts an hour between the first and last step; false runs them back to
// back, which is what a person watching the glass wants and is what the Web UI asks for.
//
// Returns immediately -- the steps are driven by the tick. ESP_ERR_INVALID_STATE if one is already
// running, which the route turns into a 503.
//
// **A manually started course primes the panel with white first**, because whatever photograph is on
// the glass would otherwise be read through the first flat. The scheduled course does not need that:
// it follows this module's own white park on the same window edge.
esp_err_t app_maint_start_course(bool spread);

// Starts a CLEAR CYCLE now: white, black, white, black, white, black, white — three black passes each
// bracketed by white, back to back, ending on white. Owner's request of 2026-09-20, after asking
// whether the existing mechanisms could deal with mild ghosting.
//
// **`cycles` repeats that, 1..EPD_MAINT_CLEAR_CYCLES_MAX, and it is a COUNT rather than a duration on
// purpose.** The request was for a continuous mode; the answer is bounded, because a duration is not
// board-independent — 8 h of continuous clearing is 1,920 refreshes here and 932 on the E1002 — and
// because a count is the thing that might help as well as the thing that costs. A count outside the
// range is ESP_ERR_INVALID_ARG and nothing starts, which is what makes the route's 400 honest.
// `epd_maint_clear_steps()` has the arithmetic and the reason the bound is five.
//
// **This is an escalation and not the first line.** One white render is MEASURED to remove a plainly
// legible 1-bit black-on-white ghost completely (ticket 30, 2026-09-10), and the nightly standby
// already does one; `{"action":"white"}` does one on demand. This exists for the case beyond that.
//
// **AND THAT CASE HAS NEVER BEEN PRODUCED HERE.** The owner declined to manufacture a ghost to test
// it, so nothing measures whether three black passes beat one white. It rests on the vendor precaution
// and on every render being a full-frame DRF; cite it as a mechanism, never as a remedy.
//
// Same refusal as the course — only one sequence can hold the panel — and it needs no leading white
// because its own first step is one.
esp_err_t app_maint_start_clear(int cycles);

// Abandons a course. A refresh already in flight finishes -- there is no way to stop one and no
// reason to want to -- so the panel is left showing whichever flat was last requested.
void app_maint_stop_course(void);

bool app_maint_course_running(void);

// How many steps a sequence has, without needing it to be running.
//
// **For a caller that has just QUEUED one**, which is the route: the two start verbs hand a flag to
// the tick and return, so for up to 200 ms the status still describes whatever ran last — and a
// `{"action":"clear"}` that answered `"steps":10` off that status was the first version of this and
// was simply wrong. Observed on hardware 2026-09-20.
int app_maint_sequence_steps(bool clearing, int cycles);

void app_maint_get_status(app_maint_status_t *out);

#endif // APP_MAINT_H
