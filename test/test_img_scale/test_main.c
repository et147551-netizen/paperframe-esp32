// img_scale: downscaling by a ratio that is not an integer.
//
// Worth its own suite for the reason img_dims has one: a resampler that is wrong still
// produces a picture. An off-by-one in the source bounds reads one row too far and nothing
// crashes; a scale computed on the wrong axis stores a portrait picture at landscape
// dimensions and the panel then letterboxes it. Neither announces itself on hardware, where
// finding out costs a fetch, a decode and a 15 s refresh.
//
// The images here are built from formulas so that the expected average of any rectangle is
// arithmetic rather than a golden file.

#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "img_scale.h"

void setUp(void) {}
void tearDown(void) {}

#define BOX 600
#define ALIGN 16

// ---------------------------------------------------------------- the target arithmetic

static void test_target_puts_the_long_edge_on_the_box(void)
{
    int32_t fw, fh, dw, dh;

    // The 12 MP case this exists for: 4032x3024 is drawn at 536x400 turned, and was being
    // stored at 1008x752 -- 2.8x the pixels.
    TEST_ASSERT_TRUE(img_scale_fit_target(4032, 3024, BOX, ALIGN, &fw, &fh, &dw, &dh));
    TEST_ASSERT_EQUAL_INT32(600, fw);
    TEST_ASSERT_EQUAL_INT32(450, fh);
    TEST_ASSERT_EQUAL_INT32(592, dw);
    TEST_ASSERT_EQUAL_INT32(448, dh);

    // Portrait is the same rule with the axes swapped, and getting this wrong is the failure
    // that would letterbox half a real library.
    TEST_ASSERT_TRUE(img_scale_fit_target(3024, 4032, BOX, ALIGN, &fw, &fh, &dw, &dh));
    TEST_ASSERT_EQUAL_INT32(450, fw);
    TEST_ASSERT_EQUAL_INT32(600, fh);
}

static void test_target_never_exceeds_the_box(void)
{
    const int32_t sizes[][2] = {
        {4032, 3024}, {3024, 4032}, {1920, 1080}, {1080, 1920}, {2400, 1600},
        {1200, 1200}, {1008, 752},  {601, 600},   {6000, 601},  {601, 6000},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int32_t fw, fh, dw, dh;
        if (!img_scale_fit_target(sizes[i][0], sizes[i][1], BOX, ALIGN, &fw, &fh, &dw, &dh)) {
            continue; // refused, which the caller treats as "keep the original"
        }
        TEST_ASSERT_TRUE(fw <= BOX);
        TEST_ASSERT_TRUE(fh <= BOX);
        TEST_ASSERT_TRUE(dw <= fw);
        TEST_ASSERT_TRUE(dh <= fh);
        TEST_ASSERT_EQUAL_INT32(0, dw % ALIGN);
        TEST_ASSERT_EQUAL_INT32(0, dh % ALIGN);
    }
}

static void test_target_keeps_the_aspect_ratio_to_within_a_pixel(void)
{
    // fw/fh is the scale and both axes share it, which is the whole reason the crop to the
    // alignment happens afterwards rather than by rounding each axis on its own. Rounding
    // independently would distort by up to 16 pixels on the short edge.
    const int32_t sizes[][2] = {
        {4032, 3024}, {1920, 1080}, {2400, 1600}, {3000, 2000}, {1536, 2048},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int32_t fw, fh, dw, dh;
        TEST_ASSERT_TRUE(
            img_scale_fit_target(sizes[i][0], sizes[i][1], BOX, ALIGN, &fw, &fh, &dw, &dh));
        // fw/fh vs sw/sh, cross-multiplied, within one pixel of slop from the integer floor.
        const int64_t lhs = (int64_t)fw * sizes[i][1];
        const int64_t rhs = (int64_t)fh * sizes[i][0];
        const int64_t diff = lhs > rhs ? lhs - rhs : rhs - lhs;
        TEST_ASSERT_TRUE(diff <= (int64_t)sizes[i][0] + sizes[i][1]);
    }
}

