// Is src/core/epd_classify.c the same classifier as epdoptimize's image-style.ts?
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
// Regenerate the fixtures: node tools/epdopt_reference.mjs --classify
//
// **What is pinned here is agreement, not judgement.** Every expected number came out of the
// library itself, so a failure means the port diverged -- it does not mean the classification is
// wrong for a photograph, and this file cannot tell you that. Whether the classifier calls a
// photograph a photograph is a question for real images through tools/render_preview.py, where a
// person can look at what it decided.
//
// The images are not in the fixture header. Each is an integer-only rule reproduced below, and
// each fixture carries an FNV-1a checksum of the bytes -- so a divergence between the JavaScript
// definition and this one fails as `checksum`, loudly, instead of as a metric that would read as
// a bug in the port.
//
// **The tolerances are per field and each one has a reason.** They were written down before the
// first run. A value in the fixture header must never be edited to make a test pass, and a
// tolerance must not be widened to do it either: both are ways of deleting the measurement.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unity.h>

#include "epd_classify.h"

#include "classify_fixtures.h"

void setUp(void) {}
void tearDown(void) {}

#define MAX_IMAGE_BYTES (200 * 300 * 3)

static uint8_t s_image[MAX_IMAGE_BYTES];

static epd_classify_sample_t s_samples[EPD_CLASSIFY_MAX_SAMPLE_DIMENSION *
                                      EPD_CLASSIFY_MAX_SAMPLE_DIMENSION];
static uint16_t s_colour_counts[EPD_CLASSIFY_COLOUR_KEYS];
static uint16_t s_tile_stamps[EPD_CLASSIFY_COLOUR_KEYS];
static uint32_t s_luma_bins[1024];

static epd_classify_scratch_t scratch(void)
{
    const epd_classify_scratch_t s = {
        .samples = s_samples,
        .sample_capacity = sizeof(s_samples) / sizeof(s_samples[0]),
        .colour_counts = s_colour_counts,
        .tile_stamps = s_tile_stamps,
        .luma_bins = s_luma_bins,
    };
    return s;
}

// ---------------------------------------------------------------------- the same images
//
// Each of these mirrors one entry of CLASSIFY_IMAGES in tools/epdopt_reference.mjs. Integer
// arithmetic only, so the two languages cannot disagree; the checksum is what proves it.

static void fill_noise(uint8_t *rgb, size_t bytes)
{
    // xorshift32. Shifts and xors on a uint32 are exactly the same sequence in JavaScript,
    // which is why the generator uses this rather than the LCG its parity fixtures use.
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < bytes; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        rgb[i] = (uint8_t)(state & 0xffu);
    }
}

static void fill_gradient(uint8_t *rgb, int32_t width, int32_t height)
{
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            uint8_t *p = &rgb[((size_t)y * (size_t)width + (size_t)x) * 3];
            p[0] = (uint8_t)((x * 255) / (width - 1));
            p[1] = (uint8_t)((y * 255) / (height - 1));
            p[2] = (uint8_t)(((x + y) * 255) / (width + height - 2));
        }
    }
}

static void fill_flat_blocks(uint8_t *rgb, int32_t width, int32_t height)
{
    static const uint8_t bands[6][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255},
    };
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            int32_t band = (x * 6) / width;
            if (band > 5) {
                band = 5;
            }
            uint8_t *p = &rgb[((size_t)y * (size_t)width + (size_t)x) * 3];
            memcpy(p, bands[band], 3);
        }
    }
}

static void fill_text_like(uint8_t *rgb, int32_t width, int32_t height)
{
    memset(rgb, 255, (size_t)width * (size_t)height * 3);
    for (int32_t y = 0; y < height; y++) {
        if (y % 20 >= 10) {
            continue;
        }
        for (int32_t x = 0; x < width; x++) {
            if (x % 6 >= 2) {
                continue;
            }
            uint8_t *p = &rgb[((size_t)y * (size_t)width + (size_t)x) * 3];
            p[0] = 16;
            p[1] = 16;
            p[2] = 16;
        }
    }
}

static void fill_line_art(uint8_t *rgb, int32_t width, int32_t height)
{
    memset(rgb, 255, (size_t)width * (size_t)height * 3);
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            if ((x + y) % 17 != 0) {
                continue;
            }
            uint8_t *p = &rgb[((size_t)y * (size_t)width + (size_t)x) * 3];
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
        }
    }
}

