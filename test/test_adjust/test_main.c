// Is src/epd_adjust.c the same function as src/epd_epdopt.c's first two stages?
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// **The comparison here has a tolerance, and that is the design rather than a concession.**
// epd_adjust.c is single precision so that it can run on the device; epd_epdopt.c is the
// byte-for-byte port of the library and is pinned to it by test/test_epdopt. Chaining the two
// is what lets "close to upstream" be checked without claiming "identical to upstream", which
// for a float rewrite would simply be false.
//
// So the reference in this file is the double implementation, not a fixture header, and it is
// computed at test time from the same input. Nothing here can be made to pass by editing an
// expected value -- there are none.
//
// The bounds below were written down before the first run and are asserted as maxima, and each
// case prints its measured maximum and mean so a regression shows up as a number moving rather
// than as a pass. **If a bound is exceeded, the rewrite is wrong; do not raise the bound.**

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unity.h>

#include "epd_adjust.h"
#include "epd_canvas.h"
#include "epd_dither.h"
#include "epd_epdopt.h"

#include "stage_fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// A strided sweep of the whole RGB cube: 16 levels per channel, every combination. 4096
// pixels covers the neutral axis, all six primaries, both ends of the chroma guard's
// smoothstep and the clipping corners, which a photograph does not.
#define SWEEP_STEPS 16
#define SWEEP_PIXELS (SWEEP_STEPS * SWEEP_STEPS * SWEEP_STEPS)

static uint8_t s_float_buf[SWEEP_PIXELS * 3];
static uint8_t s_double_buf[SWEEP_PIXELS * 3];

static void fill_sweep(uint8_t *out)
{
    size_t i = 0;
    for (int r = 0; r < SWEEP_STEPS; r++) {
        for (int g = 0; g < SWEEP_STEPS; g++) {
            for (int b = 0; b < SWEEP_STEPS; b++) {
                out[i * 3 + 0] = (uint8_t)(r * 17);
                out[i * 3 + 1] = (uint8_t)(g * 17);
                out[i * 3 + 2] = (uint8_t)(b * 17);
                i++;
            }
        }
    }
}

// The two configs describe the same processing to the two implementations. Written as one
// function so a field can never be set on one side and forgotten on the other.
static void make_pair(const epd_palette_entry_t *palette, epd_tone_mode_t tone_mode,
                      double exposure, double saturation, double contrast, double strength,
                      double shadow_boost, double highlight_compress, double midpoint,
                      epd_range_mode_t range_mode, double range_strength,
                      epd_adjust_t *fast, epd_epdopt_t *slow)
{
    const epd_adjust_t f = {
        .palette = palette,
        .tone_enabled = true,
        .tone_mode = tone_mode,
        .exposure = (float)exposure,
        .saturation = (float)saturation,
        .contrast = (float)contrast,
        .strength = (float)strength,
        .shadow_boost = (float)shadow_boost,
        .highlight_compress = (float)highlight_compress,
        .midpoint = (float)midpoint,
        .range_mode = range_mode,
        .range_strength = (float)range_strength,
        .low_percentile = 0.02f,
        .high_percentile = 0.98f,
    };
    const epd_epdopt_t s = {
        .palette = palette,
        .tone_enabled = true,
        .tone_mode = tone_mode,
        .exposure = exposure,
        .saturation = saturation,
        .contrast = contrast,
        .strength = strength,
        .shadow_boost = shadow_boost,
        .highlight_compress = highlight_compress,
        .midpoint = midpoint,
        .range_mode = range_mode,
        // The fast branch is the one epd_adjust.c reimplements. EPD_RANGE_ACCURATE is a
        // different algorithm, not a quality setting, and comparing against it would be
        // comparing two different functions.
        .range_quality = EPD_RANGE_FAST,
        .range_strength = range_strength,
        .low_percentile = 0.02,
        .high_percentile = 0.98,
        .kernel = &EPD_DIFFUSION_FLOYD_STEINBERG,
        .serpentine = false,
    };
    *fast = f;
    *slow = s;
}

// Raw numbers, not a verdict: the maximum tells you whether any pixel moved far, the mean
// whether the whole image is systematically shifted. A rewrite can pass the first and fail
// the second, and that failure is the one that would be visible on the glass.
static void compare(const char *label, size_t pixels, int max_allowed, double mean_allowed)
{
    int max_diff = 0;
    double sum = 0.0;
    size_t differing = 0;

    for (size_t i = 0; i < pixels * 3; i++) {
        const int diff = abs((int)s_float_buf[i] - (int)s_double_buf[i]);
        if (diff > max_diff) {
            max_diff = diff;
        }
        if (diff != 0) {
            differing++;
        }
        sum += diff;
    }

    const double mean = sum / (double)(pixels * 3);
    printf("# adjust-parity %s: max=%d mean=%.4f differing=%zu/%zu\n", label, max_diff, mean,
           differing, pixels * 3);

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(max_allowed, max_diff, label);
    TEST_ASSERT_TRUE_MESSAGE(mean <= mean_allowed, label);
}