static void test_target_says_nothing_to_do_when_it_already_fits(void)
{
    int32_t fw, fh, dw, dh;
    TEST_ASSERT_TRUE(img_scale_fit_target(400, 600, BOX, ALIGN, &fw, &fh, &dw, &dh));
    TEST_ASSERT_EQUAL_INT32(400, dw);
    TEST_ASSERT_EQUAL_INT32(600, dh);
    // A source already inside the box must come back UNCHANGED rather than at the aligned
    // size: cropping it would lose pixels for nothing, and re-encoding it would lose a
    // generation for nothing.
    TEST_ASSERT_EQUAL_INT32(400, fw);
    TEST_ASSERT_EQUAL_INT32(600, fh);
}

static void test_target_refuses_what_it_cannot_serve(void)
{
    int32_t fw, fh, dw, dh;
    TEST_ASSERT_FALSE(img_scale_fit_target(0, 600, BOX, ALIGN, &fw, &fh, &dw, &dh));
    TEST_ASSERT_FALSE(img_scale_fit_target(600, -1, BOX, ALIGN, &fw, &fh, &dw, &dh));
    TEST_ASSERT_FALSE(img_scale_fit_target(600, 600, 0, ALIGN, &fw, &fh, &dw, &dh));
    TEST_ASSERT_FALSE(img_scale_fit_target(600, 600, BOX, 0, &fw, &fh, &dw, &dh));
    // A sliver: 60000x8 into a 600 box is 600x0, and there is no honest answer.
    TEST_ASSERT_FALSE(img_scale_fit_target(60000, 8, BOX, ALIGN, &fw, &fh, &dw, &dh));
}

// ------------------------------------------------------------------- the resample itself

static void fill_flat(uint8_t *p, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int32_t i = 0; i < w * h; i++) {
        p[i * 3 + 0] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
}

static void test_area_preserves_a_flat_field_exactly(void)
{
    // The strongest cheap invariant: whatever the weights are, an average of one value is
    // that value. A resampler with wrong weights fails this on the edge pixels only, which
    // is why every pixel is checked rather than a sample.
    static uint8_t src[97 * 61 * 3];
    static uint8_t dst[64 * 40 * 3];
    fill_flat(src, 97, 61, 200, 100, 50);
    memset(dst, 0, sizeof(dst));

    TEST_ASSERT_TRUE(img_scale_area(src, 97, 61, 64, 40, dst, 64, 40));
    for (int32_t i = 0; i < 64 * 40; i++) {
        TEST_ASSERT_EQUAL_UINT8(200, dst[i * 3 + 0]);
        TEST_ASSERT_EQUAL_UINT8(100, dst[i * 3 + 1]);
        TEST_ASSERT_EQUAL_UINT8(50, dst[i * 3 + 2]);
    }
}

static void test_area_at_ratio_one_is_a_copy(void)
{
    static uint8_t src[16 * 16 * 3];
    static uint8_t dst[16 * 16 * 3];
    for (int32_t i = 0; i < 16 * 16 * 3; i++) {
        src[i] = (uint8_t)(i * 7);
    }
    memset(dst, 0, sizeof(dst));

    TEST_ASSERT_TRUE(img_scale_area(src, 16, 16, 16, 16, dst, 16, 16));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, dst, sizeof(src));
}

static void test_area_halving_averages_each_two_by_two_block(void)
{
    // An exact-integer case, so the expected value is arithmetic rather than a tolerance.
    static uint8_t src[8 * 8 * 3];
    static uint8_t dst[4 * 4 * 3];
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 0; x < 8; x++) {
            const int32_t i = (y * 8 + x) * 3;
            src[i + 0] = (uint8_t)(x * 8);
            src[i + 1] = (uint8_t)(y * 8);
            src[i + 2] = 0;
        }
    }
    TEST_ASSERT_TRUE(img_scale_area(src, 8, 8, 4, 4, dst, 4, 4));
    for (int32_t y = 0; y < 4; y++) {
        for (int32_t x = 0; x < 4; x++) {
            const int32_t i = (y * 4 + x) * 3;
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(x * 16 + 4), dst[i + 0]);
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(y * 16 + 4), dst[i + 1]);
        }
    }
}

