// Host-side tests for the canvas.
//
// Small canvases, not 400x600, so a wrong offset shows up as a readable coordinate
// rather than as a smear somewhere in 720 KB.

#include <string.h>

#include <unity.h>

#include "epd_canvas.h"

void setUp(void) {}
void tearDown(void) {}

// 4 wide x 6 high: portrait like the panel, small enough to reason about by hand.
#define W 4
#define H 6
static uint8_t buffer[W * H * 3];
static epd_canvas_t canvas;

static void make_canvas(void)
{
    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, buffer, sizeof(buffer), W, H));
}

static void assert_physical_pixel(int32_t px, int32_t py, uint8_t r, uint8_t g, uint8_t b)
{
    const size_t off = ((size_t)py * W + (size_t)px) * 3;
    TEST_ASSERT_EQUAL_UINT8(r, buffer[off + 0]);
    TEST_ASSERT_EQUAL_UINT8(g, buffer[off + 1]);
    TEST_ASSERT_EQUAL_UINT8(b, buffer[off + 2]);
}

// ------------------------------------------------------------------------- init

static void test_init_binds_a_buffer(void)
{
    make_canvas();
    TEST_ASSERT_EQUAL_PTR(buffer, canvas.rgb);
    TEST_ASSERT_EQUAL_INT32(W, canvas.width);
    TEST_ASSERT_EQUAL_INT32(H, canvas.height);
    TEST_ASSERT_EQUAL_UINT8(0, canvas.rotation);
}

// A short buffer must be refused here. Accepting it turns into heap corruption in
// whichever function writes the last row, a long way from the mistake.
static void test_init_refuses_a_short_buffer(void)
{
    epd_canvas_t c;
    TEST_ASSERT_FALSE(epd_canvas_init(&c, buffer, sizeof(buffer) - 1, W, H));
}

static void test_init_refuses_bad_arguments(void)
{
    epd_canvas_t c;
    TEST_ASSERT_FALSE(epd_canvas_init(&c, NULL, sizeof(buffer), W, H));
    TEST_ASSERT_FALSE(epd_canvas_init(&c, buffer, sizeof(buffer), 0, H));
    TEST_ASSERT_FALSE(epd_canvas_init(&c, buffer, sizeof(buffer), W, -1));
}

static void test_bytes_helper_matches_the_panel(void)
{
    TEST_ASSERT_EQUAL_UINT32(720000u, (uint32_t)epd_canvas_bytes(400, 600));
}

// --------------------------------------------------------------------- rotation

static void test_logical_dimensions_swap_with_rotation(void)
{
    make_canvas();
    TEST_ASSERT_EQUAL_INT32(W, epd_canvas_logical_width(&canvas));
    TEST_ASSERT_EQUAL_INT32(H, epd_canvas_logical_height(&canvas));

    epd_canvas_set_rotation(&canvas, 1);
    TEST_ASSERT_EQUAL_INT32(H, epd_canvas_logical_width(&canvas));
    TEST_ASSERT_EQUAL_INT32(W, epd_canvas_logical_height(&canvas));

    // Ticket 69: 2 shares rotation 0's logical shape and 3 shares rotation 1's. **This is the
    // property the rest of the frame rests on, not an incidental one** -- SMB_RESIZE_FIT_EDGE
    // stores one square because the logical dimensions take only TWO values across all four
    // directions, and app_display.c's auto-rotate turns by `^ 1u` for the same reason.
    epd_canvas_set_rotation(&canvas, 2);
    TEST_ASSERT_EQUAL_INT32(W, epd_canvas_logical_width(&canvas));
    TEST_ASSERT_EQUAL_INT32(H, epd_canvas_logical_height(&canvas));

    epd_canvas_set_rotation(&canvas, 3);
    TEST_ASSERT_EQUAL_INT32(H, epd_canvas_logical_width(&canvas));
    TEST_ASSERT_EQUAL_INT32(W, epd_canvas_logical_height(&canvas));
}