// EPD_ADJUST_BALANCED and EPD_EPDOPT_BALANCED_FAST must describe the same processing, or the
// timing arm in photo_main.c measures a different workload from the 12 826 ms figure it is
// being compared against -- and the comparison, not the speed, is the point of that arm.
static void test_balanced_configs_agree(void)
{
    TEST_ASSERT_EQUAL_PTR(EPD_EPDOPT_BALANCED_FAST.palette, EPD_ADJUST_BALANCED.palette);
    TEST_ASSERT_EQUAL_INT(EPD_EPDOPT_BALANCED_FAST.tone_enabled,
                          EPD_ADJUST_BALANCED.tone_enabled);
    TEST_ASSERT_EQUAL_INT(EPD_EPDOPT_BALANCED_FAST.tone_mode, EPD_ADJUST_BALANCED.tone_mode);
    TEST_ASSERT_EQUAL_INT(EPD_EPDOPT_BALANCED_FAST.range_mode, EPD_ADJUST_BALANCED.range_mode);
    TEST_ASSERT_EQUAL_FLOAT((float)EPD_EPDOPT_BALANCED_FAST.exposure,
                            EPD_ADJUST_BALANCED.exposure);
    TEST_ASSERT_EQUAL_FLOAT((float)EPD_EPDOPT_BALANCED_FAST.saturation,
                            EPD_ADJUST_BALANCED.saturation);
    TEST_ASSERT_EQUAL_FLOAT((float)EPD_EPDOPT_BALANCED_FAST.contrast,
                            EPD_ADJUST_BALANCED.contrast);
    TEST_ASSERT_EQUAL_FLOAT((float)EPD_EPDOPT_BALANCED_FAST.strength,
                            EPD_ADJUST_BALANCED.strength);
    TEST_ASSERT_EQUAL_FLOAT((float)EPD_EPDOPT_BALANCED_FAST.range_strength,
                            EPD_ADJUST_BALANCED.range_strength);
}

// The preset the timing arm runs. Its tone stage is neutral, so both LUTs are the identity and
// the tone pass must be **exactly** equal -- a difference here is a bug in the LUT build, not
// float noise, which is why this case has a bound of zero.
static void test_balanced_tone_is_exact(void)
{
    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);

    epd_adjust_tone(s_float_buf, SWEEP_PIXELS, &EPD_ADJUST_BALANCED);
    epd_epdopt_tone_map(s_double_buf, SWEEP_PIXELS, &EPD_EPDOPT_BALANCED_FAST);

    compare("balanced/tone", SWEEP_PIXELS, 0, 0.0);
}

static void test_balanced_range_display(void)
{
    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);

    epd_adjust_range(s_float_buf, SWEEP_PIXELS, &EPD_ADJUST_BALANCED);
    TEST_ASSERT_TRUE(
        epd_epdopt_range_compress(s_double_buf, SWEEP_PIXELS, &EPD_EPDOPT_BALANCED_FAST));

    compare("balanced/range-display", SWEEP_PIXELS, 1, 0.02);
}

// AUTO mode reads the image's own percentiles. Both implementations bin luma to 256 integer
// bins, so the endpoints they find should be identical and only the per-pixel arithmetic can
// differ.
static void test_range_auto(void)
{
    epd_adjust_t fast;
    epd_epdopt_t slow;
    make_pair(EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_MODE_CONTRAST, 0.0, 0.0, 0.0, 0.0, 0.0,
              -1.5, 0.5, EPD_RANGE_MODE_AUTO, 0.9, &fast, &slow);

    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);

    epd_adjust_range(s_float_buf, SWEEP_PIXELS, &fast);
    TEST_ASSERT_TRUE(epd_epdopt_range_compress(s_double_buf, SWEEP_PIXELS, &slow));

    compare("range-auto", SWEEP_PIXELS, 1, 0.02);
}

// The arms the auto flow will actually select for photographs: an S-curve with a saturation
// boost, which is the only tone path with per-pixel arithmetic in it (the HSL round trip), and
// the only one where powf() in the LUT build can shift an index.
static void test_scurve_with_saturation(void)
{
    epd_adjust_t fast;
    epd_epdopt_t slow;
    make_pair(EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_MODE_SCURVE, 0.084, 0.45, 0.0, 0.68, 0.06,
              -1.2, 0.5, EPD_RANGE_MODE_OFF, 1.0, &fast, &slow);

    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);

    epd_adjust_tone(s_float_buf, SWEEP_PIXELS, &fast);
    epd_epdopt_tone_map(s_double_buf, SWEEP_PIXELS, &slow);

    compare("scurve+saturation/tone", SWEEP_PIXELS, 3, 0.05);
}