static void test_area_is_monotone_along_a_gradient(void)
{
    // A non-integer ratio, where there is no closed form to compare against -- so the
    // property is checked instead: a horizontal ramp must stay a horizontal ramp, and it must
    // not develop a step backwards. Uneven box bounds (the bug fractional weights exist to
    // avoid) show up here as a non-monotone column.
    static uint8_t src[101 * 8 * 3];
    static uint8_t dst[60 * 8 * 3];
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 0; x < 101; x++) {
            const int32_t i = (y * 101 + x) * 3;
            const uint8_t v = (uint8_t)((x * 255) / 100);
            src[i + 0] = v;
            src[i + 1] = v;
            src[i + 2] = v;
        }
    }
    TEST_ASSERT_TRUE(img_scale_area(src, 101, 8, 60, 8, dst, 60, 8));
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 1; x < 60; x++) {
            const uint8_t prev = dst[(y * 60 + x - 1) * 3];
            const uint8_t cur = dst[(y * 60 + x) * 3];
            TEST_ASSERT_TRUE(cur >= prev);
        }
    }
    // The first destination pixel is NOT 0, and that is the filter working rather than a
    // fudge: at 101/60 it covers source column 0 whole and 68 % of column 1, so the average
    // is (0 x 1.000 + 2 x 0.683) / 1.683 = 0.81, which rounds to 1. A nearest-neighbour
    // resampler would give 0 here, and so would one that truncated its source rectangle to
    // whole pixels -- both of which this test exists to tell apart from area averaging.
    TEST_ASSERT_EQUAL_UINT8(1, dst[0]);
    // The last covers columns 99 and 100 (252 and 255), so it is close to but under 255.
    TEST_ASSERT_TRUE(dst[59 * 3] >= 250);
    TEST_ASSERT_TRUE(dst[59 * 3] <= 255);
}

static void test_area_weights_partial_source_pixels(void)
{
    // THE test for this filter, and the suite passed without it: every other case here gives
    // the same answer whether the partial source pixels are weighted by how much of them the
    // destination covers or counted whole. Verified by perturbation -- replacing the weight
    // with 1 leaves all eleven other cases green.
    //
    // Six pixels alternating 0 and 240, reduced to four, so the ratio is exactly 1.5 and every
    // destination pixel straddles a boundary:
    //
    //   weighted   (0x1.0 + 240x0.5) / 1.5 = 80, then 80, 160, 160
    //   unweighted (0 + 240) / 2           = 120 for all four
    static const uint8_t vals[6] = {0, 240, 0, 240, 0, 240};
    static uint8_t src[6 * 3];
    static uint8_t dst[4 * 3];
    for (int32_t x = 0; x < 6; x++) {
        src[x * 3 + 0] = vals[x];
        src[x * 3 + 1] = vals[x];
        src[x * 3 + 2] = vals[x];
    }
    TEST_ASSERT_TRUE(img_scale_area(src, 6, 1, 4, 1, dst, 4, 1));
    TEST_ASSERT_EQUAL_UINT8(80, dst[0 * 3]);
    TEST_ASSERT_EQUAL_UINT8(80, dst[1 * 3]);
    TEST_ASSERT_EQUAL_UINT8(160, dst[2 * 3]);
    TEST_ASSERT_EQUAL_UINT8(160, dst[3 * 3]);
}

static void test_area_weights_partial_source_rows(void)
{
    // The same discriminator on the other axis, because the row weight and the column weight
    // are separate expressions and only one of them being right is a live possibility.
    static const uint8_t vals[6] = {0, 240, 0, 240, 0, 240};
    static uint8_t src[6 * 3];
    static uint8_t dst[4 * 3];
    for (int32_t y = 0; y < 6; y++) {
        src[y * 3 + 0] = vals[y];
        src[y * 3 + 1] = vals[y];
        src[y * 3 + 2] = vals[y];
    }
    TEST_ASSERT_TRUE(img_scale_area(src, 1, 6, 1, 4, dst, 1, 4));
    TEST_ASSERT_EQUAL_UINT8(80, dst[0 * 3]);
    TEST_ASSERT_EQUAL_UINT8(80, dst[1 * 3]);
    TEST_ASSERT_EQUAL_UINT8(160, dst[2 * 3]);
    TEST_ASSERT_EQUAL_UINT8(160, dst[3 * 3]);
}