static void fill_tiny(uint8_t *rgb, int32_t width, int32_t height)
{
    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            uint8_t *p = &rgb[((size_t)y * (size_t)width + (size_t)x) * 3];
            p[0] = (uint8_t)(x * 50);
            p[1] = (uint8_t)(y * 80);
            p[2] = (uint8_t)(255 - x * 40);
        }
    }
}

static bool build_image(const char *name, int32_t width, int32_t height)
{
    const size_t bytes = (size_t)width * (size_t)height * 3;
    TEST_ASSERT_TRUE_MESSAGE(bytes <= MAX_IMAGE_BYTES, name);

    if (strcmp(name, "noise-200x300") == 0) {
        fill_noise(s_image, bytes);
    } else if (strcmp(name, "gradient-200x300") == 0) {
        fill_gradient(s_image, width, height);
    } else if (strcmp(name, "flat-blocks-160x120") == 0) {
        fill_flat_blocks(s_image, width, height);
    } else if (strcmp(name, "text-like-160x120") == 0) {
        fill_text_like(s_image, width, height);
    } else if (strcmp(name, "line-art-160x120") == 0) {
        fill_line_art(s_image, width, height);
    } else if (strcmp(name, "tiny-5x3") == 0) {
        fill_tiny(s_image, width, height);
    } else {
        return false;
    }
    return true;
}