static void test_set_rotation_ignores_unsupported_values(void)
{
    make_canvas();
    epd_canvas_set_rotation(&canvas, 1);
    epd_canvas_set_rotation(&canvas, 4); // one past the last quarter turn; must not take effect
    TEST_ASSERT_EQUAL_UINT8(1, canvas.rotation);
    epd_canvas_set_rotation(&canvas, 255);
    TEST_ASSERT_EQUAL_UINT8(1, canvas.rotation);

    // And 2 and 3 must be ACCEPTED, which is the half of this test that would have caught a
    // partially widened build -- ticket 69 §6, where the symptom is a setting that sticks
    // everywhere except on the glass.
    epd_canvas_set_rotation(&canvas, 2);
    TEST_ASSERT_EQUAL_UINT8(2, canvas.rotation);
    epd_canvas_set_rotation(&canvas, 3);
    TEST_ASSERT_EQUAL_UINT8(3, canvas.rotation);
}

// Logical (0,0) at each rotation, which pins the mapping's handedness. One corner per
// direction is enough to catch a transposition or a mirror, and the four together catch a
// direction that duplicates another -- which is what makes them worth writing out rather than
// deriving: a table generated from offset_of() would agree with any offset_of().
//
// Rotation 1 is the original and its corner is the panel's bottom-left, so an image drawn from
// the logical origin comes out upright when the device is turned.
static void test_rotation_maps_logical_origin_to_physical_corner(void)
{
    static const struct {
        uint8_t rotation;
        int32_t px, py;
    } corners[] = {
        {0, 0, 0},
        {1, 0, H - 1},
        {2, W - 1, H - 1},
        {3, W - 1, 0},
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        make_canvas();
        epd_canvas_set_rotation(&canvas, corners[i].rotation);
        epd_canvas_fill_rect(&canvas, 0, 0, 1, 1, 10, 20, 30);
        assert_physical_pixel(corners[i].px, corners[i].py, 10, 20, 30);
    }
    // All four corners are distinct, so no two rotations share an origin. Asserted rather than
    // eyeballed from the table above, because a copy-paste that gave 1 and 3 the same corner
    // would still pass every assertion in the loop.
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = i + 1; j < 4; j++) {
            TEST_ASSERT_FALSE(corners[i].px == corners[j].px && corners[i].py == corners[j].py);
        }
    }
}

static void test_rotation_covers_every_pixel_exactly_once(void)
{
    // Every rotation, because a bijection at rotation 1 says nothing about rotation 3 -- and an
    // off-by-one in one of the two `k - 1 - v` terms leaves a row unpainted and another written
    // twice, which this catches and a corner check does not.
    for (uint8_t rot = 0; rot <= EPD_CANVAS_ROTATION_MAX; rot++) {
        make_canvas();
        epd_canvas_set_rotation(&canvas, rot);
        TEST_ASSERT_EQUAL_UINT8(rot, canvas.rotation);

        // Paint the whole logical area one pixel at a time; nothing may be left unpainted,
        // which would mean the mapping is not a bijection.
        for (int32_t y = 0; y < epd_canvas_logical_height(&canvas); y++) {
            for (int32_t x = 0; x < epd_canvas_logical_width(&canvas); x++) {
                epd_canvas_fill_rect(&canvas, x, y, 1, 1, 1, 2, 3);
            }
        }
        for (size_t i = 0; i < W * H; i++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, buffer[i * 3 + 0], "unpainted pixel");
            TEST_ASSERT_EQUAL_UINT8(2, buffer[i * 3 + 1]);
            TEST_ASSERT_EQUAL_UINT8(3, buffer[i * 3 + 2]);
        }
    }
}