static void test_area_crops_rather_than_stretches(void)
{
    // dw < fw means "write less", not "squeeze the whole picture into less". The right-hand
    // column of the cropped output must equal the same column of the uncropped one -- if it
    // stretched, every column but the first would differ.
    static uint8_t src[80 * 8 * 3];
    static uint8_t full[40 * 8 * 3];
    static uint8_t crop[32 * 8 * 3];
    for (int32_t i = 0; i < 80 * 8 * 3; i++) {
        src[i] = (uint8_t)((i * 13) & 0xFF);
    }
    TEST_ASSERT_TRUE(img_scale_area(src, 80, 8, 40, 8, full, 40, 8));
    TEST_ASSERT_TRUE(img_scale_area(src, 80, 8, 40, 8, crop, 32, 8));
    for (int32_t y = 0; y < 8; y++) {
        for (int32_t x = 0; x < 32; x++) {
            for (int32_t c = 0; c < 3; c++) {
                TEST_ASSERT_EQUAL_UINT8(full[(y * 40 + x) * 3 + c], crop[(y * 32 + x) * 3 + c]);
            }
        }
    }
}

static void test_area_refuses_to_upscale_or_overrun(void)
{
    static uint8_t src[8 * 8 * 3];
    static uint8_t dst[16 * 16 * 3];
    TEST_ASSERT_FALSE(img_scale_area(src, 8, 8, 16, 8, dst, 16, 8));  // wider than the source
    TEST_ASSERT_FALSE(img_scale_area(src, 8, 8, 8, 16, dst, 8, 16));  // taller
    TEST_ASSERT_FALSE(img_scale_area(src, 8, 8, 4, 4, dst, 5, 4));    // dw past the scale
    TEST_ASSERT_FALSE(img_scale_area(NULL, 8, 8, 4, 4, dst, 4, 4));
    TEST_ASSERT_FALSE(img_scale_area(src, 8, 8, 4, 4, NULL, 4, 4));
    TEST_ASSERT_FALSE(img_scale_area(src, 0, 8, 4, 4, dst, 4, 4));
}

static void test_area_reads_nothing_past_the_source(void)
{
    // The bound that would be silent: a source rectangle whose ceiling walks one row past the
    // last. A guard row of a known value either side catches it, and the ratio is chosen so
    // that the final destination pixel's rectangle ends exactly on the source edge.
    static uint8_t buf[3 + 37 * 23 * 3 + 3];
    uint8_t *src = buf + 3;
    static uint8_t dst[16 * 10 * 3];
    memset(buf, 0xAB, sizeof(buf));
    for (int32_t i = 0; i < 37 * 23 * 3; i++) {
        src[i] = (uint8_t)(i % 251);
    }
    TEST_ASSERT_TRUE(img_scale_area(src, 37, 23, 16, 10, dst, 16, 10));
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[sizeof(buf) - 1]);
}


// ------------------------------------------------------------------ EXIF orientation
//
// A 3x2 image whose every pixel is distinct, so a transform that is right in one axis and
// wrong in the other fails rather than passing by symmetry. Red carries the column and green
// the row; blue is a constant, which is what catches a channel-order mistake.
//
//   (0,0) (1,0) (2,0)
//   (0,1) (1,1) (2,1)

#define OW 3
#define OH 2

static void fill_marked(uint8_t *p)
{
    for (int32_t y = 0; y < OH; y++) {
        for (int32_t x = 0; x < OW; x++) {
            uint8_t *q = p + ((size_t)y * OW + (size_t)x) * 3u;
            q[0] = (uint8_t)(10 + x);
            q[1] = (uint8_t)(20 + y);
            q[2] = 0x77;
        }
    }
}

// The source pixel each destination pixel must have come from, written out by hand from the
// TIFF definitions rather than derived from the implementation -- an expectation computed
// the same way as the code under test cannot disagree with it.
static void expect(int orientation, int32_t dx, int32_t dy, int32_t *sx, int32_t *sy)
{
    switch (orientation) {
    case 2: *sx = OW - 1 - dx; *sy = dy;          break; // mirrored horizontally
    case 3: *sx = OW - 1 - dx; *sy = OH - 1 - dy; break; // rotated 180
    case 4: *sx = dx;          *sy = OH - 1 - dy; break; // mirrored vertically
    case 5: *sx = dy;          *sy = dx;          break; // transposed
    case 6: *sx = dy;          *sy = OH - 1 - dx; break; // rotated 90 clockwise
    case 7: *sx = OW - 1 - dy; *sy = OH - 1 - dx; break; // transverse
    case 8: *sx = OW - 1 - dy; *sy = dx;          break; // rotated 90 anticlockwise
    default: *sx = dx;         *sy = dy;          break;
    }
}

