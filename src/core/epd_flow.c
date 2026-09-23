#include "epd_flow.h"

// A clock the caller did not supply reads as zero, which makes every duration zero rather than a
// difference between two garbage values. Kept as a function so the six brackets below read the same
// whether or not anything is being timed.
static int64_t tick(epd_flow_clock_t clock)
{
    return clock ? clock() : 0;
}

void epd_flow_apply(const epd_flow_region_t *region, const epd_auto_plan_t *plan,
                    epd_white_plan_t *white_out, epd_flow_clock_t clock,
                    epd_flow_timing_t *timing)
{
    if (!region || !region->rgb || !plan) {
        return;
    }

    // The plan is wanted by the caller's log line and by the last stage. A local stands in when the
    // caller does not want it, so epd_adjust_white_apply() always has something to read -- it is a
    // no-op for an inactive plan, so nothing here branches on it.
    epd_white_plan_t local;
    epd_white_plan_t *white = white_out ? white_out : &local;
    *white = (epd_white_plan_t){0};

    uint8_t *rgb = region->rgb;
    const size_t w = region->width;
    const size_t h = region->height;
    const size_t stride = region->stride;

    // ------------------------------------------------------------------------------------------
    // **THE ORDER. Do not reorder these six lines.** Each constraint is epd_adjust.h's, cited at
    // the stage it binds:
    //
    //   1. white_plan FIRST, because it measures the UNTOUCHED source (:170-172). Anything above it
    //      changes the p99 it latches onto, and the picture then keeps a "white" that came from
    //      already-adjusted pixels.
    //   2. paper before tone (:157) -- it runs only for the posterScan arm.
    //   3. tone before range (:144-145): "reversing them would compress a range the tone curve then
    //      moves back out of."
    //   4. level AFTER range (:161-164), not fused into the tone LUT: "That is not a choice."
    //   5. white_apply LAST of all (:170-172), after every other stage has run.
    //
    // A swap here is silent everywhere else in the build. The suite that guards it is
    // test/test_flow, which pins the first and the last by observation rather than by restating the
    // list: the plan must equal one taken on a pristine copy, and a marked pixel must come out
    // exactly the palette's white.
    // ------------------------------------------------------------------------------------------
    const int64_t t0 = tick(clock);
    epd_adjust_white_plan(rgb, w, h, stride, region->palette, &plan->white, region->white_bits,
                          region->white_bits_bytes, white);
    const int64_t t1 = tick(clock);
    epd_adjust_paper_region(rgb, w, h, stride, &plan->paper);
    const int64_t t2 = tick(clock);
    epd_adjust_tone_region(rgb, w, h, stride, &plan->adjust);
    const int64_t t3 = tick(clock);
    epd_adjust_range_region(rgb, w, h, stride, &plan->adjust);
    const int64_t t4 = tick(clock);
    epd_adjust_level_region(rgb, w, h, stride, &plan->level);
    const int64_t t5 = tick(clock);
    epd_adjust_white_apply(rgb, w, h, stride, region->white_bits, white);
    const int64_t t6 = tick(clock);

    if (timing) {
        timing->white_plan_us = t1 - t0;
        timing->paper_us = t2 - t1;
        timing->tone_us = t3 - t2;
        timing->range_us = t4 - t3;
        timing->level_us = t5 - t4;
        timing->white_apply_us = t6 - t5;
    }
}
