// Does src/epd_auto.c choose what epdoptimize's buildLayeredSuggestion() chooses?
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
// Regenerate the fixtures: node tools/epdopt_reference.mjs --auto
//
// **The classification handed in is upstream's, not this project's.** Every fixture carries the
// metrics classifyImageStyle() produced, and this file feeds those straight to
// epd_auto_suggest(). So a failure here is a fault in the suggestion and cannot be one in
// src/epd_classify.c -- which test/test_classify/ covers separately, against its own stated
// tolerances. Chaining the two would make every failure ambiguous.
//
// That also means **these are equality assertions, not tolerance bands.** The parameters
// upstream produces are decimal literals off its preset tables and its two rounding helpers
// (`Number(Math.log2(m).toFixed(3))` and `Number((m - 1).toFixed(3))`), so a correct port
// reproduces them exactly once both sides are float. The only float epsilon needed is the one
// TEST_ASSERT_EQUAL_FLOAT already applies.
//
// The generator refuses to write the header unless the fixture set reaches every arm -- all seven
// kinds, the photo arm's three branches, both sides of the quantization guard and of white
// preservation, the level and paper stages, and the palette override. Read its coverage table
// before believing a pass: threshold code tested from one side of every threshold is a suite that
// cannot fail.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "epd_auto.h"
#include "epd_dither.h"

#include "auto_fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// Fixture palette index -> the table src/app_display.c would hand in. 0 is what the frame ships;
// 1 is the arm for applyPaletteTuning's `lumaRange <= 150` override, which fires for `manual`
// (130.6) and not for `aitjcize` (195.9).
static const epd_palette_entry_t *palette_of(int index)
{
    return index == 0 ? EPD_PALETTE_EPDOPT_AITJCIZE : EPD_PALETTE_MANUAL;
}

// The fixture's classification, restricted to the four things the suggestion reads. kind_scores
// and confidence are deliberately left zero: if epd_auto.c ever starts reading one, this test
// stops being able to see it and the fixture has to grow -- which is a change worth noticing.
static epd_classification_t classification_of(const auto_fixture_t *f)
{
    epd_classification_t c;
    memset(&c, 0, sizeof(c));
    c.style = (epd_image_style_t)f->style;
    c.kind = (epd_image_kind_t)f->kind;
    c.photo_score = f->photo_score;

    const float *m = f->metrics;
    c.metrics.unique_colour_ratio = m[0];
    c.metrics.top_colour_coverage = m[1];
    c.metrics.palette_entropy = m[2];
    c.metrics.flat_ratio = m[3];
    c.metrics.soft_change_ratio = m[4];
    c.metrics.strong_edge_ratio = m[5];
    c.metrics.edge_density = m[6];
    c.metrics.horizontal_edge_ratio = m[7];
    c.metrics.vertical_edge_ratio = m[8];
    c.metrics.luma_std_dev = m[9];
    c.metrics.luma_p05 = m[10];
    c.metrics.luma_p95 = m[11];
    c.metrics.luma_range = m[12];
    c.metrics.saturation_mean = m[13];
    c.metrics.saturation_std_dev = m[14];
    c.metrics.dark_ratio = m[15];
    c.metrics.light_ratio = m[16];
    c.metrics.gray_ratio = m[17];
    c.metrics.high_saturation_ratio = m[18];
    c.metrics.warm_paper_ratio = m[19];
    c.metrics.red_ratio = m[20];
    c.metrics.dark_neutral_ratio = m[21];
    c.metrics.photo_tile_ratio = m[22];
    c.metrics.flat_tile_ratio = m[23];
    c.metrics.text_tile_ratio = m[24];
    c.metrics.gradient_tile_ratio = m[25];
    return c;
}

static const char *label(const auto_fixture_t *f)
{
    static char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s/%s", f->name, f->palette == 0 ? "aitjcize" : "manual");
    return buffer;
}

// The mapping from the header's integer columns is spelled out once here rather than at every
// assertion, because an off-by-one in it would make every test agree about the wrong thing.
// TONE order is epd_tone_mode_t: unset, off, contrast, scurve. RANGE is epd_range_mode_t: off,
// display, auto. Both match the generator's TONE_MODE_ORDER / RANGE_MODE_ORDER.
static void check_modes(const auto_fixture_t *f, const epd_auto_plan_t *plan)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(f->kind, (int)plan->kind, label(f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(f->tone_mode, (int)plan->adjust.tone_mode, label(f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(f->range_mode, (int)plan->adjust.range_mode, label(f));
}

static void test_every_fixture_agrees_on_the_modes(void)
{
    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);
        check_modes(f, &plan);
    }
}

static void test_every_fixture_agrees_on_the_tone_curve(void)
{
    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);

        TEST_ASSERT_TRUE_MESSAGE(plan.adjust.tone_enabled, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[0], plan.adjust.exposure, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[1], plan.adjust.saturation, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[2], plan.adjust.contrast, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[3], plan.adjust.strength, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[4], plan.adjust.shadow_boost, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[5], plan.adjust.highlight_compress, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->tone[6], plan.adjust.midpoint, label(f));
    }
}

