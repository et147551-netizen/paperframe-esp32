// Parity tests for the epdoptimize port: does src/epd_epdopt.c produce the same bytes as
// epdoptimize does?
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// The expected values in epdopt_fixtures.h were produced by running the library itself
// (tools/epdopt_reference.mjs), so these are not tests of what the pipeline ought to do --
// they are tests that this C is the same function as that JavaScript. The comparison is
// exact. There is no tolerance to widen, and a value in the fixture header must never be
// edited to make a test pass: the reference is the specification, and a mismatch means the
// port is wrong.
//
// Three stages are checked separately and cumulatively, so a failure names the stage.
// Getting the diffusion right while the range compressor is subtly off would otherwise
// look like one inscrutable difference in the final image.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "epd_canvas.h"
#include "epd_colour.h"
#include "epd_dither.h"
#include "epd_epdopt.h"

#include "epdopt_fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// The largest fixture is the 40 x 60 photograph.
#define MAX_PIXELS (64 * 64)
static uint8_t s_buffer[MAX_PIXELS * 3];

static const epd_palette_entry_t *palette_for(int palette_id)
{
    return palette_id == 0 ? EPD_PALETTE_EPDOPT_AITJCIZE : EPD_PALETTE_EPDOPT_SPECTRA6;
}

// Reports the first differing pixel rather than just "not equal": with 7,200 bytes in the
// largest fixture, which pixel disagreed is the whole diagnostic.
static void assert_same(const char *fixture, const char *stage, const uint8_t *actual,
                        const uint8_t *expected, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++) {
        if (actual[i * 3] == expected[i * 3] && actual[i * 3 + 1] == expected[i * 3 + 1] &&
            actual[i * 3 + 2] == expected[i * 3 + 2]) {
            continue;
        }

        char message[256];
        snprintf(message, sizeof(message),
                 "%s/%s: pixel %zu is (%u,%u,%u), reference says (%u,%u,%u)", fixture,
                 stage, i, actual[i * 3], actual[i * 3 + 1], actual[i * 3 + 2],
                 expected[i * 3], expected[i * 3 + 1], expected[i * 3 + 2]);
        TEST_FAIL_MESSAGE(message);
    }
}

// One fixture, all three stages, each compared before the next runs on its output. The
// stages are cumulative in the reference too, so this checks the composition as well as
// the pieces.
static void run_fixture(const epdopt_fixture_t *fixture)
{
    const size_t pixels = (size_t)fixture->width * (size_t)fixture->height;
    TEST_ASSERT_TRUE_MESSAGE(pixels <= MAX_PIXELS, "fixture is bigger than s_buffer");

    epd_epdopt_t cfg = EPD_EPDOPT_BALANCED;
    cfg.palette = palette_for(fixture->palette_id);

    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, s_buffer, sizeof(s_buffer), fixture->width,
                                     fixture->height));

    memcpy(s_buffer, fixture->input, pixels * 3);

    epd_epdopt_tone_map(s_buffer, pixels, &cfg);
    assert_same(fixture->name, "tone", s_buffer, fixture->after_tone, pixels);

    TEST_ASSERT_TRUE(epd_epdopt_range_compress(s_buffer, pixels, &cfg));
    assert_same(fixture->name, "range", s_buffer, fixture->after_range, pixels);

    // Kept before the diffusion so the canvas at this point can be reused for both scan
    // orders without re-running the two stages in front of them.
    static uint8_t ranged[MAX_PIXELS * 3];
    memcpy(ranged, s_buffer, pixels * 3);

    epd_epdopt_diffuse(&canvas, &cfg);
    assert_same(fixture->name, "diffuse", s_buffer, fixture->after_diffuse, pixels);

    // Serpentine scanning. Only the diffusion differs, which is why the fixture carries one
    // extra array rather than a whole second set.
    cfg.serpentine = true;
    memcpy(s_buffer, ranged, pixels * 3);
    epd_epdopt_diffuse(&canvas, &cfg);
    assert_same(fixture->name, "serpentine", s_buffer, fixture->after_diffuse_serpentine,
                pixels);
}

