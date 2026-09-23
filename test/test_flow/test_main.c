// epd_flow_apply(): the auto flow's six pixel stages, in the one order they may run in.
//
// Worth its own suite because a reordering is invisible to everything else in this build. Each stage
// is host-tested on its own and none of those suites sees the order, so swapping two lines passes
// `pio test -e native`, passes all ten device builds, and shows up only as a picture a person has to
// judge. docs/agents/handover-auto-flow.md:41 records the gap in its own table -- the row for `the
// wiring` names app_display.c's auto_adjust_canvas() and leaves the fixture column empty.
//
// The order was written out by hand in three places until 2026-09-21 (app_display.c, photo_main.c,
// tools/render_preview.c) and verified by running all three and diffing the console. That agreement
// means "the port is right" only if the three are not hand-copies of each other.
//
// **The two cases that pin the order do it by observation, not by restating the list.** Restating it
// would pass against any implementation that restates it the same way:
//
//   * the white plan is measured on the UNTOUCHED source, so it must equal one taken on a pristine
//     copy of the same pixels;
//   * the white apply is LAST, so a pixel it marked must come out exactly the palette's white --
//     had tone or range run afterwards, it would have been moved off that value.
//
// **Each is preceded by a power check.** A fixture whose tone stage does not actually move pixels
// would pass both of them under either order, and would be measuring nothing;
// test_the_fixture_can_tell_the_orders_apart is the arithmetic that says it can, and it is written
// to fail first if the plan below ever goes inert.
//
// **Both cases were made to fail on purpose, 2026-09-21, and one of them said something worth
// keeping.** Moving epd_adjust_white_plan() to after the tone stage does not merely shift the
// threshold it measures: the tone stage darkens the fixture's paper below `min_luma`, so
// epd_adjust_white_plan() returns false and **white preservation turns off entirely** -- the case
// fails on `active` being 0 where the source gives 1, not on a number being slightly out. That is
// the shape of the defect this order protects against: not a subtly different white, but a feature
// silently absent from every photograph. Moving epd_adjust_white_apply() ahead of tone instead
// fails test_white_apply_is_last alone, with "a later stage moved its pixels", which is how the two
// constraints are known to be pinned independently rather than by one assertion doing both jobs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "epd_adjust.h"
#include "epd_auto.h"
#include "epd_dither.h"
#include "epd_flow.h"

#define W 16u
#define H 16u
#define PIXELS (W * H)
#define BYTES (PIXELS * 3u)

// A near-white, low-saturation block the white plan can latch onto, on a mid-grey field. Both are
// neutral, so `max_saturation` admits them and the p99 lands on the bright block.
#define FIELD 128u
#define PAPER 250u
// Rows 0..3 of 16 are the bright block: 64 of 256 pixels, comfortably above a p99's reach into the
// grey below it.
#define PAPER_ROWS 4u

static uint8_t s_buf[BYTES];
static uint8_t s_pristine[BYTES];
static uint8_t s_bits[BYTES];  // far more than epd_adjust_white_bits_bytes() needs; 1 bit per pixel

static int64_t s_fake_clock;
static int s_clock_calls;

static int64_t fake_clock(void)
{
    s_clock_calls++;
    return ++s_fake_clock;
}

static void fill(uint8_t *dst)
{
    for (size_t y = 0; y < H; y++) {
        const uint8_t v = (y < PAPER_ROWS) ? (uint8_t)PAPER : (uint8_t)FIELD;
        for (size_t x = 0; x < W; x++) {
            uint8_t *p = &dst[(y * W + x) * 3u];
            p[0] = v;
            p[1] = v;
            p[2] = v;
        }
    }
}

// Tone darkens hard (-2 stops) and range compresses, so both stages genuinely move pixels. The
// white stage is enabled with a min_luma below PAPER so the plan activates.
static epd_auto_plan_t make_plan(void)
{
    epd_auto_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.adjust.palette = EPD_PALETTE_EPDOPT_AITJCIZE;
    plan.adjust.tone_enabled = true;
    plan.adjust.tone_mode = EPD_TONE_MODE_CONTRAST;
    plan.adjust.exposure = -2.0f;
    plan.adjust.contrast = 0.5f;
    plan.adjust.midpoint = 0.5f;
    plan.adjust.range_mode = EPD_RANGE_MODE_DISPLAY;
    plan.adjust.range_strength = 1.0f;
    plan.white.enabled = true;
    plan.white.percentile = 0.99f;
    plan.white.min_luma = 200.0f;
    plan.white.max_saturation = 0.1f;
    // paper and level stay disabled: they are the two arms this fixture is not about, and leaving
    // them off keeps the two order assertions attributable to tone and range.
    return plan;
}