static void run_cumulative(epd_range_mode_t range_mode)
{
    epd_adjust_t fast;
    epd_epdopt_t slow;
    make_pair(EPD_PALETTE_EPDOPT_ORIGINAL, EPD_TONE_MODE_SCURVE, 0.06, -0.1, 0.0, 1.0, 0.28,
              -0.75, 0.44, range_mode, 0.96, &fast, &slow);

    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);

    epd_adjust_tone(s_float_buf, SWEEP_PIXELS, &fast);
    epd_adjust_range(s_float_buf, SWEEP_PIXELS, &fast);
    epd_epdopt_tone_map(s_double_buf, SWEEP_PIXELS, &slow);
    TEST_ASSERT_TRUE(epd_epdopt_range_compress(s_double_buf, SWEEP_PIXELS, &slow));
}

// Both stages in the order production runs them, against a second palette so the endpoint
// search is exercised on a different table, and with a fixed source range.
static void test_cumulative_display_range(void)
{
    run_cumulative(EPD_RANGE_MODE_DISPLAY);
    compare("cumulative/original/display", SWEEP_PIXELS, 2, 0.02);
}

// The same pipeline with AUTO, and it is **much** further apart than any other case here:
// ~29 % of bytes off by one against 0.35 % for the tone stage alone. That is not the float
// arithmetic getting worse -- it is AUTO's endpoints being a *discrete* function of the input.
// The tone stage leaves a few dozen bytes different, those bytes can move a luma histogram
// bin's count across a percentile boundary, and then the endpoint the whole image is rescaled
// against differs by a whole level. One differing pixel shifts everything.
//
// So the bound that means anything for this case is the maximum, not the mean, and the test
// that means anything at all is the index comparison below: a one-level shift before a
// six-colour quantiser is not necessarily a different colour, and whether it is is the only
// form of this question the glass can answer.
static void test_cumulative_auto_range(void)
{
    run_cumulative(EPD_RANGE_MODE_AUTO);
    compare("cumulative/original/auto", SWEEP_PIXELS, 2, 0.5);
}

// **The acceptance test that matters.** Everything above compares intermediate bytes; the panel
// only ever sees palette indices, and two RGB values one level apart quantise to the same index
// almost everywhere. This runs the full cumulative pipeline both ways, packs both with the
// production quantiser, and counts pixels whose index differs.
//
// The bound is 0.5 % of pixels, written down before the first run. It is deliberately not zero:
// a float rewrite that never moved a single index would be surprising, and demanding it would
// be demanding parity by another name. What it does rule out is the failure that would show --
// a systematic shift large enough to move flat areas to a different colour.
//
// Measured 17/4096 = 0.415 % on first run, which is close to the bound and should not be read
// as a photograph's figure: a uniform RGB cube is the worst case for AUTO, because every
// histogram bin holds the same count and a percentile boundary therefore sits on a knife edge.
// A photograph's histogram is lumpy and its endpoints are correspondingly stable. If this
// number moves, find out why -- do not raise the bound.
static void test_indices_agree_after_quantisation(void)
{
    static uint8_t float_packed[SWEEP_PIXELS / 2];
    static uint8_t double_packed[SWEEP_PIXELS / 2];

    run_cumulative(EPD_RANGE_MODE_AUTO);

    // Quantised exactly as the device would, with tone compression off -- the range stage has
    // already done that job, and doing it twice is the hazard the shot table warns about.
    const epd_render_t cfg = {EPD_PALETTE_EPDOPT_ORIGINAL, EPD_TONE_NONE};
    epd_dither_row_none_cfg(s_float_buf, float_packed, SWEEP_PIXELS, &cfg);
    epd_dither_row_none_cfg(s_double_buf, double_packed, SWEEP_PIXELS, &cfg);

    size_t differing = 0;
    for (size_t i = 0; i < SWEEP_PIXELS / 2; i++) {
        if ((float_packed[i] & 0xF0) != (double_packed[i] & 0xF0)) {
            differing++;
        }
        if ((float_packed[i] & 0x0F) != (double_packed[i] & 0x0F)) {
            differing++;
        }
    }

    const double share = 100.0 * (double)differing / (double)SWEEP_PIXELS;
    printf("# adjust-parity indices: differing=%zu/%d (%.3f %%)\n", differing, SWEEP_PIXELS,
           share);
    TEST_ASSERT_TRUE_MESSAGE(share <= 0.5, "index share");
}

