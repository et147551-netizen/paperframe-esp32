// Is src/core/epd_diffuse.c's integer Floyd-Steinberg the same function as epdoptimize's?
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// **The reference is the library, not the port.** test/test_epdopt/epdopt_fixtures.h was generated
// by running epdoptimize itself (tools/epdopt_reference.mjs) and its stages are cumulative, so
// `after_range` is exactly the input the diffusion sees and `after_diffuse` is exactly what the
// JavaScript produces from it. Reusing that header means this suite does not inherit any mistake
// src/core/epd_epdopt.c might contain, and it is why the comparison here can be exact rather than
// tolerance-based: epd_diffuse.h's argument is that the integer form is bit-identical, and a
// tolerance would hide the one thing worth knowing.
//
// A value in that fixture header must never be edited to make a test here pass.
//
// The other three tests are about the parts the library has no opinion on, because its canvas is
// always the whole image in its natural orientation: the affine walk that lets a rotated canvas be
// diffused in the order the picture was drawn, the region confinement that keeps the white matte
// out of the picture's statistics and error, and the banding that gives the display task a yield
// point. Each is checked by construction against the same library bytes rather than against
// epd_diffuse.c's own output, so none of them is circular.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "epd_canvas.h"
#include "epd_diffuse.h"
#include "epd_dither.h"

#include "../test_epdopt/epdopt_fixtures.h"

void setUp(void) {}
void tearDown(void) {}

// The largest fixture is 32 x 48; the region test pads one by a margin on every side.
#define MAX_PIXELS (64 * 64)
static uint8_t s_buffer[MAX_PIXELS * 3];
static uint8_t s_expected[MAX_PIXELS * 3];

// Same two palettes the fixtures were generated against, in the same order.
static const epd_palette_entry_t *palette_for(int palette_id)
{
    return palette_id == 0 ? EPD_PALETTE_EPDOPT_AITJCIZE : EPD_PALETTE_EPDOPT_SPECTRA6;
}

// Reports the first differing pixel rather than just "not equal": with 4,608 bytes in the largest
// fixture, which pixel disagreed is the whole diagnostic. Same shape as test_epdopt's.
static void assert_same(const char *fixture, const char *what, const uint8_t *actual,
                        const uint8_t *expected, size_t pixels)
{
    for (size_t i = 0; i < pixels; i++) {
        if (actual[i * 3] == expected[i * 3] && actual[i * 3 + 1] == expected[i * 3 + 1] &&
            actual[i * 3 + 2] == expected[i * 3 + 2]) {
            continue;
        }

        char message[256];
        snprintf(message, sizeof(message),
                 "%s/%s: pixel %zu is (%u,%u,%u), reference says (%u,%u,%u)", fixture, what, i,
                 actual[i * 3], actual[i * 3 + 1], actual[i * 3 + 2], expected[i * 3],
                 expected[i * 3 + 1], expected[i * 3 + 2]);
        TEST_FAIL_MESSAGE(message);
    }
}

// ------------------------------------------------------------------ the load-bearing test

// Every fixture, both scan orders, against the library's own bytes. If the exactness argument in
// epd_diffuse.h is wrong anywhere, it is wrong here.
static void test_matches_the_library_bytes(void)
{
    for (size_t f = 0; f < EPDOPT_FIXTURE_COUNT; f++) {
        const epdopt_fixture_t *fx = &EPDOPT_FIXTURES[f];
        const size_t pixels = (size_t)fx->width * (size_t)fx->height;
        TEST_ASSERT_TRUE_MESSAGE(pixels <= MAX_PIXELS, "fixture is bigger than s_buffer");

        epd_diffuse_t cfg = {.palette = palette_for(fx->palette_id), .serpentine = false};

        memcpy(s_buffer, fx->after_range, pixels * 3);
        epd_diffuse_rect(s_buffer, fx->width, fx->height, 3, (ptrdiff_t)fx->width * 3, &cfg, 0,
                         fx->height);
        assert_same(fx->name, "diffuse", s_buffer, fx->after_diffuse, pixels);

        cfg.serpentine = true;
        memcpy(s_buffer, fx->after_range, pixels * 3);
        epd_diffuse_rect(s_buffer, fx->width, fx->height, 3, (ptrdiff_t)fx->width * 3, &cfg, 0,
                         fx->height);
        assert_same(fx->name, "serpentine", s_buffer, fx->after_diffuse_serpentine, pixels);
    }
}

// ------------------------------------------------------------------ the affine walk