static epd_flow_region_t make_region(uint8_t *buf)
{
    epd_flow_region_t r;
    memset(&r, 0, sizeof(r));
    r.rgb = buf;
    r.width = W;
    r.height = H;
    r.stride = W * 3u;
    r.palette = EPD_PALETTE_EPDOPT_AITJCIZE;
    r.white_bits = s_bits;
    r.white_bits_bytes = epd_adjust_white_bits_bytes(W, H);
    return r;
}

void setUp(void)
{
    fill(s_buf);
    fill(s_pristine);
    memset(s_bits, 0, sizeof(s_bits));
    s_fake_clock = 0;
    s_clock_calls = 0;
}

void tearDown(void) {}

// ------------------------------------------------------------------ the power check, first

static void test_the_fixture_can_tell_the_orders_apart(void)
{
    // The refutation condition, computed before anything is concluded from the two cases below:
    // would a white plan taken AFTER the tone stage differ from one taken before it? If not, this
    // fixture cannot see the order at all and the assertions that follow would be theatre.
    const epd_auto_plan_t plan = make_plan();

    epd_white_plan_t before;
    TEST_ASSERT_TRUE_MESSAGE(epd_adjust_white_plan(s_pristine, W, H, W * 3u,
                                                  EPD_PALETTE_EPDOPT_AITJCIZE, &plan.white, s_bits,
                                                  epd_adjust_white_bits_bytes(W, H), &before),
                             "the fixture must give the white stage something to latch onto");
    TEST_ASSERT_TRUE_MESSAGE(before.active, "the plan must be active for this suite to mean anything");

    // Tone alone, on its own copy.
    uint8_t toned[BYTES];
    memcpy(toned, s_pristine, sizeof(toned));
    epd_adjust_tone_region(toned, W, H, W * 3u, &plan.adjust);

    bool tone_moved_pixels = false;
    for (size_t i = 0; i < BYTES; i++) {
        if (toned[i] != s_pristine[i]) {
            tone_moved_pixels = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(tone_moved_pixels,
                             "the tone stage is inert -- this fixture cannot detect a reordering");

    epd_white_plan_t after;
    epd_adjust_white_plan(toned, W, H, W * 3u, EPD_PALETTE_EPDOPT_AITJCIZE, &plan.white, s_bits,
                          epd_adjust_white_bits_bytes(W, H), &after);
    // The two must disagree. If they agree, running white_plan first or third is indistinguishable
    // here and test_white_plan_is_measured_on_the_untouched_source proves nothing.
    TEST_ASSERT_TRUE_MESSAGE(before.source_white_luma != after.source_white_luma,
                             "a plan before and after tone agree -- the fixture has no power");
}

// ------------------------------------------------------------------ the order, by observation

static void test_white_plan_is_measured_on_the_untouched_source(void)
{
    const epd_auto_plan_t plan = make_plan();

    epd_white_plan_t reference;
    epd_adjust_white_plan(s_pristine, W, H, W * 3u, EPD_PALETTE_EPDOPT_AITJCIZE, &plan.white,
                          s_bits, epd_adjust_white_bits_bytes(W, H), &reference);
    memset(s_bits, 0, sizeof(s_bits));

    epd_white_plan_t actual;
    const epd_flow_region_t region = make_region(s_buf);
    epd_flow_apply(&region, &plan, &actual, NULL, NULL);

    TEST_ASSERT_EQUAL_MESSAGE(reference.active, actual.active,
                              "the plan the flow reports is not the one the source gives");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, reference.source_white_luma, actual.source_white_luma,
                                     "white_plan did not run first -- it measured adjusted pixels");
}

static void test_white_apply_is_last(void)
{
    const epd_auto_plan_t plan = make_plan();

    epd_white_plan_t white;
    const epd_flow_region_t region = make_region(s_buf);
    epd_flow_apply(&region, &plan, &white, NULL, NULL);
    TEST_ASSERT_TRUE_MESSAGE(white.active, "an inactive plan makes this case vacuous");

    // At least one pixel must be EXACTLY the palette's white. Had white_apply run before tone or
    // range, those stages would have moved it off that value afterwards.
    bool found_exact_white = false;
    for (size_t i = 0; i < PIXELS; i++) {
        const uint8_t *p = &s_buf[i * 3u];
        if (p[0] == white.target[0] && p[1] == white.target[1] && p[2] == white.target[2]) {
            found_exact_white = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_exact_white,
                             "white_apply did not run last -- a later stage moved its pixels");
}

// ------------------------------------------------------------------ all six stages, bracketed

static void test_the_clock_brackets_all_six_stages(void)
{
    // A clock that counts makes the composition's shape observable without caring what any stage
    // does: seven readings means six brackets, and one microsecond each means none was skipped and
    // none was timed twice. This is the case that fails if a stage is dropped from the sequence.
    const epd_auto_plan_t plan = make_plan();
    epd_flow_timing_t timing;
    memset(&timing, 0xAA, sizeof(timing));

    const epd_flow_region_t region = make_region(s_buf);
    epd_flow_apply(&region, &plan, NULL, fake_clock, &timing);

    TEST_ASSERT_EQUAL_INT_MESSAGE(7, s_clock_calls, "six stages need seven readings");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, timing.white_plan_us, "white_plan was not bracketed");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, timing.paper_us, "paper was not bracketed");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, timing.tone_us, "tone was not bracketed");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, timing.range_us, "range was not bracketed");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, timing.level_us, "level was not bracketed");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, timing.white_apply_us, "white_apply was not bracketed");
}