// Every pixel of a diffused canvas is already a palette colour, and the range stage must not
// be able to move one off its entry by a whole step -- that is what would turn a flat area
// into a different colour rather than into slightly different bytes.
static void test_neutral_settings_change_nothing(void)
{
    epd_adjust_t fast;
    epd_epdopt_t slow;
    make_pair(EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_MODE_OFF, 0.0, 0.0, 0.0, 0.0, 0.0, -1.5,
              0.5, EPD_RANGE_MODE_OFF, 0.0, &fast, &slow);

    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);

    epd_adjust_tone(s_float_buf, SWEEP_PIXELS, &fast);
    epd_adjust_range(s_float_buf, SWEEP_PIXELS, &fast);

    compare("neutral", SWEEP_PIXELS, 0, 0.0);
}

static void test_rejects_null(void)
{
    epd_adjust_tone(NULL, 16, &EPD_ADJUST_BALANCED);
    epd_adjust_range(NULL, 16, &EPD_ADJUST_BALANCED);

    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);
    epd_adjust_tone(s_float_buf, SWEEP_PIXELS, NULL);
    epd_adjust_range(s_float_buf, SWEEP_PIXELS, NULL);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_double_buf, s_float_buf, SWEEP_PIXELS * 3);

    epd_adjust_t no_palette = EPD_ADJUST_BALANCED;
    no_palette.palette = NULL;
    epd_adjust_range(s_float_buf, SWEEP_PIXELS, &no_palette);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_double_buf, s_float_buf, SWEEP_PIXELS * 3);
}

// ================================================================ the three added stages
//
// Paper normalisation, luma level compression and white preservation have no double counterpart
// in epd_epdopt.c, so there is nothing in this repository to chain them to. They are compared
// against the library's own bytes instead, from test/test_adjust/stage_fixtures.h.
//
// **The bounds were written down before the first run, same rule as above.** Each of these is a
// float rewrite of the same arithmetic with at most one algebraic rearrangement, so a maximum of
// 2 LSB and a mean of 0.05 is what a correct port should produce -- the two stages above measured
// 1-3 and 0.02-0.05 on comparable work. If a bound is exceeded the rewrite is wrong; report the
// number, do not raise the bound.
//
// **All three measured max=0 on the first run: byte-exact, not merely inside the bound.** That is
// worth recording and is not a reason to tighten the bound to zero. None of these stages has a
// transcendental in it -- no `pow`, no `cbrt` -- so single precision has enough headroom that the
// results round to the same bytes, which the tone stage's S-curve does not. The fixture images are
// 24 x 8 and do not sweep the whole cube, so zero here is a fact about these inputs rather than a
// guarantee, and a bound of zero would turn an ordinary rounding difference on some other picture
// into a failure that looks like a bug.
#define STAGE_MAX_BYTES (24 * 8 * 3)
#define STAGE_MAX_DIFF 2
#define STAGE_MEAN_DIFF 0.05

// The values src/epd_auto.c produces for the arms that set these, and the same ones
// tools/epdopt_reference.mjs generated the fixtures with.
static const epd_paper_t STAGE_PAPER_CFG = {
    true, 0.95f, 82.0f, 0.56f, 8.0f, 0.95f, 0.85f, {248, 248, 246},
};
static const epd_level_t STAGE_LEVEL_CFG = {true, 8.0f, 245.0f};
static const epd_white_t STAGE_WHITE_CFG = {true, 0.99f, 150.0f, 0.18f};

// Display-mode range compression at full strength against the palette the frame ships. The tone
// stage is off, because the fixture was generated with no toneMapping at all.
static epd_adjust_t stage_range_cfg(void)
{
    epd_adjust_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.palette = EPD_PALETTE_EPDOPT_AITJCIZE;
    cfg.tone_enabled = false;
    cfg.range_mode = EPD_RANGE_MODE_DISPLAY;
    cfg.range_strength = 1.0f;
    cfg.low_percentile = 0.01f;
    cfg.high_percentile = 0.99f;
    return cfg;
}

static uint8_t s_stage[STAGE_MAX_BYTES];

// Same shape as compare(), against a fixture array rather than the double buffer.
static void compare_bytes(const char *label, const uint8_t *got, const uint8_t *want,
                          size_t bytes, int max_allowed, double mean_allowed)
{
    int max_diff = 0;
    double sum = 0.0;
    size_t differing = 0;

    for (size_t i = 0; i < bytes; i++) {
        const int diff = abs((int)got[i] - (int)want[i]);
        if (diff > max_diff) {
            max_diff = diff;
        }
        if (diff != 0) {
            differing++;
        }
        sum += diff;
    }

    const double mean = sum / (double)bytes;
    printf("# stage-parity %s: max=%d mean=%.4f differing=%zu/%zu\n", label, max_diff, mean,
           differing, bytes);

    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(max_allowed, max_diff, label);
    TEST_ASSERT_TRUE_MESSAGE(mean <= mean_allowed, label);
}

static size_t stage_bytes(const stage_fixture_t *f)
{
    return (size_t)f->width * (size_t)f->height * 3u;
}