static void test_every_fixture_matches_the_reference(void)
{
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)EPDOPT_FIXTURE_COUNT);
    for (size_t i = 0; i < EPDOPT_FIXTURE_COUNT; i++) {
        run_fixture(&EPDOPT_FIXTURES[i]);
    }
}

// ------------------------------------------------------------------ standalone checks
//
// Properties the fixtures cannot state, either because they hold for every input or
// because they are about how the port is wired into the rest of the frame.

// The claim epd_epdopt_diffuse()'s header makes, and what lets the existing packer be
// reused: after diffusion every pixel is exactly a palette entry, so a nearest-colour pack
// is lossless rather than a second quantisation.
static void test_diffusion_leaves_only_palette_colours(void)
{
    const epd_epdopt_t *cfg = &EPD_EPDOPT_BALANCED;
    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, s_buffer, sizeof(s_buffer), 32, 32));

    for (size_t i = 0; i < 32u * 32u; i++) {
        s_buffer[i * 3 + 0] = (uint8_t)(i * 7);
        s_buffer[i * 3 + 1] = (uint8_t)(i * 13);
        s_buffer[i * 3 + 2] = (uint8_t)(i * 29);
    }

    epd_epdopt_diffuse(&canvas, cfg);

    for (size_t i = 0; i < 32u * 32u; i++) {
        bool found = false;
        for (size_t p = 0; p < EPD_PALETTE_COUNT; p++) {
            if (s_buffer[i * 3 + 0] == cfg->palette[p].r &&
                s_buffer[i * 3 + 1] == cfg->palette[p].g &&
                s_buffer[i * 3 + 2] == cfg->palette[p].b) {
                found = true;
                break;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "diffusion left a colour the panel cannot show");
    }
}

// Packing a diffused canvas must not change any pixel's colour. If it did, the render
// would be quantising twice and the second pass would be undoing the dither.
static void test_packing_a_diffused_canvas_is_lossless(void)
{
    const epd_epdopt_t *cfg = &EPD_EPDOPT_BALANCED;
    const int32_t width = 16;
    const int32_t height = 16;
    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, s_buffer, sizeof(s_buffer), width, height));

    for (size_t i = 0; i < (size_t)(width * height); i++) {
        s_buffer[i * 3 + 0] = (uint8_t)(i * 3);
        s_buffer[i * 3 + 1] = (uint8_t)(255 - i);
        s_buffer[i * 3 + 2] = (uint8_t)(i * 11);
    }
    epd_epdopt_diffuse(&canvas, cfg);

    const epd_render_t pack = {cfg->palette, EPD_TONE_NONE};
    uint8_t packed[16 / 2];

    for (int32_t y = 0; y < height; y++) {
        const uint8_t *row = epd_canvas_row(&canvas, y);
        epd_dither_row_none_cfg(row, packed, (size_t)width, &pack);

        for (int32_t x = 0; x < width; x++) {
            const uint8_t nibble =
                (x & 1) ? (packed[x >> 1] & 0x0f) : (uint8_t)(packed[x >> 1] >> 4);
            const uint8_t *pixel = &row[(size_t)x * 3];

            const epd_palette_entry_t *entry = NULL;
            for (size_t p = 0; p < EPD_PALETTE_COUNT; p++) {
                if (cfg->palette[p].index == nibble) {
                    entry = &cfg->palette[p];
                    break;
                }
            }
            TEST_ASSERT_NOT_NULL_MESSAGE(entry, "packer emitted an index not in the palette");
            TEST_ASSERT_EQUAL_UINT8(pixel[0], entry->r);
            TEST_ASSERT_EQUAL_UINT8(pixel[1], entry->g);
            TEST_ASSERT_EQUAL_UINT8(pixel[2], entry->b);
        }
    }
}

