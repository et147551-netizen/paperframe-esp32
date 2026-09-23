// The auto flow's pixel stages, in the one order they may run in.
//
// epd_adjust.h owns each stage and documents the order three times over, in prose, at the stages it
// constrains:
//
//     :144  "Callers run tone before range: reversing them would compress a range the tone curve
//            then moves back out of."
//     :161  "Runs **after** range compression, not fused into the tone LUT. That is not a choice."
//     :170  "a plan measured on the **untouched source**, and an application after every other
//            stage has run. Between them the source is gone, so something has to be remembered."
//
// Documented, and until 2026-09-21 enforced nowhere. The sequence was written out by hand in THREE
// places -- app_display.c's auto_adjust_canvas() on the device, photo_main.c's timing arm, and
// tools/render_preview.c on the host -- and the project's own verification for it was to run all
// three and diff the console: .scratch/digital-frame/compare_plans.py, fourteen fixtures,
// device against host. That agreement means "the port is right" only if the three are not three
// hand-copies of one another, which is what this module makes true.
//
// **A reordering is invisible to every other check in the build.** Each stage is host-tested on its
// own -- 443 cases across 25 suites -- and none of them sees the order, so swapping two lines passes
// `pio test -e native`, passes all ten device builds, and shows up as a picture a person has to
// judge. docs/handover-auto-flow.md:41 records the gap in its own table: the row for `the
// wiring` names app_display.c's auto_adjust_canvas() and leaves the fixture column empty. Two of
// the existing suites can only assert that a WITNESS still exists for a rule enforced here --
// test_auto/test_main.c:262 fails with "no fixture reaches the diffuser with range=off; the pre-tone
// pass is now untested".
//
// Three callers, one of them a host tool compiled by tools/render_preview.py with its own hardcoded
// source list, which is why this file is in src/core/: ESP-IDF-free, so it links under env:native
// and under clang alongside epd_adjust.c.
//
// **What this module is not.** It does not decide anything -- epd_auto_suggest() produces the plan
// and epd_classify_canvas() the classification, both before this runs, and neither is part of the
// order under protection. It does not touch the canvas outside the region it is given: the region
// form of every stage is the one the device uses, because the canvas is matted white and two of
// these stages measure statistics over the pixels they are handed (epd_adjust.h:132-137). And it
// does not apply epd_auto_row_tone(), which is a decision about the render config rather than a
// pixel stage -- that is already one function with three callers, which is the right shape.

#ifndef EPD_FLOW_H
#define EPD_FLOW_H

#include <stdint.h>
#include <stddef.h>

#include "epd_adjust.h"
#include "epd_auto.h"

// The rectangle the stages run over, and the scratch the white plan needs.
//
// Grouped into a struct rather than passed as seven arguments because all three callers compute the
// same seven values from a canvas and a fit, and an interface a caller has to get right seven times
// is one this module would not be shortening.
typedef struct {
    // The region's top-left pixel INSIDE the caller's buffer, not the buffer's start.
    uint8_t *rgb;
    size_t width;
    size_t height;
    // Bytes per row of the BUFFER that contains the region -- the whole canvas width times three,
    // not the region's. epd_canvas_physical_rect() is how a caller turns a logical rectangle into
    // the origin and the dimensions above.
    size_t stride;
    const epd_palette_entry_t *palette;
    // At least epd_adjust_white_bits_bytes(width, height). A short or absent buffer makes the white
    // plan decline, which is a refusal rather than a wild write (epd_adjust.h:191-194), and the
    // other five stages still run.
    uint8_t *white_bits;
    size_t white_bits_bytes;
} epd_flow_region_t;

// Microseconds, so the caller can supply its own. src/core/ is ESP-IDF-free and has no clock of its
// own; the device passes esp_timer_get_time and the host passes NULL.
typedef int64_t (*epd_flow_clock_t)(void);

// Per-stage durations, in the order the stages run. All zero when no clock was supplied.
//
// This exists because photo_main.c's whole purpose is these six numbers -- they are what
// docs/measurements.md quotes -- so a composition that could not report them would have left
// the timing arm as a fourth hand-written copy of the order.
typedef struct {
    int64_t white_plan_us;
    int64_t paper_us;
    int64_t tone_us;
    int64_t range_us;
    int64_t level_us;
    int64_t white_apply_us;
} epd_flow_timing_t;

// Runs the six stages over `region`, in order.
//
// `white_out` receives the plan that was measured on the untouched source, for the caller's log
// line; both it and `timing` and `clock` may be NULL. Does nothing when `region`, `region->rgb` or
// `plan` is NULL.
void epd_flow_apply(const epd_flow_region_t *region, const epd_auto_plan_t *plan,
                    epd_white_plan_t *white_out, epd_flow_clock_t clock,
                    epd_flow_timing_t *timing);

#endif  // EPD_FLOW_H