static void test_every_fixture_agrees_on_range_compression(void)
{
    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);

        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->range[0], plan.adjust.range_strength, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->range[1], plan.adjust.low_percentile, label(f));
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->range[2], plan.adjust.high_percentile, label(f));
        // The palette travels through unchanged: epd_adjust_range() needs its endpoints, and a
        // plan carrying the wrong palette would compress into the wrong range.
        TEST_ASSERT_EQUAL_PTR_MESSAGE(palette_of(f->palette), plan.adjust.palette, label(f));
    }
}

static void test_every_fixture_agrees_on_the_three_extra_stages(void)
{
    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);

        TEST_ASSERT_EQUAL_INT_MESSAGE(f->level_enabled, plan.level.enabled ? 1 : 0, label(f));
        if (f->level_enabled) {
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->level[0], plan.level.black, label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->level[1], plan.level.white, label(f));
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE(f->paper_enabled, plan.paper.enabled ? 1 : 0, label(f));
        if (f->paper_enabled) {
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->paper[0], plan.paper.strength, label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->paper[1], plan.paper.min_luma, label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->paper[2], plan.paper.saturation_threshold,
                                            label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->paper[3], plan.paper.warm_bias_threshold,
                                            label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->paper[4], plan.paper.black_anchor, label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->paper[5], plan.paper.preserve_red, label(f));
            for (int ch = 0; ch < 3; ch++) {
                TEST_ASSERT_EQUAL_INT_MESSAGE(f->paper_white[ch],
                                              (int)plan.paper.paper_white[ch], label(f));
            }
        }

        TEST_ASSERT_EQUAL_INT_MESSAGE(f->white_enabled, plan.white.enabled ? 1 : 0, label(f));
        if (f->white_enabled) {
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->white[0], plan.white.percentile, label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->white[1], plan.white.min_luma, label(f));
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(f->white[2], plan.white.max_saturation, label(f));
        }
    }
}

static void test_every_fixture_agrees_on_the_output_choices(void)
{
    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);

        TEST_ASSERT_EQUAL_INT_MESSAGE(f->nearest, plan.nearest ? 1 : 0, label(f));
        TEST_ASSERT_EQUAL_INT_MESSAGE(f->serpentine, plan.serpentine ? 1 : 0, label(f));
        TEST_ASSERT_EQUAL_INT_MESSAGE(f->wanted_lab, plan.wanted_lab ? 1 : 0, label(f));
        TEST_ASSERT_EQUAL_INT_MESSAGE(f->wanted_stucki, plan.wanted_stucki ? 1 : 0, label(f));
    }
}

// White preservation is on for every plan whose range mode is not off, and off for every plan
// whose range mode is off. That is enforceAutoWhitePreservation's whole content, and it is
// asserted separately from the fixture comparison because it is a *relationship* -- a port that
// got both the range mode and the flag wrong in the same direction would satisfy the fixtures
// and fail this.
static void test_white_preservation_follows_the_range_mode(void)
{
    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);

        const bool range_off = plan.adjust.range_mode == EPD_RANGE_MODE_OFF;
        TEST_ASSERT_EQUAL_INT_MESSAGE(range_off ? 0 : 1, plan.white.enabled ? 1 : 0, label(f));
    }
}

// **The one rule that is this project's and not upstream's, and the one that silently ruins a
// picture if it is wrong.** epd_render_t.tone is display-mode range compression in integers, so
// with epd_adjust_range() also running the image would be squeezed into the panel's range twice.
// Turning it off unconditionally is not the answer either: when auto asks for no range
// compression, the row path's copy is the only one there is, and ticket 19 measured that
// compression as the win. So: exactly one, always.
//
// **Both quantiser paths are checked, because the diffusion path applies the same value somewhere
// else.** The row path puts it in epd_render_t.tone and matches per pixel; the diffusion path
// applies it to the region up front and then packs at EPD_TONE_NONE unconditionally. A plan that
// got a compression from epd_adjust_range() *and* a non-zero pre-tone would be compressed twice on
// the diffusion path just as surely as on the row path, so the count below covers both and the
// arm that would break is flatIllustration -- range=off and NOT nearest, so it is the one kind
// that reaches the diffuser with the row path's compensation as its only compression.
static void test_row_tone_gives_exactly_one_range_compression(void)
{
    int with_stage = 0;
    int without_stage = 0;
    int diffused_without_stage = 0;

    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        const epd_classification_t c = classification_of(f);
        epd_auto_plan_t plan;
        epd_auto_suggest(&c, palette_of(f->palette), &plan);

        const uint8_t tone = epd_auto_row_tone(&plan, EPD_TONE_FULL);
        if (plan.adjust.range_mode == EPD_RANGE_MODE_OFF) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(EPD_TONE_FULL, tone, label(f));
            without_stage++;
            if (!plan.nearest) {
                diffused_without_stage++;
            }
        } else {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(EPD_TONE_NONE, tone, label(f));
            with_stage++;
        }
    }

    // The case the diffusion path must not drop on the floor. If this ever reaches zero, either
    // the palette guard swallowed every range=off arm or the kind table changed, and the pre-pass
    // in src/app_display.c has become untested rather than unnecessary.
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(
        0, diffused_without_stage,
        "no fixture reaches the diffuser with range=off; the pre-tone pass is now untested");

    // Both sides have to be represented or this test is an assertion about one branch wearing
    // the shape of two. The generator's `range:off` and `range:display` requirements are what
    // keep this satisfiable, and this is where it is checked rather than assumed.
    TEST_ASSERT_TRUE(with_stage > 0);
    TEST_ASSERT_TRUE(without_stage > 0);

    // A palette that carries no compression of its own keeps carrying none.
    epd_auto_plan_t off_plan;
    memset(&off_plan, 0, sizeof(off_plan));
    off_plan.adjust.range_mode = EPD_RANGE_MODE_OFF;
    TEST_ASSERT_EQUAL_UINT8(EPD_TONE_NONE, epd_auto_row_tone(&off_plan, EPD_TONE_NONE));
    TEST_ASSERT_EQUAL_UINT8(EPD_TONE_HALF, epd_auto_row_tone(&off_plan, EPD_TONE_HALF));
    // And a NULL plan means "no auto", so the palette's own setting is what applies.
    TEST_ASSERT_EQUAL_UINT8(EPD_TONE_FULL, epd_auto_row_tone(NULL, EPD_TONE_FULL));
}