// Diffusion walks logical rows, so rotating the canvas must rotate the result rather than
// producing a differently-dithered picture. Drawn through epd_canvas_pixel() at both
// rotations from the same logical content, the two must agree pixel for pixel in logical
// coordinates -- if they do not, the dither turns with the frame's rotation setting.
static void test_diffusion_follows_logical_orientation(void)
{
    const epd_epdopt_t *cfg = &EPD_EPDOPT_BALANCED;
    static uint8_t other[MAX_PIXELS * 3];
    const int32_t width = 24;
    const int32_t height = 24; // square, so both rotations have the same logical extent

    epd_canvas_t portrait;
    epd_canvas_t landscape;
    TEST_ASSERT_TRUE(epd_canvas_init(&portrait, s_buffer, sizeof(s_buffer), width, height));
    TEST_ASSERT_TRUE(epd_canvas_init(&landscape, other, sizeof(other), width, height));
    epd_canvas_set_rotation(&landscape, 1);

    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            const uint8_t r = (uint8_t)(x * 9);
            const uint8_t g = (uint8_t)(y * 9);
            const uint8_t b = (uint8_t)(x * y);
            uint8_t *a = epd_canvas_pixel(&portrait, x, y);
            uint8_t *c = epd_canvas_pixel(&landscape, x, y);
            TEST_ASSERT_NOT_NULL(a);
            TEST_ASSERT_NOT_NULL(c);
            a[0] = c[0] = r;
            a[1] = c[1] = g;
            a[2] = c[2] = b;
        }
    }

    epd_epdopt_diffuse(&portrait, cfg);
    epd_epdopt_diffuse(&landscape, cfg);

    for (int32_t y = 0; y < height; y++) {
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *a = epd_canvas_pixel(&portrait, x, y);
            const uint8_t *c = epd_canvas_pixel(&landscape, x, y);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(a, c, 3);
        }
    }
}

// The palettes must be in epdoptimize's canonical role order, because that order is what
// breaks distance ties. This is the one property of the tables that a wrong edit would not
// otherwise show up as anything but a handful of differently-coloured pixels.
static void test_epdopt_palettes_are_in_canonical_role_order(void)
{
    const epd_palette_entry_t *tables[] = {
        EPD_PALETTE_EPDOPT_AITJCIZE, EPD_PALETTE_EPDOPT_SPECTRA6,
        EPD_PALETTE_EPDOPT_LEGACY,   EPD_PALETTE_EPDOPT_BOEBER,
        EPD_PALETTE_EPDOPT_ORIGINAL,
    };
    // black, white, blue, green, red, yellow -- palette-order.ts:1-27.
    const uint8_t expected[EPD_PALETTE_COUNT] = {0x0, 0x1, 0x5, 0x6, 0x3, 0x2};

    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected[i], tables[t][i].index,
                                            "palette is not in canonical role order");
        }
    }
}

// The presets are ported values, not invented ones. Wrong numbers here would silently
// change every photograph, and the fixtures cannot catch it: they were generated for the
// balanced preset, so a bad EPD_EPDOPT_BALANCED would simply fail everywhere without
// saying why.
static void test_balanced_preset_matches_upstream(void)
{
    TEST_ASSERT_TRUE(EPD_EPDOPT_BALANCED.tone_enabled);
    TEST_ASSERT_EQUAL_INT(EPD_TONE_MODE_CONTRAST, EPD_EPDOPT_BALANCED.tone_mode);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, EPD_EPDOPT_BALANCED.exposure);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, EPD_EPDOPT_BALANCED.saturation);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, EPD_EPDOPT_BALANCED.contrast);
    TEST_ASSERT_EQUAL_INT(EPD_RANGE_MODE_DISPLAY, EPD_EPDOPT_BALANCED.range_mode);
    TEST_ASSERT_EQUAL_INT(EPD_RANGE_ACCURATE, EPD_EPDOPT_BALANCED.range_quality);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, EPD_EPDOPT_BALANCED.range_strength);
    TEST_ASSERT_FALSE(EPD_EPDOPT_BALANCED.serpentine);
    TEST_ASSERT_EQUAL_PTR(&EPD_DIFFUSION_FLOYD_STEINBERG, EPD_EPDOPT_BALANCED.kernel);

    // The fast variant differs in exactly one field. If it ever differs in two, one of
    // them is a typo rather than a decision.
    TEST_ASSERT_EQUAL_INT(EPD_RANGE_FAST, EPD_EPDOPT_BALANCED_FAST.range_quality);
    TEST_ASSERT_EQUAL_INT(EPD_EPDOPT_BALANCED.range_mode, EPD_EPDOPT_BALANCED_FAST.range_mode);
    TEST_ASSERT_EQUAL_PTR(EPD_EPDOPT_BALANCED.palette, EPD_EPDOPT_BALANCED_FAST.palette);
    TEST_ASSERT_EQUAL_PTR(EPD_EPDOPT_BALANCED.kernel, EPD_EPDOPT_BALANCED_FAST.kernel);
}