// A rotation-1 canvas holds the picture turned 90 degrees in memory, so a logical row runs
// backwards through it. Diffusing such a canvas must still produce the library's bytes: the
// diffusion direction follows the picture, not the panel's scan order. This is what
// epd_canvas_logical_walk() is for, and getting its two steps backwards would show up here and
// nowhere else -- at rotation 0 the logical and physical walks are the same walk, which is also
// why the hardware measurement of the two orders has to be taken at rotation 1.
static void test_rotation_1_diffuses_the_picture_not_the_panel(void)
{
    for (size_t f = 0; f < EPDOPT_FIXTURE_COUNT; f++) {
        const epdopt_fixture_t *fx = &EPDOPT_FIXTURES[f];
        const size_t pixels = (size_t)fx->width * (size_t)fx->height;

        // Logical w x h at rotation 1 needs a physical canvas of h x w.
        epd_canvas_t canvas;
        TEST_ASSERT_TRUE(epd_canvas_init(&canvas, s_buffer, sizeof(s_buffer), fx->height,
                                         fx->width));
        epd_canvas_set_rotation(&canvas, 1);
        TEST_ASSERT_EQUAL_INT32(fx->width, epd_canvas_logical_width(&canvas));
        TEST_ASSERT_EQUAL_INT32(fx->height, epd_canvas_logical_height(&canvas));

        for (int32_t y = 0; y < fx->height; y++) {
            for (int32_t x = 0; x < fx->width; x++) {
                uint8_t *px = epd_canvas_pixel(&canvas, x, y);
                TEST_ASSERT_NOT_NULL(px);
                memcpy(px, &fx->after_range[((size_t)y * (size_t)fx->width + (size_t)x) * 3], 3);
            }
        }

        uint8_t *origin = NULL;
        ptrdiff_t step_x = 0, step_y = 0;
        TEST_ASSERT_TRUE(epd_canvas_logical_walk(&canvas, 0, 0, fx->width, fx->height, &origin,
                                                 &step_x, &step_y));
        TEST_ASSERT_EQUAL_INT32(-(ptrdiff_t)fx->height * 3, step_x);
        TEST_ASSERT_EQUAL_INT32(3, step_y);

        const epd_diffuse_t cfg = {.palette = palette_for(fx->palette_id), .serpentine = true};
        epd_diffuse_rect(origin, fx->width, fx->height, step_x, step_y, &cfg, 0, fx->height);

        for (int32_t y = 0; y < fx->height; y++) {
            for (int32_t x = 0; x < fx->width; x++) {
                const uint8_t *px = epd_canvas_pixel(&canvas, x, y);
                memcpy(&s_expected[((size_t)y * (size_t)fx->width + (size_t)x) * 3], px, 3);
            }
        }
        assert_same(fx->name, "rot1", s_expected, fx->after_diffuse_serpentine, pixels);
    }
}

// ------------------------------------------------------------------ region confinement

// The canvas is matted white around the picture (FR-5.3) and 255,255,255 is not a palette entry,
// so a whole-canvas diffusion would push the matte's quantisation error into the picture's edge.
// Diffusing the picture's own rectangle must give the library's bytes for the interior and leave
// every surrounding pixel exactly as it was.
//
// **Run at ALL FOUR rotations, because they fail differently.** At rotation 0 a tap past the right
// edge lands on the matte, which this test sees; at rotation 1 the same tap lands `w * step_x` away,
// and with step_x negative that is *before* the origin -- outside the rectangle in a direction a
// whole-canvas check would not think to look. Rotations 2 and 3 arrived with ticket 69 and put the
// negative sign on the other combinations: at 2 **both** steps are negative, and at 3 it is step_y.
// Every rotation with a region is something the device actually runs, so none of these is a
// hypothetical arm.
//
// The expected bytes are the SAME fixture at every rotation, and that is the point: the diffusion
// must walk the picture as it was drawn rather than as the panel is scanned, so turning the canvas
// must not change a single output byte.
#define MARGIN_X 3
#define MARGIN_Y 5