// The second invariant of ticket 69 §2, and the one that catches a mis-derived `step_x`: the
// affine steps epd_canvas_logical_walk() hands out must agree with epd_canvas_pixel() over the
// whole rectangle, at every rotation.
//
// **Checked against epd_canvas_pixel() and not against a table**, which is the whole point --
// the walk exists to avoid a function call per pixel, so the pixel accessor is the independent
// authority for what it should produce. A table would be the same derivation written twice.
static void test_logical_walk_agrees_with_the_pixel_accessor(void)
{
    for (uint8_t rot = 0; rot <= EPD_CANVAS_ROTATION_MAX; rot++) {
        make_canvas();
        epd_canvas_set_rotation(&canvas, rot);
        const int32_t lw = epd_canvas_logical_width(&canvas);
        const int32_t lh = epd_canvas_logical_height(&canvas);

        // An interior rectangle, so a sign error cannot be hidden by the canvas edge, and one
        // that is not square, so a transposition is visible.
        const int32_t rx = 1, ry = 1, rw = lw - 2, rh = lh - 2;
        // **AND BOTH EXTENTS MUST BE AT LEAST 2, WHICH IS ASSERTED AND NOT ASSUMED.** The first
        // version of this test used `rh = lh - 3`, which on a 4x6 canvas is **1** at the odd
        // rotations -- so `dy` never left 0, `step_y` was never multiplied by anything, and the
        // test passed with rotation 3's `step_y` sign deliberately inverted. A fixture that does
        // not exercise the axis it was written for is the failure mode this project has now hit
        // six times (docs/handover-auto-flow.md); the cheap defence is to state the
        // precondition the test needs.
        TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(2, rw, "rectangle too thin to test step_x");
        TEST_ASSERT_GREATER_OR_EQUAL_INT32_MESSAGE(2, rh, "rectangle too thin to test step_y");
        uint8_t *origin = NULL;
        ptrdiff_t sx = 0, sy = 0;
        TEST_ASSERT_TRUE(epd_canvas_logical_walk(&canvas, rx, ry, rw, rh, &origin, &sx, &sy));

        for (int32_t dy = 0; dy < rh; dy++) {
            for (int32_t dx = 0; dx < rw; dx++) {
                const uint8_t *walked = origin + dx * sx + dy * sy;
                const uint8_t *direct = epd_canvas_pixel(&canvas, rx + dx, ry + dy);
                TEST_ASSERT_NOT_NULL(direct);
                TEST_ASSERT_EQUAL_PTR_MESSAGE(direct, walked, "walk disagrees with pixel()");
            }
        }
    }
}