// diffusion-maps.ts factors sum to 1: error is redistributed, not created or destroyed.
static void test_diffusion_kernels_conserve_error(void)
{
    const epd_diffusion_kernel_t *kernels[] = {&EPD_DIFFUSION_FLOYD_STEINBERG,
                                              &EPD_DIFFUSION_STUCKI};

    for (size_t k = 0; k < sizeof(kernels) / sizeof(kernels[0]); k++) {
        double total = 0.0;
        for (size_t t = 0; t < kernels[k]->count; t++) {
            total += kernels[k]->taps[t].factor;
            // No tap may point backwards on the current row: the error would land on a
            // pixel already emitted and be silently dropped.
            const bool forward = kernels[k]->taps[t].dy > 0 ||
                                 (kernels[k]->taps[t].dy == 0 && kernels[k]->taps[t].dx > 0);
            TEST_ASSERT_TRUE_MESSAGE(forward, "diffusion tap points at an emitted pixel");
        }
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, total);
    }
}

// epd_clamp_byte() is JavaScript's Math.round after a clamp, which is floor(v + 0.5) --
// not lround(), not a cast. Every ported stage depends on this and it is one line to get
// wrong.
static void test_clamp_byte_rounds_halves_upwards(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, epd_clamp_byte(-40.0));
    TEST_ASSERT_EQUAL_UINT8(255, epd_clamp_byte(400.0));
    TEST_ASSERT_EQUAL_UINT8(0, epd_clamp_byte(0.4));
    TEST_ASSERT_EQUAL_UINT8(1, epd_clamp_byte(0.5));
    TEST_ASSERT_EQUAL_UINT8(2, epd_clamp_byte(1.5));
    TEST_ASSERT_EQUAL_UINT8(3, epd_clamp_byte(2.5)); // banker's rounding would give 2
    TEST_ASSERT_EQUAL_UINT8(128, epd_clamp_byte(127.5));
    TEST_ASSERT_EQUAL_UINT8(0, epd_clamp_byte(0.0 / 0.0)); // NaN, per processing.ts:316
}

// A round trip through L*a*b* has to come back to the same byte for in-gamut colours, or
// the range compressor is adding error before it has done anything.
static void test_lab_round_trip_is_stable_in_gamut(void)
{
    for (int r = 0; r < 256; r += 17) {
        for (int g = 0; g < 256; g += 17) {
            for (int b = 0; b < 256; b += 17) {
                double l, a, bb;
                epd_rgb_to_lab((uint8_t)r, (uint8_t)g, (uint8_t)b, &l, &a, &bb);
                uint8_t br, bg, bbb;
                epd_lab_to_rgb(l, a, bb, &br, &bg, &bbb);
                TEST_ASSERT_UINT8_WITHIN(1, r, br);
                TEST_ASSERT_UINT8_WITHIN(1, g, bg);
                TEST_ASSERT_UINT8_WITHIN(1, b, bbb);
            }
        }
    }
}