static void region_case(const epdopt_fixture_t *fx, uint8_t rotation)
{
    const size_t pixels = (size_t)fx->width * (size_t)fx->height;
    const int32_t cw = fx->width + MARGIN_X * 2;
    const int32_t ch = fx->height + MARGIN_Y * 2;
    TEST_ASSERT_TRUE_MESSAGE((size_t)cw * (size_t)ch <= MAX_PIXELS, "padded canvas too big");

    // Logical cw x ch needs a physical ch x cw at the ODD rotations -- the same `& 1` epd_canvas.c
    // derives from offset_of(), rather than `== 1`, which silently built a wrongly-shaped canvas
    // at rotation 3 and made every assertion below meaningless.
    const bool turned = (rotation & 1u) != 0u;
    epd_canvas_t canvas;
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, s_buffer, sizeof(s_buffer), turned ? ch : cw,
                                     turned ? cw : ch));
    epd_canvas_set_rotation(&canvas, rotation);
    TEST_ASSERT_EQUAL_INT32(cw, epd_canvas_logical_width(&canvas));
    TEST_ASSERT_EQUAL_INT32(ch, epd_canvas_logical_height(&canvas));
    // White, as the real matte is -- and deliberately not a palette colour, so a leak shows up as
    // a changed byte rather than as a coincidence.
    epd_canvas_fill(&canvas, 255, 255, 255);

    // Pixel by pixel rather than a row memcpy: a logical row is only contiguous at rotation 0.
    for (int32_t y = 0; y < fx->height; y++) {
        for (int32_t x = 0; x < fx->width; x++) {
            uint8_t *px = epd_canvas_pixel(&canvas, MARGIN_X + x, MARGIN_Y + y);
            TEST_ASSERT_NOT_NULL(px);
            memcpy(px, &fx->after_range[((size_t)y * (size_t)fx->width + (size_t)x) * 3], 3);
        }
    }

    uint8_t *origin = NULL;
    ptrdiff_t step_x = 0, step_y = 0;
    TEST_ASSERT_TRUE(epd_canvas_logical_walk(&canvas, MARGIN_X, MARGIN_Y, fx->width, fx->height,
                                             &origin, &step_x, &step_y));

    const epd_diffuse_t cfg = {.palette = palette_for(fx->palette_id), .serpentine = false};
    epd_diffuse_rect(origin, fx->width, fx->height, step_x, step_y, &cfg, 0, fx->height);

    for (int32_t y = 0; y < fx->height; y++) {
        for (int32_t x = 0; x < fx->width; x++) {
            memcpy(&s_expected[((size_t)y * (size_t)fx->width + (size_t)x) * 3],
                   epd_canvas_pixel(&canvas, MARGIN_X + x, MARGIN_Y + y), 3);
        }
    }
    char what[24];
    snprintf(what, sizeof(what), "region-rot%u", (unsigned)rotation);
    assert_same(fx->name, what, s_expected, fx->after_diffuse, pixels);

    for (int32_t y = 0; y < ch; y++) {
        for (int32_t x = 0; x < cw; x++) {
            const bool inside = x >= MARGIN_X && x < MARGIN_X + fx->width && y >= MARGIN_Y &&
                                y < MARGIN_Y + fx->height;
            if (inside) {
                continue;
            }
            const uint8_t *px = epd_canvas_pixel(&canvas, x, y);
            char message[128];
            snprintf(message, sizeof(message), "%s rot%u: matte at %ld,%ld was written", fx->name,
                     (unsigned)rotation, (long)x, (long)y);
            TEST_ASSERT_TRUE_MESSAGE(px[0] == 255 && px[1] == 255 && px[2] == 255, message);
        }
    }
}

static void test_region_does_not_leak(void)
{
    for (size_t f = 0; f < EPDOPT_FIXTURE_COUNT; f++) {
        for (uint8_t rot = 0; rot <= EPD_CANVAS_ROTATION_MAX; rot++) {
            region_case(&EPDOPT_FIXTURES[f], rot);
        }
    }
}

// ------------------------------------------------------------------ banding

// The display task holds CPU 0 for the whole diffusion, so it has to be able to stop between rows
// and let the idle task run. That is only safe if a banded pass is bit-identical to one pass, which
// it is because every scrap of diffusion state lives in the pixels. Odd band heights are the
// interesting case with serpentine on, since a band can start on a right-to-left row.
static void test_banding_equals_one_pass(void)
{
    static const int32_t BANDS[] = {1, 3, 7};

    for (size_t f = 0; f < EPDOPT_FIXTURE_COUNT; f++) {
        const epdopt_fixture_t *fx = &EPDOPT_FIXTURES[f];
        const size_t pixels = (size_t)fx->width * (size_t)fx->height;
        const ptrdiff_t step_y = (ptrdiff_t)fx->width * 3;

        for (size_t b = 0; b < sizeof(BANDS) / sizeof(BANDS[0]); b++) {
            const epd_diffuse_t cfg = {.palette = palette_for(fx->palette_id),
                                       .serpentine = true};

            memcpy(s_buffer, fx->after_range, pixels * 3);
            for (int32_t y = 0; y < fx->height; y += BANDS[b]) {
                epd_diffuse_rect(s_buffer, fx->width, fx->height, 3, step_y, &cfg, y, BANDS[b]);
            }

            char what[32];
            snprintf(what, sizeof(what), "bands-of-%ld", (long)BANDS[b]);
            assert_same(fx->name, what, s_buffer, fx->after_diffuse_serpentine, pixels);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_matches_the_library_bytes);
    RUN_TEST(test_rotation_1_diffuses_the_picture_not_the_panel);
    RUN_TEST(test_region_does_not_leak);
    RUN_TEST(test_banding_equals_one_pass);
    return UNITY_END();
}