static uint32_t fnv1a(const uint8_t *data, size_t bytes)
{
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < bytes; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

// ------------------------------------------------------------------------- the tolerances

// In the order of the fixture's `metrics[]`, which is the declaration order of
// epd_classify_metrics_t from unique_colour_ratio onwards.
//
//  * **5e-4 for a counted ratio.** These are integer counts over ~17,000 samples, so they
//    would be exact but for one thing: this port computes luma in `float` and the library in
//    `double`, so a sample whose luma sits within ~1e-5 of a threshold (36, 88, 92, 220, 230)
//    can fall on the other side of it. One such sample moves a ratio by 1/17,120 = 5.8e-5, and
//    5e-4 allows a handful.
//  * **2e-3 for the two edge-direction ratios**, whose denominator is the edge count rather
//    than the sample count and can be a few hundred.
//  * **1e-2 for the four tile ratios.** A tile's own lumaStdDev is compared against 38, 18 and
//    12; a float/double difference there reclassifies a whole tile, and there are only ~171 of
//    them, so one flip is 0.006. This tolerance allows exactly one.
//  * **0.26 / 0.26 / 0.52 for the two percentiles and their range.** The port reads them off a
//    quarter-luma histogram instead of sorting every sample, which is documented in
//    epd_classify.h as deviation 2 and is worth at most 0.25 per endpoint.
//  * **1e-2 for the two standard deviations and the entropy**, which are the only metrics
//    accumulated rather than counted.
static const float METRIC_TOLERANCE[CLASSIFY_METRIC_COUNT] = {
    5e-4f,  // unique_colour_ratio
    5e-4f,  // top_colour_coverage
    1e-2f,  // palette_entropy
    5e-4f,  // flat_ratio
    5e-4f,  // soft_change_ratio
    5e-4f,  // strong_edge_ratio
    5e-4f,  // edge_density
    2e-3f,  // horizontal_edge_ratio
    2e-3f,  // vertical_edge_ratio
    1e-2f,  // luma_std_dev
    0.26f,  // luma_p05
    0.26f,  // luma_p95
    0.52f,  // luma_range
    1e-3f,  // saturation_mean
    1e-3f,  // saturation_std_dev
    5e-4f,  // dark_ratio
    5e-4f,  // light_ratio
    5e-4f,  // gray_ratio
    5e-4f,  // high_saturation_ratio
    5e-4f,  // warm_paper_ratio
    5e-4f,  // red_ratio
    5e-4f,  // dark_neutral_ratio
    1e-2f,  // photo_tile_ratio
    1e-2f,  // flat_tile_ratio
    1e-2f,  // text_tile_ratio
    1e-2f,  // gradient_tile_ratio
};

static const char *const METRIC_NAME[CLASSIFY_METRIC_COUNT] = {
    "unique_colour_ratio", "top_colour_coverage", "palette_entropy",  "flat_ratio",
    "soft_change_ratio",   "strong_edge_ratio",   "edge_density",     "horizontal_edge_ratio",
    "vertical_edge_ratio", "luma_std_dev",        "luma_p05",         "luma_p95",
    "luma_range",          "saturation_mean",     "saturation_std_dev", "dark_ratio",
    "light_ratio",         "gray_ratio",          "high_saturation_ratio", "warm_paper_ratio",
    "red_ratio",           "dark_neutral_ratio",  "photo_tile_ratio", "flat_tile_ratio",
    "text_tile_ratio",     "gradient_tile_ratio",
};

// Written out by name rather than by casting the struct to a float array: the correspondence
// between this order and the generator's METRIC_FIELDS is the thing that has to be right, and a
// pointer cast would hide a reordering instead of breaking on it.
static void metrics_to_array(const epd_classify_metrics_t *m, float *out)
{
    out[0] = m->unique_colour_ratio;
    out[1] = m->top_colour_coverage;
    out[2] = m->palette_entropy;
    out[3] = m->flat_ratio;
    out[4] = m->soft_change_ratio;
    out[5] = m->strong_edge_ratio;
    out[6] = m->edge_density;
    out[7] = m->horizontal_edge_ratio;
    out[8] = m->vertical_edge_ratio;
    out[9] = m->luma_std_dev;
    out[10] = m->luma_p05;
    out[11] = m->luma_p95;
    out[12] = m->luma_range;
    out[13] = m->saturation_mean;
    out[14] = m->saturation_std_dev;
    out[15] = m->dark_ratio;
    out[16] = m->light_ratio;
    out[17] = m->gray_ratio;
    out[18] = m->high_saturation_ratio;
    out[19] = m->warm_paper_ratio;
    out[20] = m->red_ratio;
    out[21] = m->dark_neutral_ratio;
    out[22] = m->photo_tile_ratio;
    out[23] = m->flat_tile_ratio;
    out[24] = m->text_tile_ratio;
    out[25] = m->gradient_tile_ratio;
}

// ------------------------------------------------------------------------------- the tests

static void test_images_are_the_same_images(void)
{
    for (size_t i = 0; i < CLASSIFY_FIXTURE_COUNT; i++) {
        const classify_fixture_t *f = &CLASSIFY_FIXTURES[i];
        TEST_ASSERT_TRUE_MESSAGE(build_image(f->name, f->width, f->height), f->name);

        const uint32_t sum = fnv1a(s_image, (size_t)f->width * (size_t)f->height * 3);
        if (sum != f->checksum) {
            printf("# classify %s: checksum %08x, fixture says %08x\n", f->name, sum,
                   f->checksum);
        }
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(f->checksum, sum, f->name);
    }
}

// The decision, which is the only part of a classification anything downstream reads.
//
// **The kind is allowed to differ only when the argmax was never determined**, and one fixture
// needs that: for `flat-blocks-160x120` upstream scores flatIllustration at
// 0.9999999999999999 and pixelArt at 1, a difference of one double ulp. Both round to exactly
// 1.0f here, and upstream's reduce keeps the *earlier* entry on a tie -- so this port picks
// flatIllustration and the library picks pixelArt, from arithmetic 1.1e-16 apart. No amount of
// care in this file changes that: reproducing it would need every metric bit-exact, which the
// quarter-luma percentile in epd_classify.h rules out by design.
//
// So the assertion is: either the kind matches, or the two kinds are tied to within 1e-6 in
// **this port's own** scores, which is the statement "there was nothing here to get right".
// A real divergence -- two kinds a visible distance apart -- still fails.
static void test_every_fixture_agrees_on_style_and_kind(void)
{
    for (size_t i = 0; i < CLASSIFY_FIXTURE_COUNT; i++) {
        const classify_fixture_t *f = &CLASSIFY_FIXTURES[i];
        TEST_ASSERT_TRUE(build_image(f->name, f->width, f->height));

        const epd_classify_scratch_t s = scratch();
        epd_classification_t got;
        TEST_ASSERT_TRUE_MESSAGE(
            epd_classify_rgb(s_image, f->width, f->height, &s, &got), f->name);

        printf("# classify %s: kind=%s style=%d photo_score=%.6f (fixture kind=%d style=%d "
               "photo_score=%.6f)\n",
               f->name, epd_image_kind_name(got.kind), (int)got.style, (double)got.photo_score,
               f->kind, f->style, (double)f->photo_score);

        if ((int)got.kind != f->kind) {
            const float mine = got.kind_scores[got.kind];
            const float theirs = got.kind_scores[f->kind];
            printf("# classify %s: TIE -- %s scores %.9f, %s scores %.9f, difference %.3e\n",
                   f->name, epd_image_kind_name(got.kind), (double)mine,
                   epd_image_kind_name((epd_image_kind_t)f->kind), (double)theirs,
                   (double)fabsf(mine - theirs));
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6f, theirs, mine, f->name);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(f->style, (int)got.style, f->name);
    }
}

// The scores behind the decision. A kind can be right while the scores are wrong -- one score
// dominating for the wrong reason -- and that would be a port bug waiting for a different image.
static void test_every_fixture_agrees_on_scores(void)
{
    for (size_t i = 0; i < CLASSIFY_FIXTURE_COUNT; i++) {
        const classify_fixture_t *f = &CLASSIFY_FIXTURES[i];
        TEST_ASSERT_TRUE(build_image(f->name, f->width, f->height));

        const epd_classify_scratch_t s = scratch();
        epd_classification_t got;
        TEST_ASSERT_TRUE(epd_classify_rgb(s_image, f->width, f->height, &s, &got));

        // 2e-2: a score is a weighted sum of normalised metrics, and the largest weight on a
        // metric with a 1e-2 tolerance is 0.34, so this is the metric tolerances propagated
        // rather than a number chosen to fit.
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2e-2f, f->photo_score, got.photo_score, f->name);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(4e-2f, f->confidence, got.confidence, f->name);
        for (size_t k = 0; k < EPD_KIND_COUNT; k++) {
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(2e-2f, f->kind_scores[k], got.kind_scores[k],
                                             f->name);
        }
    }
}