// applyPaletteTuning's override is the reason `palette` is an argument at all. At least one
// fixture pair has to disagree between the two palettes, or the argument is decoration.
static void test_the_palette_changes_the_answer(void)
{
    int differing = 0;

    for (size_t i = 0; i < AUTO_FIXTURE_COUNT; i++) {
        const auto_fixture_t *f = &AUTO_FIXTURES[i];
        if (f->palette != 0) {
            continue;
        }
        const epd_classification_t c = classification_of(f);

        epd_auto_plan_t wide;
        epd_auto_plan_t narrow;
        epd_auto_suggest(&c, EPD_PALETTE_EPDOPT_AITJCIZE, &wide);
        epd_auto_suggest(&c, EPD_PALETTE_MANUAL, &narrow);

        if (wide.adjust.range_mode != narrow.adjust.range_mode ||
            wide.adjust.range_strength != narrow.adjust.range_strength) {
            differing++;
            printf("# palette changes %s: %d@%.2f -> %d@%.2f\n", f->name,
                   (int)wide.adjust.range_mode, (double)wide.adjust.range_strength,
                   (int)narrow.adjust.range_mode, (double)narrow.adjust.range_strength);
        }
    }

    TEST_ASSERT_TRUE(differing > 0);
}

// No palette at all is upstream's `null` profile: applyPaletteTuning does nothing, so a kind
// that asked for no range compression still gets none. It is not a state app_display.c can
// produce, and it is the one input that could reasonably crash.
static void test_a_null_palette_is_accepted(void)
{
    epd_classification_t c;
    memset(&c, 0, sizeof(c));
    c.kind = EPD_KIND_PIXEL_ART;
    c.metrics.luma_range = 200.0f;
    c.metrics.luma_std_dev = 60.0f;

    epd_auto_plan_t plan;
    memset(&plan, 0xa5, sizeof(plan));
    epd_auto_suggest(&c, NULL, &plan);

    TEST_ASSERT_EQUAL_INT(EPD_RANGE_MODE_OFF, (int)plan.adjust.range_mode);
    TEST_ASSERT_EQUAL_INT(EPD_TONE_MODE_OFF, (int)plan.adjust.tone_mode);
    TEST_ASSERT_NULL(plan.adjust.palette);
    TEST_ASSERT_FALSE(plan.white.enabled);
}

// A NULL argument leaves the caller's struct alone rather than half-filling it. The display task
// keeps its plan across a render; a partially written one would be worse than an untouched one.
static void test_refuses_rather_than_half_writing(void)
{
    epd_auto_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.kind = EPD_KIND_LINE_ART;

    epd_auto_suggest(NULL, EPD_PALETTE_EPDOPT_AITJCIZE, &plan);
    TEST_ASSERT_EQUAL_INT(EPD_KIND_LINE_ART, (int)plan.kind);

    epd_classification_t c;
    memset(&c, 0, sizeof(c));
    epd_auto_suggest(&c, EPD_PALETTE_EPDOPT_AITJCIZE, NULL);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_every_fixture_agrees_on_the_modes);
    RUN_TEST(test_every_fixture_agrees_on_the_tone_curve);
    RUN_TEST(test_every_fixture_agrees_on_range_compression);
    RUN_TEST(test_every_fixture_agrees_on_the_three_extra_stages);
    RUN_TEST(test_every_fixture_agrees_on_the_output_choices);
    RUN_TEST(test_white_preservation_follows_the_range_mode);
    RUN_TEST(test_row_tone_gives_exactly_one_range_compression);
    RUN_TEST(test_the_palette_changes_the_answer);
    RUN_TEST(test_a_null_palette_is_accepted);
    RUN_TEST(test_refuses_rather_than_half_writing);

    return UNITY_END();
}