static void test_paper_normalisation_matches_the_library(void)
{
    for (size_t i = 0; i < STAGE_FIXTURE_COUNT; i++) {
        const stage_fixture_t *f = &STAGE_FIXTURES[i];
        memcpy(s_stage, f->input, stage_bytes(f));
        epd_adjust_paper_region(s_stage, (size_t)f->width, (size_t)f->height,
                                (size_t)f->width * 3u, &STAGE_PAPER_CFG);
        compare_bytes(f->name, s_stage, f->after_paper, stage_bytes(f), STAGE_MAX_DIFF,
                      STAGE_MEAN_DIFF);
    }
}

static void test_level_compression_matches_the_library(void)
{
    for (size_t i = 0; i < STAGE_FIXTURE_COUNT; i++) {
        const stage_fixture_t *f = &STAGE_FIXTURES[i];
        memcpy(s_stage, f->input, stage_bytes(f));
        epd_adjust_level_region(s_stage, (size_t)f->width, (size_t)f->height,
                                (size_t)f->width * 3u, &STAGE_LEVEL_CFG);
        compare_bytes(f->name, s_stage, f->after_level, stage_bytes(f), STAGE_MAX_DIFF,
                      STAGE_MEAN_DIFF);
    }
}

// Two assertions per fixture, and the second is the one white preservation is actually in: range
// alone must reproduce `after_range`, and range followed by the two white calls must reproduce
// `after_range_white`. The first is the control -- if it fails, the disagreement is in range
// compression and says nothing about the stage under test.
static void test_white_preservation_matches_the_library(void)
{
    const epd_adjust_t cfg = stage_range_cfg();
    int active_seen = 0;
    int inactive_seen = 0;

    for (size_t i = 0; i < STAGE_FIXTURE_COUNT; i++) {
        const stage_fixture_t *f = &STAGE_FIXTURES[i];
        const size_t width = (size_t)f->width;
        const size_t height = (size_t)f->height;
        const size_t stride = width * 3u;
        char label[80];

        memcpy(s_stage, f->input, stage_bytes(f));
        epd_adjust_range_region(s_stage, width, height, stride, &cfg);
        snprintf(label, sizeof(label), "%s/range-control", f->name);
        compare_bytes(label, s_stage, f->after_range, stage_bytes(f), STAGE_MAX_DIFF,
                      STAGE_MEAN_DIFF);

        // The plan is measured on the untouched source, which is what makes it two calls.
        uint8_t bits[(24 * 8 + 7) / 8];
        epd_white_plan_t plan;
        memcpy(s_stage, f->input, stage_bytes(f));
        const bool active = epd_adjust_white_plan(s_stage, width, height, stride,
                                                 EPD_PALETTE_EPDOPT_AITJCIZE, &STAGE_WHITE_CFG,
                                                 bits, sizeof(bits), &plan);
        printf("# white-plan %s: active=%d source_white_luma=%.2f target=%d,%d,%d\n", f->name,
               active ? 1 : 0, (double)plan.source_white_luma, (int)plan.target[0],
               (int)plan.target[1], (int)plan.target[2]);

        epd_adjust_range_region(s_stage, width, height, stride, &cfg);
        epd_adjust_white_apply(s_stage, width, height, stride, bits, &plan);
        snprintf(label, sizeof(label), "%s/range+white", f->name);
        compare_bytes(label, s_stage, f->after_range_white, stage_bytes(f), STAGE_MAX_DIFF,
                      STAGE_MEAN_DIFF);

        // The relationship, rather than a second implementation of dither.ts:378-385 in the
        // generator. **It only holds in one direction, and asserting the other way round is
        // what the first version of this test did wrong:** `mixed-24x8` produces an active plan
        // (source white luma 240) whose apply pass then changes nothing, because every marked
        // pixel came out of range compression already at or above the palette white's luma and
        // dither.ts:412 leaves those alone. Upstream does the same, so the bytes agree exactly.
        // An active plan therefore does not imply a changed picture; a changed picture does
        // imply an active plan.
        if (f->white_changed) {
            TEST_ASSERT_TRUE_MESSAGE(active, f->name);
        }
        if (active) {
            active_seen++;
            TEST_ASSERT_TRUE_MESSAGE(plan.active, f->name);
            TEST_ASSERT_TRUE_MESSAGE(plan.source_white_luma >= STAGE_WHITE_CFG.min_luma,
                                     f->name);
        } else {
            inactive_seen++;
            TEST_ASSERT_FALSE_MESSAGE(plan.active, f->name);
            // An abandoned plan must leave the picture exactly as range compression did, or the
            // apply pass is running off a stale plan.
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, f->white_changed, f->name);
        }
    }

    // Both outcomes have to occur or this test is one branch wearing the shape of two. The
    // generator refuses to write the header unless they do; this is where that is checked rather
    // than trusted.
    TEST_ASSERT_TRUE(active_seen > 0);
    TEST_ASSERT_TRUE(inactive_seen > 0);
}