static void test_every_fixture_agrees_on_metrics(void)
{
    for (size_t i = 0; i < CLASSIFY_FIXTURE_COUNT; i++) {
        const classify_fixture_t *f = &CLASSIFY_FIXTURES[i];
        TEST_ASSERT_TRUE(build_image(f->name, f->width, f->height));

        const epd_classify_scratch_t s = scratch();
        epd_classification_t got;
        TEST_ASSERT_TRUE(epd_classify_rgb(s_image, f->width, f->height, &s, &got));

        float mine[CLASSIFY_METRIC_COUNT];
        metrics_to_array(&got.metrics, mine);

        for (size_t k = 0; k < CLASSIFY_METRIC_COUNT; k++) {
            const float difference = fabsf(mine[k] - f->metrics[k]);
            if (difference > METRIC_TOLERANCE[k]) {
                printf("# classify %s: %s got %.9f, fixture %.9f, difference %.9f > %.9f\n",
                       f->name, METRIC_NAME[k], (double)mine[k], (double)f->metrics[k],
                       (double)difference, (double)METRIC_TOLERANCE[k]);
            }
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE(METRIC_TOLERANCE[k], f->metrics[k], mine[k],
                                             METRIC_NAME[k]);
        }
    }
}

// The canvas entry point has to agree with the flat one over the same pixels, or every device
// classification is of something the tests never saw. A canvas at rotation 0 whose logical
// extent is the whole image is the case where the two must be identical.
static void test_canvas_and_rgb_agree(void)
{
    const int32_t width = 160;
    const int32_t height = 120;
    TEST_ASSERT_TRUE(build_image("flat-blocks-160x120", width, height));

    static uint8_t canvas_buffer[160 * 120 * 3];
    memcpy(canvas_buffer, s_image, sizeof(canvas_buffer));

    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(
        epd_canvas_init(&canvas, canvas_buffer, sizeof(canvas_buffer), width, height));

    const epd_classify_scratch_t s = scratch();
    epd_classification_t from_canvas;
    epd_classification_t from_rgb;
    TEST_ASSERT_TRUE(epd_classify_canvas(&canvas, 0, 0, width, height, &s, &from_canvas));
    TEST_ASSERT_TRUE(epd_classify_rgb(s_image, width, height, &s, &from_rgb));

    TEST_ASSERT_EQUAL_INT(from_rgb.kind, from_canvas.kind);
    TEST_ASSERT_EQUAL_INT(from_rgb.style, from_canvas.style);
    TEST_ASSERT_EQUAL_FLOAT(from_rgb.photo_score, from_canvas.photo_score);
}