static void test_no_clock_leaves_every_duration_zero(void)
{
    // Zero rather than the difference between two garbage readings, so a caller that forgot the
    // clock reads an obvious nothing instead of a plausible number.
    const epd_auto_plan_t plan = make_plan();
    epd_flow_timing_t timing;
    memset(&timing, 0xAA, sizeof(timing));

    const epd_flow_region_t region = make_region(s_buf);
    epd_flow_apply(&region, &plan, NULL, NULL, &timing);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_clock_calls, "no clock was supplied");
    TEST_ASSERT_EQUAL_INT64(0, timing.white_plan_us);
    TEST_ASSERT_EQUAL_INT64(0, timing.paper_us);
    TEST_ASSERT_EQUAL_INT64(0, timing.tone_us);
    TEST_ASSERT_EQUAL_INT64(0, timing.range_us);
    TEST_ASSERT_EQUAL_INT64(0, timing.level_us);
    TEST_ASSERT_EQUAL_INT64(0, timing.white_apply_us);
}

// ------------------------------------------------------------------ refusals

static void test_a_null_region_or_plan_does_nothing(void)
{
    const epd_auto_plan_t plan = make_plan();
    epd_flow_region_t region = make_region(s_buf);

    epd_flow_apply(NULL, &plan, NULL, fake_clock, NULL);
    epd_flow_apply(&region, NULL, NULL, fake_clock, NULL);
    region.rgb = NULL;
    epd_flow_apply(&region, &plan, NULL, fake_clock, NULL);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_clock_calls, "a refused call runs no stage");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(s_pristine, s_buf, BYTES, "a refused call touches no pixel");
}

static void test_the_other_five_stages_run_without_white_scratch(void)
{
    // epd_adjust.h:191-194: a missing `bits` makes the white plan decline rather than write wildly.
    // The rest of the flow still has to happen, because a render that cannot preserve white is
    // still a render -- the same argument app_display.c makes for its PSRAM scratch.
    const epd_auto_plan_t plan = make_plan();
    epd_flow_region_t region = make_region(s_buf);
    region.white_bits = NULL;
    region.white_bits_bytes = 0;

    epd_white_plan_t white;
    epd_flow_apply(&region, &plan, &white, NULL, NULL);

    TEST_ASSERT_FALSE_MESSAGE(white.active, "no scratch means no plan");
    bool changed = false;
    for (size_t i = 0; i < BYTES; i++) {
        if (s_buf[i] != s_pristine[i]) {
            changed = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(changed, "tone and range must still have run");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_fixture_can_tell_the_orders_apart);
    RUN_TEST(test_white_plan_is_measured_on_the_untouched_source);
    RUN_TEST(test_white_apply_is_last);
    RUN_TEST(test_the_clock_brackets_all_six_stages);
    RUN_TEST(test_no_clock_leaves_every_duration_zero);
    RUN_TEST(test_a_null_region_or_plan_does_nothing);
    RUN_TEST(test_the_other_five_stages_run_without_white_scratch);
    return UNITY_END();
}