// The two lightness routines must agree: one is used for the auto-mode histogram and the
// other for the compression itself, and a disagreement would move pixels relative to the
// percentiles measured over them.
static void test_lab_lightness_agrees_with_the_full_conversion(void)
{
    for (int v = 0; v < 256; v += 1) {
        double l, a, b;
        epd_rgb_to_lab((uint8_t)v, (uint8_t)v, (uint8_t)v, &l, &a, &b);
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, l,
                                  epd_rgb_to_lab_lightness((uint8_t)v, (uint8_t)v, (uint8_t)v));
    }
}

// Rejecting bad arguments rather than writing through a null pointer, the same contract the
// existing dither functions have.
static void test_rejects_null(void)
{
    epd_epdopt_tone_map(NULL, 4, &EPD_EPDOPT_BALANCED);
    epd_epdopt_tone_map(s_buffer, 4, NULL);
    TEST_ASSERT_TRUE(epd_epdopt_range_compress(NULL, 4, &EPD_EPDOPT_BALANCED));
    TEST_ASSERT_TRUE(epd_epdopt_range_compress(s_buffer, 4, NULL));
    epd_epdopt_diffuse(NULL, &EPD_EPDOPT_BALANCED);
    TEST_ASSERT_FALSE(epd_epdopt_render(NULL, &EPD_EPDOPT_BALANCED, true));

    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, s_buffer, sizeof(s_buffer), 4, 4));
    epd_epdopt_diffuse(&canvas, NULL);
    TEST_ASSERT_FALSE(epd_epdopt_render(&canvas, NULL, true));
}

// The composed entry point must be the same function as the stages run by hand -- it is
// what production calls and the fixtures exercise the stages, so a divergence between the
// two would be invisible.
static void test_render_matches_the_stages_run_by_hand(void)
{
    static uint8_t composed[MAX_PIXELS * 3];
    const int32_t width = 20;
    const int32_t height = 20;
    const size_t pixels = (size_t)width * (size_t)height;

    epd_canvas_t staged;
    epd_canvas_t whole;
    TEST_ASSERT_TRUE(epd_canvas_init(&staged, s_buffer, sizeof(s_buffer), width, height));
    TEST_ASSERT_TRUE(epd_canvas_init(&whole, composed, sizeof(composed), width, height));

    for (size_t i = 0; i < pixels; i++) {
        const uint8_t v[3] = {(uint8_t)(i * 5), (uint8_t)(255 - i * 3), (uint8_t)(i * 17)};
        memcpy(&s_buffer[i * 3], v, 3);
        memcpy(&composed[i * 3], v, 3);
    }

    epd_epdopt_tone_map(s_buffer, pixels, &EPD_EPDOPT_BALANCED);
    TEST_ASSERT_TRUE(epd_epdopt_range_compress(s_buffer, pixels, &EPD_EPDOPT_BALANCED));
    epd_epdopt_diffuse(&staged, &EPD_EPDOPT_BALANCED);

    TEST_ASSERT_TRUE(epd_epdopt_render(&whole, &EPD_EPDOPT_BALANCED, true));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_buffer, composed, pixels * 3);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_every_fixture_matches_the_reference);

    RUN_TEST(test_diffusion_leaves_only_palette_colours);
    RUN_TEST(test_packing_a_diffused_canvas_is_lossless);
    RUN_TEST(test_diffusion_follows_logical_orientation);
    RUN_TEST(test_diffusion_kernels_conserve_error);

    RUN_TEST(test_epdopt_palettes_are_in_canonical_role_order);
    RUN_TEST(test_balanced_preset_matches_upstream);

    RUN_TEST(test_clamp_byte_rounds_halves_upwards);
    RUN_TEST(test_lab_round_trip_is_stable_in_gamut);
    RUN_TEST(test_lab_lightness_agrees_with_the_full_conversion);

    RUN_TEST(test_render_matches_the_stages_run_by_hand);
    RUN_TEST(test_rejects_null);

    return UNITY_END();
}