// And the rectangle form, for the per-pixel adjustment stages that walk physical rows with a
// stride. Same authority: the physical rect must be exactly the bounding box of the pixels
// epd_canvas_pixel() hands back for that logical rectangle.
static void test_physical_rect_is_the_bounding_box_of_the_pixels(void)
{
    for (uint8_t rot = 0; rot <= EPD_CANVAS_ROTATION_MAX; rot++) {
        make_canvas();
        epd_canvas_set_rotation(&canvas, rot);
        const int32_t lw = epd_canvas_logical_width(&canvas);
        const int32_t lh = epd_canvas_logical_height(&canvas);
        const int32_t rx = 1, ry = 1, rw = lw - 2, rh = lh - 2;
        // Same precondition as the walk test above, and for the same reason it exists there.
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(2, rw);
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(2, rh);

        int32_t px = 0, py = 0, pw = 0, ph = 0;
        TEST_ASSERT_TRUE(epd_canvas_physical_rect(&canvas, rx, ry, rw, rh, &px, &py, &pw, &ph));

        int32_t minx = W, miny = H, maxx = -1, maxy = -1;
        for (int32_t dy = 0; dy < rh; dy++) {
            for (int32_t dx = 0; dx < rw; dx++) {
                const uint8_t *p = epd_canvas_pixel(&canvas, rx + dx, ry + dy);
                TEST_ASSERT_NOT_NULL(p);
                const int32_t off = (int32_t)(p - buffer) / 3;
                const int32_t x = off % W, y = off / W;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
        TEST_ASSERT_EQUAL_INT32_MESSAGE(minx, px, "physical_rect x");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(miny, py, "physical_rect y");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(maxx - minx + 1, pw, "physical_rect w");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(maxy - miny + 1, ph, "physical_rect h");
    }
}

// ------------------------------------------------------------------------- fill

static void test_fill_covers_the_whole_buffer(void)
{
    make_canvas();
    epd_canvas_fill(&canvas, 7, 8, 9);
    for (size_t i = 0; i < W * H; i++) {
        TEST_ASSERT_EQUAL_UINT8(7, buffer[i * 3 + 0]);
        TEST_ASSERT_EQUAL_UINT8(8, buffer[i * 3 + 1]);
        TEST_ASSERT_EQUAL_UINT8(9, buffer[i * 3 + 2]);
    }
}

// White is the common case (every render starts with it) and takes the memset path.
static void test_fill_grey_path_matches_the_general_path(void)
{
    make_canvas();
    epd_canvas_fill(&canvas, 255, 255, 255);
    for (size_t i = 0; i < W * H * 3; i++) {
        TEST_ASSERT_EQUAL_UINT8(255, buffer[i]);
    }
}

// -------------------------------------------------------------------- fill_rect

static void test_fill_rect_writes_only_inside(void)
{
    make_canvas();
    epd_canvas_fill_rect(&canvas, 1, 2, 2, 2, 100, 110, 120);

    assert_physical_pixel(1, 2, 100, 110, 120);
    assert_physical_pixel(2, 3, 100, 110, 120);
    assert_physical_pixel(0, 0, 0, 0, 0);   // outside
    assert_physical_pixel(3, 4, 0, 0, 0);   // outside
}

static void test_fill_rect_clips_at_every_edge(void)
{
    make_canvas();
    epd_canvas_fill_rect(&canvas, -5, -5, 100, 100, 50, 60, 70);
    // Everything covered, nothing written out of bounds -- ASan/valgrind would catch the
    // latter; here we simply require the visible result to be a full cover.
    for (size_t i = 0; i < W * H; i++) {
        TEST_ASSERT_EQUAL_UINT8(50, buffer[i * 3]);
    }
}

static void test_fill_rect_ignores_empty_and_negative_extents(void)
{
    make_canvas();
    epd_canvas_fill_rect(&canvas, 0, 0, 0, 5, 9, 9, 9);
    epd_canvas_fill_rect(&canvas, 0, 0, 5, -3, 9, 9, 9);
    for (size_t i = 0; i < W * H * 3; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, buffer[i]);
    }
}

// -------------------------------------------------------------------------- blit

static void test_blit_at_native_size_is_a_copy(void)
{
    make_canvas();
    uint8_t src[W * H * 3];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i * 7);
    }

    const epd_fit_t fit = epd_fit_centre(W, H, W, H);
    epd_canvas_blit(&canvas, src, W, H, fit);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, buffer, sizeof(src));
}

static void test_blit_centres_a_letterboxed_image(void)
{
    make_canvas();
    epd_canvas_fill(&canvas, 255, 255, 255);

    // 4x2 source into a 4x6 canvas: full width, centred vertically, rows 2 and 3.
    uint8_t src[4 * 2 * 3];
    memset(src, 40, sizeof(src));

    const epd_fit_t fit = epd_fit_centre(4, 2, W, H);
    TEST_ASSERT_EQUAL_INT32(4, fit.width);
    TEST_ASSERT_EQUAL_INT32(2, fit.height);
    TEST_ASSERT_EQUAL_INT32(0, fit.x);
    TEST_ASSERT_EQUAL_INT32(2, fit.y);

    epd_canvas_blit(&canvas, src, 4, 2, fit);

    assert_physical_pixel(0, 1, 255, 255, 255); // matte above
    assert_physical_pixel(0, 2, 40, 40, 40);    // image
    assert_physical_pixel(3, 3, 40, 40, 40);
    assert_physical_pixel(0, 4, 255, 255, 255); // matte below
}