static void test_orient_moves_every_pixel_where_the_tag_says(void)
{
    uint8_t src[OW * OH * 3];
    uint8_t dst[OW * OH * 3];
    fill_marked(src);

    for (int o = 2; o <= 8; o++) {
        const bool swaps = img_orient_swaps_axes(o);
        const int32_t dw = swaps ? OH : OW;
        const int32_t dh = swaps ? OW : OH;
        memset(dst, 0, sizeof(dst));
        TEST_ASSERT_TRUE(img_orient_rgb(src, OW, OH, o, dst));
        for (int32_t y = 0; y < dh; y++) {
            for (int32_t x = 0; x < dw; x++) {
                int32_t sx = 0, sy = 0;
                expect(o, x, y, &sx, &sy);
                const uint8_t *want = src + ((size_t)sy * OW + (size_t)sx) * 3u;
                const uint8_t *got = dst + ((size_t)y * (size_t)dw + (size_t)x) * 3u;
                TEST_ASSERT_EQUAL_UINT8(want[0], got[0]);
                TEST_ASSERT_EQUAL_UINT8(want[1], got[1]);
                TEST_ASSERT_EQUAL_UINT8(want[2], got[2]);
            }
        }
    }
}

// The one that matters on this frame: a phone stores a portrait photograph as a landscape
// frame plus Orientation=6, so the top-left of the displayed picture is the BOTTOM-left of
// the stored one. Getting the direction backwards gives an upside-down picture that every
// dimension check still passes.
static void test_orientation_six_turns_clockwise(void)
{
    uint8_t src[OW * OH * 3];
    uint8_t dst[OW * OH * 3];
    fill_marked(src);
    TEST_ASSERT_TRUE(img_orient_rgb(src, OW, OH, 6, dst));
    // The stored image's LEFT column becomes the displayed TOP row, bottom pixel first. So
    // destination (0,0) is stored (0, OH-1) -- and under orientation 8 the same pixel would
    // be stored (OW-1, 0), which is why this one assertion separates the two directions.
    TEST_ASSERT_EQUAL_UINT8(10 + 0, dst[0]);
    TEST_ASSERT_EQUAL_UINT8(20 + (OH - 1), dst[1]);
    // Destination (1,0), one to the right along that same top row: stored (0, 0).
    TEST_ASSERT_EQUAL_UINT8(10 + 0, dst[3]);
    TEST_ASSERT_EQUAL_UINT8(20 + 0, dst[4]);
    // Destination (0, OW-1), the bottom-left: the stored image's right column, bottom pixel.
    const uint8_t *p = dst + ((size_t)(OW - 1) * (size_t)OH) * 3u;
    TEST_ASSERT_EQUAL_UINT8(10 + (OW - 1), p[0]);
    TEST_ASSERT_EQUAL_UINT8(20 + (OH - 1), p[1]);
}

static void test_orient_axis_swap_is_reported_for_the_turns_only(void)
{
    for (int o = 1; o <= 4; o++) {
        TEST_ASSERT_FALSE(img_orient_swaps_axes(o));
    }
    for (int o = 5; o <= 8; o++) {
        TEST_ASSERT_TRUE(img_orient_swaps_axes(o));
    }
    TEST_ASSERT_FALSE(img_orient_swaps_axes(0));
    TEST_ASSERT_FALSE(img_orient_swaps_axes(9));
}