// A region is the point of the canvas entry point: on the device the picture sits inside a white
// matte, and classifying the matte is what would call a photograph a flat illustration. This
// puts the flat-blocks image in a corner of a white canvas and checks that classifying its
// rectangle gives the same answer as classifying it alone -- and that classifying the whole
// canvas does not.
static void test_a_region_excludes_the_matte(void)
{
    const int32_t width = 160;
    const int32_t height = 120;
    TEST_ASSERT_TRUE(build_image("flat-blocks-160x120", width, height));

    // 400 x 200 for a 4:3 image, so the fit leaves a third of the canvas white. A 240 x 200
    // canvas -- the first thing tried here -- leaves only a tenth, and a tenth is small enough
    // that the assertion below became a statement about the exact geometry rather than about
    // the matte. Pick the shape that makes the effect unmistakable.
    static uint8_t canvas_buffer[400 * 200 * 3];
    memset(canvas_buffer, 255, sizeof(canvas_buffer));

    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, canvas_buffer, sizeof(canvas_buffer), 400, 200));
    const epd_fit_t fit = epd_fit_centre(width, height, 400, 200);
    epd_canvas_blit(&canvas, s_image, width, height, fit);

    const epd_classify_scratch_t s = scratch();
    epd_classification_t alone;
    epd_classification_t region;
    epd_classification_t whole;
    TEST_ASSERT_TRUE(epd_classify_rgb(s_image, width, height, &s, &alone));
    TEST_ASSERT_TRUE(
        epd_classify_canvas(&canvas, fit.x, fit.y, fit.width, fit.height, &s, &region));
    TEST_ASSERT_TRUE(epd_classify_canvas(&canvas, 0, 0, 400, 200, &s, &whole));

    printf("# classify matte: alone=%s region=%s whole=%s light_ratio alone=%.4f region=%.4f "
           "whole=%.4f\n",
           epd_image_kind_name(alone.kind), epd_image_kind_name(region.kind),
           epd_image_kind_name(whole.kind), (double)alone.metrics.light_ratio,
           (double)region.metrics.light_ratio, (double)whole.metrics.light_ratio);

    // The fit scales the image, so the region is not pixel-identical to the original and the
    // metrics move a little. The white ratio is the metric that shows the matte, and it is the
    // one that must not have grown when the region is used.
    TEST_ASSERT_FLOAT_WITHIN(0.02f, alone.metrics.light_ratio, region.metrics.light_ratio);
    TEST_ASSERT_TRUE(whole.metrics.light_ratio > region.metrics.light_ratio + 0.15f);
}

static void test_refuses_rather_than_guessing(void)
{
    const epd_classify_scratch_t s = scratch();
    epd_classification_t out;

    TEST_ASSERT_FALSE(epd_classify_rgb(NULL, 16, 16, &s, &out));
    TEST_ASSERT_FALSE(epd_classify_rgb(s_image, 0, 16, &s, &out));
    TEST_ASSERT_FALSE(epd_classify_rgb(s_image, 16, -1, &s, &out));
    TEST_ASSERT_FALSE(epd_classify_rgb(s_image, 16, 16, NULL, &out));
    TEST_ASSERT_FALSE(epd_classify_rgb(s_image, 16, 16, &s, NULL));

    epd_classify_scratch_t short_scratch = s;
    short_scratch.sample_capacity = 4;
    TEST_ASSERT_FALSE(epd_classify_rgb(s_image, 200, 300, &short_scratch, &out));

    epd_classify_scratch_t no_bins = s;
    no_bins.luma_bins = NULL;
    TEST_ASSERT_FALSE(epd_classify_rgb(s_image, 200, 300, &no_bins, &out));

    // A region outside the canvas, which on the device would mean a fit rectangle computed
    // against different dimensions than the canvas actually has.
    static uint8_t canvas_buffer[32 * 32 * 3];
    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, canvas_buffer, sizeof(canvas_buffer), 32, 32));
    TEST_ASSERT_FALSE(epd_classify_canvas(&canvas, 0, 0, 33, 32, &s, &out));
    TEST_ASSERT_FALSE(epd_classify_canvas(&canvas, -1, 0, 16, 16, &s, &out));
    TEST_ASSERT_FALSE(epd_classify_canvas(&canvas, 0, 0, 0, 16, &s, &out));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_images_are_the_same_images);
    RUN_TEST(test_every_fixture_agrees_on_style_and_kind);
    RUN_TEST(test_every_fixture_agrees_on_scores);
    RUN_TEST(test_every_fixture_agrees_on_metrics);
    RUN_TEST(test_canvas_and_rgb_agree);
    RUN_TEST(test_a_region_excludes_the_matte);
    RUN_TEST(test_refuses_rather_than_guessing);

    return UNITY_END();
}