static void test_blit_scales_up_without_reading_past_the_source(void)
{
    make_canvas();
    // 1x1 source scaled to fill: every destination pixel is that one colour, and the
    // sampler must never index beyond the single source pixel.
    const uint8_t src[3] = {77, 88, 99};
    const epd_fit_t fit = epd_fit_centre(1, 1, W, H);
    epd_canvas_blit(&canvas, src, 1, 1, fit);

    for (int32_t y = fit.y; y < fit.y + fit.height; y++) {
        for (int32_t x = fit.x; x < fit.x + fit.width; x++) {
            assert_physical_pixel(x, y, 77, 88, 99);
        }
    }
}

static void test_blit_rejects_bad_input(void)
{
    make_canvas();
    const uint8_t src[3] = {1, 2, 3};
    const epd_fit_t good = epd_fit_centre(1, 1, W, H);

    epd_canvas_blit(&canvas, NULL, 1, 1, good);
    epd_canvas_blit(&canvas, src, 0, 1, good);
    epd_canvas_blit(&canvas, src, 1, 1, epd_fit_centre(0, 0, W, H)); // zeroed fit

    for (size_t i = 0; i < W * H * 3; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, buffer[i]);
    }
}

// --------------------------------------------------------------------------- rows

static void test_row_returns_physical_rows_regardless_of_rotation(void)
{
    make_canvas();
    epd_canvas_fill(&canvas, 5, 6, 7);
    epd_canvas_set_rotation(&canvas, 1);

    // The panel scans in native orientation, so there must still be `height` rows of
    // `width` pixels no matter what the application thinks it drew.
    for (int32_t y = 0; y < H; y++) {
        const uint8_t *row = epd_canvas_row(&canvas, y);
        TEST_ASSERT_NOT_NULL(row);
        TEST_ASSERT_EQUAL_PTR(&buffer[(size_t)y * W * 3], row);
    }
}

static void test_row_rejects_out_of_range(void)
{
    make_canvas();
    TEST_ASSERT_NULL(epd_canvas_row(&canvas, -1));
    TEST_ASSERT_NULL(epd_canvas_row(&canvas, H));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_binds_a_buffer);
    RUN_TEST(test_init_refuses_a_short_buffer);
    RUN_TEST(test_init_refuses_bad_arguments);
    RUN_TEST(test_bytes_helper_matches_the_panel);

    RUN_TEST(test_logical_dimensions_swap_with_rotation);
    RUN_TEST(test_set_rotation_ignores_unsupported_values);
    RUN_TEST(test_rotation_maps_logical_origin_to_physical_corner);
    RUN_TEST(test_rotation_covers_every_pixel_exactly_once);
    RUN_TEST(test_logical_walk_agrees_with_the_pixel_accessor);
    RUN_TEST(test_physical_rect_is_the_bounding_box_of_the_pixels);

    RUN_TEST(test_fill_covers_the_whole_buffer);
    RUN_TEST(test_fill_grey_path_matches_the_general_path);

    RUN_TEST(test_fill_rect_writes_only_inside);
    RUN_TEST(test_fill_rect_clips_at_every_edge);
    RUN_TEST(test_fill_rect_ignores_empty_and_negative_extents);

    RUN_TEST(test_blit_at_native_size_is_a_copy);
    RUN_TEST(test_blit_centres_a_letterboxed_image);
    RUN_TEST(test_blit_scales_up_without_reading_past_the_source);
    RUN_TEST(test_blit_rejects_bad_input);

    RUN_TEST(test_row_returns_physical_rows_regardless_of_rotation);
    RUN_TEST(test_row_rejects_out_of_range);

    return UNITY_END();
}