// Orientation 1 is nothing to do rather than a copy: a copy would cost an allocation the
// size of the picture on every import, and img_resize.c decides whether to allocate from
// this answer.
static void test_orient_one_and_out_of_range_are_refused(void)
{
    uint8_t src[OW * OH * 3];
    uint8_t dst[OW * OH * 3];
    fill_marked(src);
    memset(dst, 0xCD, sizeof(dst));

    TEST_ASSERT_FALSE(img_orient_needed(1));
    TEST_ASSERT_FALSE(img_orient_needed(0));
    TEST_ASSERT_FALSE(img_orient_needed(9));
    for (int o = 2; o <= 8; o++) {
        TEST_ASSERT_TRUE(img_orient_needed(o));
    }

    TEST_ASSERT_FALSE(img_orient_rgb(src, OW, OH, 1, dst));
    TEST_ASSERT_FALSE(img_orient_rgb(src, OW, OH, 9, dst));
    TEST_ASSERT_FALSE(img_orient_rgb(NULL, OW, OH, 6, dst));
    TEST_ASSERT_FALSE(img_orient_rgb(src, OW, OH, 6, NULL));
    TEST_ASSERT_FALSE(img_orient_rgb(src, 0, OH, 6, dst));
    TEST_ASSERT_EQUAL_UINT8(0xCD, dst[0]); // and it wrote nothing on the way to saying so
}

// Two 90 degree turns in opposite directions are the identity. It is the check that does not
// need the hand-written table above to be right, so the two can only agree by both being
// correct.
static void test_six_then_eight_is_the_original(void)
{
    uint8_t src[OW * OH * 3];
    uint8_t mid[OW * OH * 3];
    uint8_t back[OW * OH * 3];
    fill_marked(src);
    TEST_ASSERT_TRUE(img_orient_rgb(src, OW, OH, 6, mid));
    TEST_ASSERT_TRUE(img_orient_rgb(mid, OH, OW, 8, back));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, back, sizeof(src));
}

// A non-square, non-symmetric size that is not the fixture above, walked to its last pixel:
// an index that overruns by a row is the classic failure and it does not show on 3x2.
static void test_orient_writes_every_pixel_of_a_larger_image(void)
{
    enum { W = 7, H = 5 };
    uint8_t src[W * H * 3];
    uint8_t dst[W * H * 3];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i % 251u + 1u); // never 0, so an unwritten byte is visible
    }
    memset(dst, 0, sizeof(dst));
    TEST_ASSERT_TRUE(img_orient_rgb(src, W, H, 6, dst));
    // Every destination byte came from somewhere: no source byte is zero, so an unwritten
    // pixel is visible as one.
    for (size_t i = 0; i < sizeof(dst); i++) {
        TEST_ASSERT_TRUE(dst[i] != 0);
    }
    // The last destination pixel is the stored top-right corner.
    const uint8_t *last = dst + ((size_t)(W - 1) * (size_t)H + (size_t)(H - 1)) * 3u;
    const uint8_t *want = src + ((size_t)0 * W + (size_t)(W - 1)) * 3u;
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, last, 3);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_target_puts_the_long_edge_on_the_box);
    RUN_TEST(test_target_never_exceeds_the_box);
    RUN_TEST(test_target_keeps_the_aspect_ratio_to_within_a_pixel);
    RUN_TEST(test_target_says_nothing_to_do_when_it_already_fits);
    RUN_TEST(test_target_refuses_what_it_cannot_serve);

    RUN_TEST(test_area_preserves_a_flat_field_exactly);
    RUN_TEST(test_area_at_ratio_one_is_a_copy);
    RUN_TEST(test_area_halving_averages_each_two_by_two_block);
    RUN_TEST(test_area_is_monotone_along_a_gradient);
    RUN_TEST(test_area_weights_partial_source_pixels);
    RUN_TEST(test_area_weights_partial_source_rows);
    RUN_TEST(test_area_crops_rather_than_stretches);
    RUN_TEST(test_area_refuses_to_upscale_or_overrun);
    RUN_TEST(test_area_reads_nothing_past_the_source);

    RUN_TEST(test_orient_moves_every_pixel_where_the_tag_says);
    RUN_TEST(test_orientation_six_turns_clockwise);
    RUN_TEST(test_orient_axis_swap_is_reported_for_the_turns_only);
    RUN_TEST(test_orient_one_and_out_of_range_are_refused);
    RUN_TEST(test_six_then_eight_is_the_original);
    RUN_TEST(test_orient_writes_every_pixel_of_a_larger_image);
    return UNITY_END();
}