// ================================================================ the region forms
//
// The device runs every stage over a rectangle inside the canvas, because the canvas is matted
// white and two of these measure statistics over the pixels they are given. Two things have to
// hold: the region's bytes come out the same as if it had been alone in a tight buffer, and
// nothing outside the region is touched. The second is what a stride bug would break.

#define EMBED_W 40
#define EMBED_H 12
#define EMBED_X 8
#define EMBED_Y 2
#define EMBED_SENTINEL 0x5A

typedef enum {
    REGION_PAPER,
    REGION_TONE,
    REGION_LEVEL,
    REGION_RANGE_DISPLAY,
    REGION_RANGE_AUTO,
    REGION_WHITE,
} region_stage_t;

static const char *region_stage_name(region_stage_t stage)
{
    switch (stage) {
    case REGION_PAPER:
        return "paper";
    case REGION_TONE:
        return "tone";
    case REGION_LEVEL:
        return "level";
    case REGION_RANGE_DISPLAY:
        return "range-display";
    case REGION_RANGE_AUTO:
        return "range-auto";
    default:
        return "white";
    }
}

// A non-neutral saturation for the tone case on purpose: that is the HSL branch, which reads and
// writes through a different code path from the LUT one and is the more likely place for a stride
// error to hide.
static epd_adjust_t stage_tone_cfg(void)
{
    epd_adjust_t cfg = stage_range_cfg();
    cfg.tone_enabled = true;
    cfg.tone_mode = EPD_TONE_MODE_SCURVE;
    cfg.exposure = 0.084f;
    cfg.saturation = 0.45f;
    cfg.strength = 0.68f;
    cfg.shadow_boost = 0.06f;
    cfg.highlight_compress = -1.2f;
    cfg.midpoint = 0.5f;
    cfg.range_mode = EPD_RANGE_MODE_OFF;
    return cfg;
}

static void run_region_stage(region_stage_t stage, uint8_t *rgb, size_t w, size_t h,
                             size_t stride)
{
    // Sized for the largest fixture. The white case needs its own, because the bit index is
    // region-local -- a stride error would either mark the wrong pixels or walk off the end.
    static uint8_t bits[(24 * 8 + 7) / 8];
    epd_adjust_t cfg;
    epd_white_plan_t plan;

    switch (stage) {
    case REGION_PAPER:
        epd_adjust_paper_region(rgb, w, h, stride, &STAGE_PAPER_CFG);
        return;
    case REGION_TONE:
        cfg = stage_tone_cfg();
        epd_adjust_tone_region(rgb, w, h, stride, &cfg);
        return;
    case REGION_LEVEL:
        epd_adjust_level_region(rgb, w, h, stride, &STAGE_LEVEL_CFG);
        return;
    case REGION_RANGE_DISPLAY:
        cfg = stage_range_cfg();
        epd_adjust_range_region(rgb, w, h, stride, &cfg);
        return;
    case REGION_RANGE_AUTO:
        cfg = stage_range_cfg();
        cfg.range_mode = EPD_RANGE_MODE_AUTO;
        epd_adjust_range_region(rgb, w, h, stride, &cfg);
        return;
    default:
        cfg = stage_range_cfg();
        epd_adjust_white_plan(rgb, w, h, stride, EPD_PALETTE_EPDOPT_AITJCIZE, &STAGE_WHITE_CFG,
                              bits, sizeof(bits), &plan);
        epd_adjust_range_region(rgb, w, h, stride, &cfg);
        epd_adjust_white_apply(rgb, w, h, stride, bits, &plan);
        return;
    }
}

static void test_a_region_is_untouched_by_its_surroundings(void)
{
    static uint8_t embedded[EMBED_W * EMBED_H * 3];

    for (int stage = REGION_PAPER; stage <= REGION_WHITE; stage++) {
        for (size_t i = 0; i < STAGE_FIXTURE_COUNT; i++) {
            const stage_fixture_t *f = &STAGE_FIXTURES[i];
            const size_t w = (size_t)f->width;
            const size_t h = (size_t)f->height;
            TEST_ASSERT_TRUE(w + EMBED_X <= EMBED_W && h + EMBED_Y <= EMBED_H);

            // Tight: the region alone, in a buffer whose stride is its own width.
            memcpy(s_stage, f->input, stage_bytes(f));
            run_region_stage((region_stage_t)stage, s_stage, w, h, w * 3u);

            // Embedded: the same pixels inside a larger buffer, surrounded by a sentinel that a
            // stride error would either read or write.
            memset(embedded, EMBED_SENTINEL, sizeof(embedded));
            for (size_t row = 0; row < h; row++) {
                memcpy(&embedded[((EMBED_Y + row) * EMBED_W + EMBED_X) * 3],
                       &f->input[row * w * 3], w * 3);
            }
            run_region_stage((region_stage_t)stage,
                             &embedded[(EMBED_Y * EMBED_W + EMBED_X) * 3], w, h, EMBED_W * 3u);

            char label[96];
            snprintf(label, sizeof(label), "%s/%s", region_stage_name((region_stage_t)stage),
                     f->name);
            for (size_t row = 0; row < h; row++) {
                TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(
                    &s_stage[row * w * 3],
                    &embedded[((EMBED_Y + row) * EMBED_W + EMBED_X) * 3], w * 3, label);
            }

            for (size_t py = 0; py < EMBED_H; py++) {
                for (size_t px = 0; px < EMBED_W; px++) {
                    const bool inside = px >= EMBED_X && px < EMBED_X + w && py >= EMBED_Y &&
                                        py < EMBED_Y + h;
                    if (inside) {
                        continue;
                    }
                    const uint8_t *p = &embedded[(py * EMBED_W + px) * 3];
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(EMBED_SENTINEL, p[0], label);
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(EMBED_SENTINEL, p[1], label);
                    TEST_ASSERT_EQUAL_UINT8_MESSAGE(EMBED_SENTINEL, p[2], label);
                }
            }
        }
    }
}

// The flat entry points are the region form at height 1, and for AUTO mode that is a claim rather
// than a definition: the percentiles are measured over the pixel *set*, and 24 x 8 and 1 x 192
// are the same set. If that ever stops holding, every existing case in this file that calls
// epd_adjust_range() is measuring something other than what the device runs.
static void test_the_flat_form_is_the_region_form(void)
{
    static uint8_t flat[STAGE_MAX_BYTES];

    for (size_t i = 0; i < STAGE_FIXTURE_COUNT; i++) {
        const stage_fixture_t *f = &STAGE_FIXTURES[i];
        const size_t pixels = (size_t)f->width * (size_t)f->height;
        epd_adjust_t cfg = stage_range_cfg();
        cfg.range_mode = EPD_RANGE_MODE_AUTO;

        memcpy(flat, f->input, stage_bytes(f));
        epd_adjust_range(flat, pixels, &cfg);

        memcpy(s_stage, f->input, stage_bytes(f));
        epd_adjust_range_region(s_stage, (size_t)f->width, (size_t)f->height,
                                (size_t)f->width * 3u, &cfg);

        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(flat, s_stage, stage_bytes(f), f->name);
    }
}

// The rotation mapping lives in epd_canvas.c and nowhere else, so this checks the rectangle form
// against offset_of()'s own answer for the corners rather than against a second copy of the
// arithmetic: whatever epd_canvas_pixel() says the logical corners are, the rectangle has to
// contain exactly those physical rows and columns.
static void test_the_physical_rect_follows_the_rotation(void)
{
    static uint8_t buffer[400 * 60 * 3];
    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, buffer, sizeof(buffer), 400, 60));

    // All four quarter turns since ticket 69, and the loop bound is the canvas's own constant so
    // that a fifth direction could not leave this arm testing two of five.
    for (uint8_t rotation = 0; rotation <= EPD_CANVAS_ROTATION_MAX; rotation++) {
        epd_canvas_set_rotation(&canvas, rotation);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(rotation, canvas.rotation, "rotation was refused");
        const int32_t lw = epd_canvas_logical_width(&canvas);
        const int32_t lh = epd_canvas_logical_height(&canvas);
        const int32_t x = 7;
        const int32_t y = 3;
        const int32_t w = lw - 11;
        const int32_t h = lh - 5;

        int32_t px = 0, py = 0, pw = 0, ph = 0;
        TEST_ASSERT_TRUE(epd_canvas_physical_rect(&canvas, x, y, w, h, &px, &py, &pw, &ph));
        TEST_ASSERT_EQUAL_INT32(w * h, pw * ph);

        // Every logical corner has to land inside the physical rectangle, and the rectangle has
        // to be no bigger than the four corners require -- which together pin it exactly.
        const int32_t corners[4][2] = {
            {x, y}, {x + w - 1, y}, {x, y + h - 1}, {x + w - 1, y + h - 1},
        };
        int32_t min_x = canvas.width, max_x = -1, min_y = canvas.height, max_y = -1;
        for (int i = 0; i < 4; i++) {
            uint8_t *pixel = epd_canvas_pixel(&canvas, corners[i][0], corners[i][1]);
            TEST_ASSERT_NOT_NULL(pixel);
            const int32_t offset = (int32_t)(pixel - canvas.rgb) / 3;
            const int32_t cx = offset % canvas.width;
            const int32_t cy = offset / canvas.width;
            if (cx < min_x) min_x = cx;
            if (cx > max_x) max_x = cx;
            if (cy < min_y) min_y = cy;
            if (cy > max_y) max_y = cy;
        }
        TEST_ASSERT_EQUAL_INT32(min_x, px);
        TEST_ASSERT_EQUAL_INT32(min_y, py);
        TEST_ASSERT_EQUAL_INT32(max_x - min_x + 1, pw);
        TEST_ASSERT_EQUAL_INT32(max_y - min_y + 1, ph);
    }

    // Refusals rather than clipping: a rectangle that does not fit was computed against
    // different dimensions than the canvas has, and shrinking it would hide that.
    int32_t px, py, pw, ph;
    epd_canvas_set_rotation(&canvas, 0);
    TEST_ASSERT_FALSE(epd_canvas_physical_rect(&canvas, 0, 0, 401, 60, &px, &py, &pw, &ph));
    TEST_ASSERT_FALSE(epd_canvas_physical_rect(&canvas, -1, 0, 10, 10, &px, &py, &pw, &ph));
    TEST_ASSERT_FALSE(epd_canvas_physical_rect(&canvas, 0, 0, 0, 10, &px, &py, &pw, &ph));
    TEST_ASSERT_FALSE(epd_canvas_physical_rect(NULL, 0, 0, 10, 10, &px, &py, &pw, &ph));
}

static void test_the_added_stages_reject_null(void)
{
    uint8_t bits[8];
    epd_white_plan_t plan;

    epd_adjust_paper_region(NULL, 4, 4, 12, &STAGE_PAPER_CFG);
    epd_adjust_level_region(NULL, 4, 4, 12, &STAGE_LEVEL_CFG);
    epd_adjust_paper_region(s_stage, 4, 4, 12, NULL);
    epd_adjust_level_region(s_stage, 4, 4, 12, NULL);

    // A stage whose `enabled` is false is a no-op, so the caller does not have to branch.
    fill_sweep(s_float_buf);
    fill_sweep(s_double_buf);
    const epd_paper_t paper_off = {false, 0.95f, 82.0f, 0.56f, 8.0f, 0.95f, 0.85f, {248, 248, 246}};
    const epd_level_t level_off = {false, 8.0f, 245.0f};
    epd_adjust_paper_region(s_float_buf, SWEEP_PIXELS, 1, SWEEP_PIXELS * 3, &paper_off);
    epd_adjust_level_region(s_float_buf, SWEEP_PIXELS, 1, SWEEP_PIXELS * 3, &level_off);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_double_buf, s_float_buf, SWEEP_PIXELS * 3);

    // A short bits buffer is a refusal, not a wild write. 4 x 4 needs two bytes.
    TEST_ASSERT_FALSE(epd_adjust_white_plan(s_stage, 4, 4, 12, EPD_PALETTE_EPDOPT_AITJCIZE,
                                            &STAGE_WHITE_CFG, bits, 1, &plan));
    TEST_ASSERT_FALSE(plan.active);
    TEST_ASSERT_FALSE(epd_adjust_white_plan(s_stage, 4, 4, 12, NULL, &STAGE_WHITE_CFG, bits,
                                            sizeof(bits), &plan));
    TEST_ASSERT_FALSE(epd_adjust_white_plan(NULL, 4, 4, 12, EPD_PALETTE_EPDOPT_AITJCIZE,
                                            &STAGE_WHITE_CFG, bits, sizeof(bits), &plan));
    TEST_ASSERT_EQUAL_size_t(2, epd_adjust_white_bits_bytes(4, 4));
    TEST_ASSERT_EQUAL_size_t(30000, epd_adjust_white_bits_bytes(400, 600));

    // An inactive plan applies nothing.
    fill_sweep(s_float_buf);
    memset(&plan, 0, sizeof(plan));
    epd_adjust_white_apply(s_float_buf, SWEEP_PIXELS, 1, SWEEP_PIXELS * 3, bits, &plan);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_double_buf, s_float_buf, SWEEP_PIXELS * 3);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_balanced_configs_agree);
    RUN_TEST(test_balanced_tone_is_exact);
    RUN_TEST(test_balanced_range_display);
    RUN_TEST(test_range_auto);
    RUN_TEST(test_scurve_with_saturation);
    RUN_TEST(test_cumulative_display_range);
    RUN_TEST(test_cumulative_auto_range);
    RUN_TEST(test_indices_agree_after_quantisation);
    RUN_TEST(test_neutral_settings_change_nothing);
    RUN_TEST(test_rejects_null);

    RUN_TEST(test_paper_normalisation_matches_the_library);
    RUN_TEST(test_level_compression_matches_the_library);
    RUN_TEST(test_white_preservation_matches_the_library);
    RUN_TEST(test_a_region_is_untouched_by_its_surroundings);
    RUN_TEST(test_the_flat_form_is_the_region_form);
    RUN_TEST(test_the_physical_rect_follows_the_rotation);
    RUN_TEST(test_the_added_stages_reject_null);

    return UNITY_END();
}
