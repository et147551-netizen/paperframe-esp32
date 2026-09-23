// Host-side tests for colour quantisation, dithering and image framing.
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// Every photograph the frame displays passes through this code, and the only way to
// judge it on hardware is a 12-to-16 second refresh followed by squinting at a webcam
// photograph. So it is checked here instead, where a wrong answer is a failing assert
// rather than a subtly ugly picture nobody notices.

#include <string.h>

#include <unity.h>

#include "epd_dither.h"

void setUp(void) {}
void tearDown(void) {}

// The six indices this panel renders. 4 is orange (7.3" only) and 7 is unused; both are
// defects if they ever reach the panel.
static bool is_valid_index(uint8_t idx)
{
    return idx == 0x0 || idx == 0x1 || idx == 0x2 || idx == 0x3 || idx == 0x5 ||
           idx == 0x6;
}

// ---------------------------------------------------------------------- palette

static void test_palette_holds_only_renderable_indices(void)
{
    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        TEST_ASSERT_TRUE_MESSAGE(is_valid_index(EPD_PALETTE[i].index),
                                 "palette carries an index this panel cannot render");
    }
}

static void test_palette_has_no_duplicate_indices(void)
{
    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        for (size_t j = i + 1; j < EPD_PALETTE_COUNT; j++) {
            TEST_ASSERT_NOT_EQUAL(EPD_PALETTE[i].index, EPD_PALETTE[j].index);
        }
    }
}

// ---------------------------------------------------------------------- nearest

static void test_nearest_maps_each_palette_colour_to_itself(void)
{
    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const epd_palette_entry_t p = EPD_PALETTE[i];
        TEST_ASSERT_EQUAL_UINT8(p.index, epd_nearest_index(p.r, p.g, p.b));
    }
}

// An image that is already six-colour -- which is what a re-render of something the
// frame previously displayed looks like -- must survive unchanged.
static void test_nearest_is_idempotent_on_palette_colours(void)
{
    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const epd_palette_entry_t p = EPD_PALETTE[i];
        const uint8_t once = epd_nearest_index(p.r, p.g, p.b);
        // Feed the matched entry's RGB back in; it must land on the same index.
        for (size_t j = 0; j < EPD_PALETTE_COUNT; j++) {
            if (EPD_PALETTE[j].index == once) {
                TEST_ASSERT_EQUAL_UINT8(
                    once, epd_nearest_index(EPD_PALETTE[j].r, EPD_PALETTE[j].g,
                                            EPD_PALETTE[j].b));
            }
        }
    }
}

// Sweeps the RGB cube on a coarse grid. 4,913 colours is enough to catch a palette
// entry with a stray index without making the suite slow.
static void test_nearest_never_emits_an_unrenderable_index(void)
{
    for (int32_t r = 0; r <= 255; r += 15) {
        for (int32_t g = 0; g <= 255; g += 15) {
            for (int32_t b = 0; b <= 255; b += 15) {
                TEST_ASSERT_TRUE(is_valid_index(epd_nearest_index(r, g, b)));
            }
        }
    }
}

// Out-of-range inputs reach this function for real: the dithered path adds a signed
// bias before matching, so values below 0 and above 255 are normal, not pathological.
static void test_nearest_handles_out_of_range_input(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x0, epd_nearest_index(-400, -400, -400)); // clamps to black
    TEST_ASSERT_EQUAL_UINT8(0x1, epd_nearest_index(600, 600, 600));    // and to white
    TEST_ASSERT_TRUE(is_valid_index(epd_nearest_index(-1, 300, 128)));
}

// Independent brute force over the palette, written from the definition rather than by
// calling the implementation. Catches an entry transcribed wrong, a loop bound off by
// one, or a comparison that skips the last palette member.
static uint8_t brute_force_nearest(int32_t r, int32_t g, int32_t b)
{
    long best_dist = -1;
    uint8_t best = 0xFF;
    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const long dr = r - (long)EPD_PALETTE[i].r;
        const long dg = g - (long)EPD_PALETTE[i].g;
        const long db = b - (long)EPD_PALETTE[i].b;
        const long d = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || d < best_dist) {
            best_dist = d;
            best = EPD_PALETTE[i].index;
        }
    }
    return best;
}

static void test_nearest_agrees_with_brute_force(void)
{
    for (int32_t r = 0; r <= 255; r += 11) {
        for (int32_t g = 0; g <= 255; g += 11) {
            for (int32_t b = 0; b <= 255; b += 11) {
                TEST_ASSERT_EQUAL_UINT8(brute_force_nearest(r, g, b),
                                        epd_nearest_index(r, g, b));
            }
        }
    }
}

// Mid-grey is nearest to GREEN, not to black or white, and this is not a bug.
//
// Worth pinning as a test because it is counter-intuitive enough to be "fixed" by
// someone later. Squared distances from (127,127,127): green 13,522; blue 21,082;
// yellow 34,881; red 36,354; black 48,387; white 49,152. The palette has no grey axis --
// its two neutrals sit at the extreme corners of the cube, while green and blue sit near
// the middle. A single mid-grey pixel therefore renders green.
//
// This is precisely why epd_pair_index() exists: across a pair of pixels the summed
// error term can reach greys the palette cannot express in one pixel, which is what
// test_pair_averages_towards_unreachable_colours checks.
static void test_nearest_mid_grey_is_green_not_black_or_white(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x6, epd_nearest_index(127, 127, 127));
    TEST_ASSERT_EQUAL_UINT8(0x6, epd_nearest_index(128, 128, 128));
}

// The neutrals do win near the corners, which is what keeps highlights and shadows
// clean.
static void test_nearest_extremes_are_black_and_white(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x0, epd_nearest_index(10, 10, 10));
    TEST_ASSERT_EQUAL_UINT8(0x1, epd_nearest_index(245, 245, 245));
}

// ------------------------------------------------------------------- pair search

static void test_pair_packs_high_nibble_first(void)
{
    // Pure black next to pure white must come back as exactly those two, in order.
    const uint8_t packed = epd_pair_index(0, 0, 0, 255, 255, 255);
    TEST_ASSERT_EQUAL_UINT8(0x0, packed >> 4);
    TEST_ASSERT_EQUAL_UINT8(0x1, packed & 0x0F);
}

static void test_pair_never_emits_an_unrenderable_index(void)
{
    for (int32_t v = -300; v <= 560; v += 37) {
        const uint8_t packed = epd_pair_index(v, 255 - v, v / 2, v / 3, v, 255 - v);
        TEST_ASSERT_TRUE(is_valid_index(packed >> 4));
        TEST_ASSERT_TRUE(is_valid_index(packed & 0x0F));
    }
}

// The reason the pair search exists rather than two independent lookups: two pixels
// together can average to a colour neither can reach alone. Mid-grey is the clearest
// case -- there is no grey in the palette, so a good pair is black beside white.
static void test_pair_averages_towards_unreachable_colours(void)
{
    const uint8_t packed = epd_pair_index(128, 128, 128, 128, 128, 128);
    const uint8_t hi = packed >> 4;
    const uint8_t lo = packed & 0x0F;
    TEST_ASSERT_TRUE(is_valid_index(hi));
    TEST_ASSERT_TRUE(is_valid_index(lo));
    TEST_ASSERT_TRUE_MESSAGE(hi != lo,
                             "mid-grey should split into two different colours; a pair "
                             "search that returns twice the same entry is behaving like "
                             "a plain nearest lookup");
}

// ----------------------------------------------------------------- row: no dither

static void fill_solid_row(uint8_t *row, size_t width, uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t x = 0; x < width; x++) {
        row[x * 3 + 0] = r;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = b;
    }
}

static void test_row_none_packs_two_pixels_per_byte(void)
{
    uint8_t row[4 * 3];
    uint8_t out[2] = {0xAA, 0xAA};

    // black, white, red, green
    const uint8_t rgb[4][3] = {{0, 0, 0}, {255, 255, 255}, {191, 0, 0}, {67, 138, 28}};
    for (size_t i = 0; i < 4; i++) {
        memcpy(&row[i * 3], rgb[i], 3);
    }

    epd_dither_row_none(row, out, 4);
    TEST_ASSERT_EQUAL_UINT8(0x01, out[0]); // black | white
    TEST_ASSERT_EQUAL_UINT8(0x36, out[1]); // red   | green
}

static void test_row_none_odd_width_pads_with_white(void)
{
    uint8_t row[3 * 3];
    uint8_t out[2] = {0xAA, 0xAA};
    fill_solid_row(row, 3, 0, 0, 0);

    epd_dither_row_none(row, out, 3);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);        // black | black
    TEST_ASSERT_EQUAL_UINT8(0x01, out[1] & 0x0F); // low nibble padded white
    TEST_ASSERT_EQUAL_UINT8(0x0, out[1] >> 4);    // high nibble is the real pixel
}

static void test_row_none_leaves_a_solid_row_flat(void)
{
    uint8_t row[400 * 3];
    uint8_t out[200];
    fill_solid_row(row, 400, 128, 128, 128);

    epd_dither_row_none(row, out, 400);
    for (size_t i = 1; i < 200; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(out[0], out[i],
                                        "the undithered path must not add texture");
    }
}

static void test_row_none_rejects_null(void)
{
    uint8_t out[4] = {0x5A, 0x5A, 0x5A, 0x5A};
    epd_dither_row_none(NULL, out, 8);
    TEST_ASSERT_EQUAL_UINT8(0x5A, out[0]); // untouched
}

// ----------------------------------------------------- tone as a separate pass
//
// **The equivalence the diffusion path rests on.** epd_diffuse_rect() leaves the canvas holding
// exact palette colours, so the pack after it must run at EPD_TONE_NONE -- which means any integer
// compression the plan still owes has to be applied to the pixels beforehand instead of folded
// into the pack. That is only sound if the two are the same arithmetic, and this is where that is
// checked rather than assumed. It is not obviously true: apply_tone() inside the row path works on
// int32 and matches on int32, while a separate pass has to write bytes back first.
static void test_tone_rect_then_no_tone_equals_tone_at_the_pack(void)
{
    // A gradient with all three channels differing, so a per-channel mistake cannot hide.
    uint8_t src[64 * 3];
    for (size_t x = 0; x < 64; x++) {
        src[x * 3 + 0] = (uint8_t)(x * 4);
        src[x * 3 + 1] = (uint8_t)(255 - x * 4);
        src[x * 3 + 2] = (uint8_t)(x * 2 + 40);
    }

    const epd_render_t full = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_FULL};
    const epd_render_t none = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_NONE};

    uint8_t folded[32];
    epd_dither_row_none_cfg(src, folded, 64, &full);

    uint8_t separated[32];
    uint8_t staged[64 * 3];
    memcpy(staged, src, sizeof(staged));
    epd_dither_tone_rect(staged, 64, 1, 3, 64 * 3, &full);
    epd_dither_row_none_cfg(staged, separated, 64, &none);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(folded, separated, 32);

    // And EPD_TONE_NONE really is a no-op rather than an identity that rounds.
    memcpy(staged, src, sizeof(staged));
    epd_dither_tone_rect(staged, 64, 1, 3, 64 * 3, &none);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, staged, sizeof(staged));
}

// -------------------------------------------------------------- row: dithered

static void test_row_quality_adds_texture_where_none_does_not(void)
{
    uint8_t row[400 * 3];
    uint8_t flat[200];
    uint8_t dithered[200];
    fill_solid_row(row, 400, 128, 128, 128);

    epd_dither_row_none(row, flat, 400);
    epd_dither_row_quality(row, dithered, 400, 0, EPD_DITHER_STRENGTH_QUALITY);

    bool varies = false;
    for (size_t i = 1; i < 200; i++) {
        if (dithered[i] != dithered[0]) {
            varies = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(varies, "a solid mid-grey row must dither to a mix");
    TEST_ASSERT_TRUE(memcmp(flat, dithered, 200) != 0);
}

// The bias pattern advances with the row index. If it did not, every row would carry an
// identical pattern and the image would show vertical banding.
static void test_row_quality_pattern_changes_between_rows(void)
{
    uint8_t row[400 * 3];
    uint8_t y0[200];
    uint8_t y1[200];
    fill_solid_row(row, 400, 128, 128, 128);

    epd_dither_row_quality(row, y0, 400, 0, EPD_DITHER_STRENGTH_QUALITY);
    epd_dither_row_quality(row, y1, 400, 1, EPD_DITHER_STRENGTH_QUALITY);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(y0, y1, 200) != 0,
                             "consecutive rows share a bias pattern -- expect banding");
}

static void test_row_quality_is_deterministic(void)
{
    uint8_t row[400 * 3];
    uint8_t a[200];
    uint8_t b[200];
    fill_solid_row(row, 400, 90, 140, 200);

    epd_dither_row_quality(row, a, 400, 17, EPD_DITHER_STRENGTH_QUALITY);
    epd_dither_row_quality(row, b, 400, 17, EPD_DITHER_STRENGTH_QUALITY);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, 200);
}

// Saturated inputs at both ends of a row and at extreme row indices: the bias arithmetic
// runs on signed 32-bit values and must not wrap.
static void test_row_quality_never_emits_an_unrenderable_index(void)
{
    uint8_t row[400 * 3];
    uint8_t out[200];

    const uint8_t extremes[][3] = {
        {0, 0, 0}, {255, 255, 255}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 128, 128},
    };

    for (size_t e = 0; e < sizeof(extremes) / sizeof(extremes[0]); e++) {
        fill_solid_row(row, 400, extremes[e][0], extremes[e][1], extremes[e][2]);
        const size_t rows[] = {0, 1, 599, 4096};
        for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
            epd_dither_row_quality(row, out, 400, rows[k], EPD_DITHER_STRENGTH_QUALITY);
            for (size_t i = 0; i < 200; i++) {
                TEST_ASSERT_TRUE(is_valid_index(out[i] >> 4));
                TEST_ASSERT_TRUE(is_valid_index(out[i] & 0x0F));
            }
        }
    }
}

// A gradient is the case the dither exists for, and the one most likely to expose an
// edge bug because it visits every input value.
static void test_row_quality_handles_a_full_gradient(void)
{
    uint8_t row[256 * 3];
    uint8_t out[128];
    for (size_t x = 0; x < 256; x++) {
        row[x * 3 + 0] = (uint8_t)x;
        row[x * 3 + 1] = (uint8_t)(255 - x);
        row[x * 3 + 2] = (uint8_t)((x * 3) & 0xFF);
    }
    epd_dither_row_quality(row, out, 256, 42, EPD_DITHER_STRENGTH_QUALITY);
    for (size_t i = 0; i < 128; i++) {
        TEST_ASSERT_TRUE(is_valid_index(out[i] >> 4));
        TEST_ASSERT_TRUE(is_valid_index(out[i] & 0x0F));
    }
}

static void test_row_quality_odd_width_writes_the_final_pixel(void)
{
    uint8_t row[5 * 3];
    uint8_t out[3] = {0xAA, 0xAA, 0xAA};
    fill_solid_row(row, 5, 200, 30, 30);

    epd_dither_row_quality(row, out, 5, 0, EPD_DITHER_STRENGTH_QUALITY);
    // ceil(5/2) = 3 bytes must all have been written.
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(is_valid_index(out[i] >> 4));
        TEST_ASSERT_TRUE(is_valid_index(out[i] & 0x0F));
    }
}

static void test_row_quality_rejects_null(void)
{
    uint8_t out[4] = {0x5A, 0x5A, 0x5A, 0x5A};
    epd_dither_row_quality(NULL, out, 8, 0, EPD_DITHER_STRENGTH_QUALITY);
    TEST_ASSERT_EQUAL_UINT8(0x5A, out[0]);
}

// -------------------------------------------------------------- fit and centre

static void test_fit_landscape_into_portrait_screen(void)
{
    // 800x400 into the panel's native 400x600: width binds, letterboxed top and bottom.
    const epd_fit_t f = epd_fit_centre(800, 400, 400, 600);
    TEST_ASSERT_EQUAL_INT32(400, f.width);
    TEST_ASSERT_EQUAL_INT32(200, f.height);
    TEST_ASSERT_EQUAL_INT32(0, f.x);
    TEST_ASSERT_EQUAL_INT32(200, f.y);
}

static void test_fit_portrait_into_portrait_screen(void)
{
    // 300x900 into 400x600: height binds, pillarboxed left and right.
    const epd_fit_t f = epd_fit_centre(300, 900, 400, 600);
    TEST_ASSERT_EQUAL_INT32(200, f.width);
    TEST_ASSERT_EQUAL_INT32(600, f.height);
    TEST_ASSERT_EQUAL_INT32(100, f.x);
    TEST_ASSERT_EQUAL_INT32(0, f.y);
}

static void test_fit_exact_match_is_untouched(void)
{
    const epd_fit_t f = epd_fit_centre(400, 600, 400, 600);
    TEST_ASSERT_EQUAL_INT32(400, f.width);
    TEST_ASSERT_EQUAL_INT32(600, f.height);
    TEST_ASSERT_EQUAL_INT32(0, f.x);
    TEST_ASSERT_EQUAL_INT32(0, f.y);
}

// The shipping firmware scales up as well as down -- min() of the two ratios, not
// min(1, ...) -- so a small image fills the screen rather than sitting in the middle.
static void test_fit_scales_small_images_up(void)
{
    const epd_fit_t f = epd_fit_centre(100, 150, 400, 600);
    TEST_ASSERT_EQUAL_INT32(400, f.width);
    TEST_ASSERT_EQUAL_INT32(600, f.height);
}

static void test_fit_never_exceeds_the_screen(void)
{
    const int32_t sizes[][2] = {
        {1, 1}, {4000, 3}, {3, 4000}, {401, 601}, {399, 599}, {1920, 1080},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const epd_fit_t f = epd_fit_centre(sizes[i][0], sizes[i][1], 400, 600);
        TEST_ASSERT_TRUE(f.width <= 400);
        TEST_ASSERT_TRUE(f.height <= 600);
        TEST_ASSERT_TRUE(f.x >= 0);
        TEST_ASSERT_TRUE(f.y >= 0);
    }
}

// A corrupt image header must not become a negative-width blit.
static void test_fit_rejects_bad_dimensions(void)
{
    const epd_fit_t a = epd_fit_centre(0, 600, 400, 600);
    TEST_ASSERT_EQUAL_INT32(0, a.width);
    TEST_ASSERT_EQUAL_INT32(0, a.height);

    const epd_fit_t b = epd_fit_centre(-100, 600, 400, 600);
    TEST_ASSERT_EQUAL_INT32(0, b.width);

    const epd_fit_t c = epd_fit_centre(400, 600, 0, 0);
    TEST_ASSERT_EQUAL_INT32(0, c.width);
}

// ------------------------------------------------ the reduction that keeps the fit
//
// epd_fit_reduction() is what the SMB import path stores a photograph at. The property
// that matters is one-sided -- the stored image may be larger than it is drawn and must
// never be smaller -- so the guarantee is asserted over a table rather than only at the
// two sizes anybody has looked at.

// The bench share, and the case that says this is not the long-edge rule. 768x1344 is
// drawn at 342x600, so factor 2 (384x672) is the largest that still covers it; the
// long-edge rule against a 600 target would say ceil(1344/600) = 3, store 256x448, and
// make the panel upscale by a third.
static void test_reduction_is_the_binding_ratio_not_the_long_edge(void)
{
    TEST_ASSERT_EQUAL_INT32(2, epd_fit_reduction(768, 1344, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(2, epd_fit_reduction(768, 1344, 400, 600, 1));
}

// A 12 MP phone photograph. The unaligned answer is 10 (403x302); cropping 302 to a
// multiple of 16 gives 288, which is under the 300 it is drawn at, so the factor steps
// down to 9. This is the case the alignment loop exists for.
static void test_reduction_steps_down_for_the_alignment_crop(void)
{
    TEST_ASSERT_EQUAL_INT32(10, epd_fit_reduction(4032, 3024, 400, 600, 1));
    TEST_ASSERT_EQUAL_INT32(9, epd_fit_reduction(4032, 3024, 400, 600, 16));
}

// At or below the panel there is nothing to reduce, and a small image is UPSCALED by
// epd_fit_centre() -- so the reduction must be 1 rather than 0 or negative.
static void test_reduction_never_goes_below_one(void)
{
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(400, 600, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(343, 600, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(100, 150, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(1, 1, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(0, 600, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(-100, 600, 400, 600, 16));
    TEST_ASSERT_EQUAL_INT32(1, epd_fit_reduction(400, 600, 0, 0, 16));
}

// The guarantee itself, over both sides of 2:3 and both orientations of the screen.
static void test_reduction_never_stores_less_than_is_drawn(void)
{
    const int32_t sizes[][2] = {
        {768, 1344}, {4032, 3024}, {3024, 4032}, {1920, 1080}, {800, 400},
        {1200, 600}, {401, 601}, {399, 599}, {4000, 3}, {3, 4000}, {2000, 3000},
    };
    const int32_t screens[][2] = {{400, 600}, {600, 400}};
    for (size_t s = 0; s < sizeof(screens) / sizeof(screens[0]); s++) {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            const int32_t w = sizes[i][0], h = sizes[i][1];
            const int32_t sw = screens[s][0], sh = screens[s][1];
            const int32_t f = epd_fit_reduction(w, h, sw, sh, 16);
            TEST_ASSERT_TRUE(f >= 1);
            const epd_fit_t fit = epd_fit_centre(w, h, sw, sh);
            if (f == 1) {
                continue; // nothing was reduced, so there is nothing to guarantee
            }
            TEST_ASSERT_TRUE((w / f) / 16 * 16 >= fit.width);
            TEST_ASSERT_TRUE((h / f) / 16 * 16 >= fit.height);
        }
    }
}


// ------------------------------------------------------------- the auto-rotation rule

static void test_rotate_turns_a_disagreeing_picture(void)
{
    // 4:3 either way round. This is the case Android's 1.5 threshold would refuse, and it
    // is what a phone shoots, so it is the one a threshold under 1.333 exists for.
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(4032, 3024, 400, 600));
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(3024, 4032, 600, 400));
    // 3:2 and 16:9, the other two shapes a camera produces.
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(3000, 2000, 400, 600));
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(1920, 1080, 400, 600));
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(1080, 1920, 600, 400));
    // 5:4, exactly 1.25. Protected until 2026-09-19 and turned since: it filled 53 % of the
    // glass where a turn gives 83 %, which is what moved the threshold from 130 to 125.
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(1280, 1024, 400, 600));
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(1024, 1280, 600, 400));
}

static void test_rotate_leaves_an_agreeing_picture_alone(void)
{
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(3024, 4032, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(4032, 3024, 600, 400));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(768, 1344, 400, 600));
}

static void test_rotate_protects_a_near_square_composition(void)
{
    // The whole point of having a threshold rather than "orientations disagree". 6:5 is 1.2
    // and is what now pins the value at 1.25 rather than at 1.0 -- the case where a turn
    // still gains area (73 % against 61 %) and the rule declines to take it. A 1024x1024
    // source disagrees with nothing but is checked at both screens anyway.
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(1200, 1000, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(1000, 1200, 600, 400));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(1024, 1024, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(1024, 1024, 600, 400));
}

static void test_rotate_is_exact_at_the_threshold(void)
{
    // 1.25 rotates and anything under it does not, so the comparison is >= and the
    // arithmetic is integer. A test that only used 4:3 and 1:1 could not see this.
    TEST_ASSERT_TRUE(epd_fit_wants_rotate(1250, 1000, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(1249, 1000, 400, 600));
}

static void test_rotate_rejects_bad_dimensions(void)
{
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(0, 600, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(400, 0, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(-4032, 3024, 400, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(4032, 3024, 0, 600));
    TEST_ASSERT_FALSE(epd_fit_wants_rotate(4032, 3024, 400, -600));
}

static void test_rotate_always_increases_the_drawn_area(void)
{
    // Not a threshold test: the claim is that turning a picture the rule selects can never
    // make it smaller on the glass, which is what makes the value a matter of taste rather
    // than an optimum to find. Checked against epd_fit_centre() rather than asserted.
    const int32_t sizes[][2] = {
        {4032, 3024}, {3000, 2000}, {1920, 1080}, {1250, 1000}, {1280, 1024},
        {3024, 4032}, {1080, 1920}, {1024, 1280},
    };
    const int32_t screens[][2] = {{400, 600}, {600, 400}};
    for (size_t s = 0; s < sizeof(screens) / sizeof(screens[0]); s++) {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            const int32_t w = sizes[i][0], h = sizes[i][1];
            const int32_t sw = screens[s][0], sh = screens[s][1];
            if (!epd_fit_wants_rotate(w, h, sw, sh)) {
                continue;
            }
            const epd_fit_t as_is = epd_fit_centre(w, h, sw, sh);
            const epd_fit_t turned = epd_fit_centre(w, h, sh, sw);
            TEST_ASSERT_TRUE((int64_t)turned.width * turned.height >
                             (int64_t)as_is.width * as_is.height);
        }
    }
}


// ------------------------------------------------- measured palette and tone compression

// Ticket 19. The stock path must not move: every image rendered before this existed was
// rendered by it, and a silent change would show up as "the frame looks different now"
// with nothing to point at.
static void test_stock_cfg_is_identical_to_the_legacy_path(void)
{
    uint8_t src[64 * 3];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)((i * 37) & 0xFF);
    }
    uint8_t legacy[32], cfg[32];

    epd_dither_row_none(src, legacy, 64);
    epd_dither_row_none_cfg(src, cfg, 64, &EPD_RENDER_STOCK);
    TEST_ASSERT_EQUAL_MEMORY(legacy, cfg, sizeof(legacy));

    for (size_t y = 0; y < 4; y++) {
        epd_dither_row_quality(src, legacy, 64, y, EPD_DITHER_STRENGTH_QUALITY);
        epd_dither_row_quality_cfg(src, cfg, 64, y, EPD_DITHER_STRENGTH_QUALITY,
                                   &EPD_RENDER_STOCK);
        TEST_ASSERT_EQUAL_MEMORY(legacy, cfg, sizeof(legacy));
    }
}

// Entry 0 must be black and entry 1 white: tone compression reads the reachable range
// off those two positions, so a reordered table would compress into the wrong range
// while still looking like a valid palette.
static void test_measured_palette_layout_and_validity(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x0, EPD_PALETTE_MEASURED[0].index);
    TEST_ASSERT_EQUAL_HEX8(0x1, EPD_PALETTE_MEASURED[1].index);
    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        TEST_ASSERT_TRUE(is_valid_index(EPD_PALETTE_MEASURED[i].index));
        // Every index the device palette can emit, the measured one can too -- they
        // describe the same six physical colours.
        TEST_ASSERT_EQUAL_HEX8(EPD_PALETTE[i].index, EPD_PALETTE_MEASURED[i].index);
    }
}

// The panel's white is a mid grey and its black is not black. The measured table has to
// say so, or there is nothing to compress into.
static void test_measured_white_is_a_mid_grey_and_black_is_not_black(void)
{
    TEST_ASSERT_LESS_THAN_UINT8(200, EPD_PALETTE_MEASURED[1].r);
    TEST_ASSERT_GREATER_THAN_UINT8(100, EPD_PALETTE_MEASURED[1].r);
    TEST_ASSERT_GREATER_THAN_UINT8(10, EPD_PALETTE_MEASURED[0].b);
}

// Compression is what stops the ends of the range being thrown away: input black lands
// on the panel's black, input white on its white.
static void test_tone_compression_maps_the_ends_onto_the_panel(void)
{
    const uint8_t black[6] = {0, 0, 0, 0, 0, 0};
    const uint8_t white[6] = {255, 255, 255, 255, 255, 255};
    uint8_t out[1];

    epd_dither_row_none_cfg(black, out, 2, &EPD_RENDER_MEASURED);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    epd_dither_row_none_cfg(white, out, 2, &EPD_RENDER_MEASURED);
    TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
}

// A quarter-tone input is the case the old path could not express: 64 is far below the
// panel's white, and matching it against a palette that thinks white is 255 puts it in a
// different place than matching against one that knows white is 154.
static void test_measured_render_differs_from_stock_on_a_real_gradient(void)
{
    uint8_t src[64 * 3];
    for (size_t x = 0; x < 64; x++) {
        const uint8_t v = (uint8_t)(x * 4);
        src[x * 3] = v;
        src[x * 3 + 1] = v;
        src[x * 3 + 2] = v;
    }
    uint8_t stock[32], measured[32];
    epd_dither_row_quality_cfg(src, stock, 64, 0, EPD_DITHER_STRENGTH_QUALITY,
                               &EPD_RENDER_STOCK);
    epd_dither_row_quality_cfg(src, measured, 64, 0, EPD_DITHER_STRENGTH_QUALITY,
                               &EPD_RENDER_MEASURED);
    TEST_ASSERT_FALSE(memcmp(stock, measured, sizeof(stock)) == 0);
}

static void test_measured_render_never_emits_an_unrenderable_index(void)
{
    uint8_t src[64 * 3];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)((i * 101) & 0xFF);
    }
    uint8_t out[32];
    for (size_t y = 0; y < 8; y++) {
        epd_dither_row_quality_cfg(src, out, 64, y, EPD_DITHER_STRENGTH_QUALITY,
                                   &EPD_RENDER_MEASURED);
        for (size_t i = 0; i < sizeof(out); i++) {
            TEST_ASSERT_TRUE(is_valid_index((uint8_t)(out[i] >> 4)));
            TEST_ASSERT_TRUE(is_valid_index((uint8_t)(out[i] & 0x0F)));
        }
    }
    epd_dither_row_none_cfg(src, out, 64, &EPD_RENDER_MEASURED);
    for (size_t i = 0; i < sizeof(out); i++) {
        TEST_ASSERT_TRUE(is_valid_index((uint8_t)(out[i] >> 4)));
        TEST_ASSERT_TRUE(is_valid_index((uint8_t)(out[i] & 0x0F)));
    }
}

// A config with no palette must be refused, not followed into a null dereference.
static void test_cfg_rejects_null(void)
{
    uint8_t src[6] = {0}, out[3] = {0xAA, 0xAA, 0xAA};
    const epd_render_t empty = {NULL, EPD_TONE_FULL};

    epd_dither_row_none_cfg(src, out, 2, NULL);
    epd_dither_row_none_cfg(src, out, 2, &empty);
    epd_dither_row_quality_cfg(src, out, 2, 0, EPD_DITHER_STRENGTH_QUALITY, NULL);
    epd_dither_row_quality_cfg(src, out, 2, 0, EPD_DITHER_STRENGTH_QUALITY, &empty);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);
}


// Golden bytes rather than "these two spellings agree". Comparing the wrapper against
// the cfg call proves they route to the same place and nothing about what that place
// does; if the stock palette were edited, both sides would move together and the test
// would still pass. These pin the output itself.
//
// Generated from src/core/epd_dither.c at the commit that chose the default, over
// src[i] = (i * 37) & 0xFF, row y = 3.
static const uint8_t golden_stock_none[32] = {
    0x05, 0x35, 0x26, 0x10, 0x53, 0x52, 0x61, 0x05,
    0x35, 0x26, 0x16, 0x53, 0x52, 0x61, 0x65, 0x05,
    0x35, 0x26, 0x10, 0x53, 0x52, 0x61, 0x05, 0x35,
    0x26, 0x16, 0x53, 0x52, 0x52, 0x61, 0x05, 0x35,
};
static const uint8_t golden_stock_quality[32] = {
    0x65, 0x35, 0x66, 0x50, 0x53, 0x52, 0x61, 0x65,
    0x25, 0x25, 0x56, 0x13, 0x52, 0x61, 0x65, 0x65,
    0x35, 0x20, 0x10, 0x53, 0x62, 0x51, 0x05, 0x35,
    0x26, 0x56, 0x13, 0x62, 0x62, 0x61, 0x05, 0x36,
};
// EPD_RENDER_MANUAL -- ticket 19's choice, and the default until 2026-09-05. These bytes are
// unchanged from when they pinned the default: they were re-derived from EPD_RENDER_MANUAL
// when it stopped being the default and came out identical, so nothing here was regenerated
// to make a test pass.
static const uint8_t golden_manual_none[32] = {
    0x05, 0x35, 0x26, 0x10, 0x53, 0x52, 0x61, 0x65,
    0x35, 0x25, 0x16, 0x13, 0x52, 0x51, 0x61, 0x05,
    0x35, 0x26, 0x10, 0x53, 0x52, 0x61, 0x61, 0x35,
    0x25, 0x16, 0x13, 0x52, 0x52, 0x61, 0x05, 0x35,
};
static const uint8_t golden_manual_quality[32] = {
    0x65, 0x35, 0x66, 0x10, 0x53, 0x16, 0x51, 0x01,
    0x35, 0x25, 0x10, 0x13, 0x61, 0x61, 0x55, 0x01,
    0x35, 0x25, 0x10, 0x53, 0x52, 0x51, 0x65, 0x35,
    0x25, 0x10, 0x13, 0x61, 0x52, 0x61, 0x05, 0x35,
};

// EPD_RENDER_DEFAULT as of 2026-09-05: epdoptimize's aitjcize calibration, fully compressed.
// Generated from src/core/epd_dither.c over the same input, at the commit that adopted it.
static const uint8_t golden_default_none[32] = {
    0x05, 0x36, 0x26, 0x16, 0x53, 0x62, 0x61, 0x65,
    0x36, 0x26, 0x16, 0x13, 0x52, 0x61, 0x61, 0x05,
    0x36, 0x26, 0x16, 0x53, 0x62, 0x61, 0x65, 0x36,
    0x26, 0x16, 0x13, 0x52, 0x62, 0x61, 0x05, 0x36,
};
static const uint8_t golden_default_quality[32] = {
    0x66, 0x35, 0x66, 0x10, 0x53, 0x52, 0x61, 0x01,
    0x35, 0x25, 0x56, 0x13, 0x52, 0x61, 0x55, 0x01,
    0x35, 0x25, 0x10, 0x53, 0x62, 0x51, 0x66, 0x35,
    0x26, 0x65, 0x13, 0x52, 0x52, 0x61, 0x65, 0x36,
};

static void fill_ramp(uint8_t *src, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++) {
        src[i] = (uint8_t)((i * 37) & 0xFF);
    }
}

static void test_stock_output_matches_its_golden(void)
{
    uint8_t src[64 * 3], out[32];
    fill_ramp(src, sizeof(src));

    epd_dither_row_none_cfg(src, out, 64, &EPD_RENDER_STOCK);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_stock_none, out, sizeof(out));

    epd_dither_row_quality_cfg(src, out, 64, 3, EPD_DITHER_STRENGTH_QUALITY,
                               &EPD_RENDER_STOCK);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_stock_quality, out, sizeof(out));
}

// The default was chosen by putting candidates on the glass. Changing it is a decision, so it
// should fail here and be re-decided, not drift. It has been re-decided once, on 2026-09-05.
static void test_default_output_matches_its_golden(void)
{
    uint8_t src[64 * 3], out[32];
    fill_ramp(src, sizeof(src));

    epd_dither_row_none_cfg(src, out, 64, &EPD_RENDER_DEFAULT);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_default_none, out, sizeof(out));

    epd_dither_row_quality_cfg(src, out, 64, 3, EPD_DITHER_STRENGTH_QUALITY,
                               &EPD_RENDER_DEFAULT);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_default_quality, out, sizeof(out));
}

// Ticket 19's choice, pinned after it stopped being the default. It is still selectable, and
// losing this would throw away the record of what the new default was chosen against.
static void test_manual_output_matches_its_golden(void)
{
    uint8_t src[64 * 3], out[32];
    fill_ramp(src, sizeof(src));

    epd_dither_row_none_cfg(src, out, 64, &EPD_RENDER_MANUAL);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_manual_none, out, sizeof(out));

    epd_dither_row_quality_cfg(src, out, 64, 3, EPD_DITHER_STRENGTH_QUALITY,
                               &EPD_RENDER_MANUAL);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_manual_quality, out, sizeof(out));
}

// The default is epdoptimize's aitjcize calibration with full compression, and that is a
// claim worth asserting rather than reading. It must also be what the registry returns for
// its first entry, since that is what the Web UI offers first and what a factory-fresh
// device stores.
static void test_default_is_the_epdopt_palette_fully_compressed(void)
{
    TEST_ASSERT_EQUAL_PTR(EPD_PALETTE_EPDOPT_AITJCIZE, EPD_RENDER_DEFAULT.palette);
    TEST_ASSERT_EQUAL_UINT8(EPD_TONE_FULL, EPD_RENDER_DEFAULT.tone);
    TEST_ASSERT_EQUAL_PTR(EPD_RENDER_EPDOPT.palette, EPD_RENDER_DEFAULT.palette);

    const epd_render_t first = epd_render_for_palette(EPD_PALETTE_ID_AITJCIZE);
    TEST_ASSERT_EQUAL_PTR(EPD_RENDER_DEFAULT.palette, first.palette);
    TEST_ASSERT_EQUAL_UINT8(EPD_RENDER_DEFAULT.tone, first.tone);
}

// ------------------------------------------------------------- the palette registry
//
// The registry is what the setting, the HTTP API and the Web UI all read, so a hole in it is
// a palette that can be stored and then not rendered.

static void test_every_palette_id_is_complete(void)
{
    for (int i = 0; i < EPD_PALETTE_ID_COUNT; i++) {
        const epd_palette_id_t id = (epd_palette_id_t)i;
        const char *name = epd_palette_id_name(id);
        const char *label = epd_palette_id_label(id);
        TEST_ASSERT_NOT_NULL_MESSAGE(name, "palette id has no wire name");
        TEST_ASSERT_NOT_NULL_MESSAGE(label, "palette id has no label");
        TEST_ASSERT_TRUE_MESSAGE(name[0] != 0, "palette wire name is empty");
        TEST_ASSERT_TRUE_MESSAGE(label[0] != 0, "palette label is empty");
        TEST_ASSERT_NOT_NULL_MESSAGE(epd_render_for_palette(id).palette,
                                     "palette id has no render config");
    }
}

static void test_palette_names_round_trip_and_are_unique(void)
{
    for (int i = 0; i < EPD_PALETTE_ID_COUNT; i++) {
        epd_palette_id_t back;
        TEST_ASSERT_TRUE(
            epd_palette_id_from_name(epd_palette_id_name((epd_palette_id_t)i), &back));
        TEST_ASSERT_EQUAL_INT(i, (int)back);

        for (int j = i + 1; j < EPD_PALETTE_ID_COUNT; j++) {
            TEST_ASSERT_TRUE_MESSAGE(
                strcmp(epd_palette_id_name((epd_palette_id_t)i),
                       epd_palette_id_name((epd_palette_id_t)j)) != 0,
                "two palette ids share a wire name");
        }
    }
}

// An unknown name must be refused and must leave the caller's variable alone -- that is what
// lets the HTTP layer answer 400 instead of silently rendering the default.
static void test_unknown_palette_name_is_refused(void)
{
    epd_palette_id_t id = EPD_PALETTE_ID_STOCK;
    TEST_ASSERT_FALSE(epd_palette_id_from_name("no-such-palette", &id));
    TEST_ASSERT_EQUAL_INT(EPD_PALETTE_ID_STOCK, (int)id);
    TEST_ASSERT_FALSE(epd_palette_id_from_name("", &id));
    TEST_ASSERT_FALSE(epd_palette_id_from_name(NULL, &id));
    TEST_ASSERT_FALSE(epd_palette_id_from_name("aitjcize", NULL));
    TEST_ASSERT_EQUAL_INT(EPD_PALETTE_ID_STOCK, (int)id);
}

// A corrupt NVS entry, or a downgrade from a build that had more palettes, must render
// rather than index off the end of the table.
static void test_out_of_range_palette_falls_back_to_the_default(void)
{
    const epd_render_t high = epd_render_for_palette((epd_palette_id_t)EPD_PALETTE_ID_COUNT);
    TEST_ASSERT_EQUAL_PTR(EPD_RENDER_DEFAULT.palette, high.palette);
    TEST_ASSERT_EQUAL_UINT8(EPD_RENDER_DEFAULT.tone, high.tone);

    const epd_render_t low = epd_render_for_palette((epd_palette_id_t)-1);
    TEST_ASSERT_EQUAL_PTR(EPD_RENDER_DEFAULT.palette, low.palette);
    TEST_ASSERT_NULL(epd_palette_id_name((epd_palette_id_t)EPD_PALETTE_ID_COUNT));
    TEST_ASSERT_NULL(epd_palette_id_label((epd_palette_id_t)-1));
}

// STOCK is the one entry with no tone compression, and that is the point of offering it: if
// it ever gains compression it stops being a distinct choice.
static void test_stock_palette_entry_has_no_compression(void)
{
    const epd_render_t stock = epd_render_for_palette(EPD_PALETTE_ID_STOCK);
    TEST_ASSERT_EQUAL_PTR(EPD_PALETTE, stock.palette);
    TEST_ASSERT_EQUAL_UINT8(EPD_TONE_NONE, stock.tone);

    for (int i = 0; i < EPD_PALETTE_ID_COUNT; i++) {
        if (i == EPD_PALETTE_ID_STOCK) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            EPD_TONE_FULL, epd_render_for_palette((epd_palette_id_t)i).tone,
            "a selectable palette other than stock is not fully compressed");
    }
}

// The tone compression reads the reachable range off palette entries 0 and 1, so a palette
// whose black is not darkest or whose white is not lightest would compress into the wrong
// range while looking perfectly well-formed. Checked for every palette a render config can
// name, because this is the one structural requirement epd_dither.h states and nothing else
// enforces.
static void test_every_render_palette_has_black_first_and_white_second(void)
{
    const epd_palette_entry_t *tables[] = {
        EPD_PALETTE,                  EPD_PALETTE_MEASURED,
        EPD_PALETTE_MANUAL,           EPD_PALETTE_EPDOPT_AITJCIZE,
        EPD_PALETTE_EPDOPT_SPECTRA6,  EPD_PALETTE_EPDOPT_LEGACY,
        EPD_PALETTE_EPDOPT_BOEBER,    EPD_PALETTE_EPDOPT_ORIGINAL,
    };

    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        const epd_palette_entry_t *p = tables[t];
        const int32_t black = p[0].r + p[0].g + p[0].b;
        const int32_t white = p[1].r + p[1].g + p[1].b;
        for (size_t i = 2; i < EPD_PALETTE_COUNT; i++) {
            const int32_t sum = p[i].r + p[i].g + p[i].b;
            TEST_ASSERT_TRUE_MESSAGE(sum >= black, "entry 0 is not the darkest colour");
            TEST_ASSERT_TRUE_MESSAGE(sum <= white, "entry 1 is not the lightest colour");
        }
    }
}

// The manual's white and black are where the measured table's anchors came from. If the
// two ever disagree, one of them was edited without the other and the compression range
// no longer matches the palette it compresses into.
static void test_manual_and_measured_share_their_anchors(void)
{
    for (size_t i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_UINT8(EPD_PALETTE_MANUAL[i].r, EPD_PALETTE_MEASURED[i].r);
        TEST_ASSERT_EQUAL_UINT8(EPD_PALETTE_MANUAL[i].g, EPD_PALETTE_MEASURED[i].g);
        TEST_ASSERT_EQUAL_UINT8(EPD_PALETTE_MANUAL[i].b, EPD_PALETTE_MEASURED[i].b);
    }
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_palette_holds_only_renderable_indices);
    RUN_TEST(test_palette_has_no_duplicate_indices);

    RUN_TEST(test_nearest_maps_each_palette_colour_to_itself);
    RUN_TEST(test_nearest_is_idempotent_on_palette_colours);
    RUN_TEST(test_nearest_never_emits_an_unrenderable_index);
    RUN_TEST(test_nearest_handles_out_of_range_input);
    RUN_TEST(test_nearest_agrees_with_brute_force);
    RUN_TEST(test_nearest_mid_grey_is_green_not_black_or_white);
    RUN_TEST(test_nearest_extremes_are_black_and_white);

    RUN_TEST(test_pair_packs_high_nibble_first);
    RUN_TEST(test_pair_never_emits_an_unrenderable_index);
    RUN_TEST(test_pair_averages_towards_unreachable_colours);

    RUN_TEST(test_row_none_packs_two_pixels_per_byte);
    RUN_TEST(test_row_none_odd_width_pads_with_white);
    RUN_TEST(test_row_none_leaves_a_solid_row_flat);
    RUN_TEST(test_row_none_rejects_null);
    RUN_TEST(test_tone_rect_then_no_tone_equals_tone_at_the_pack);

    RUN_TEST(test_row_quality_adds_texture_where_none_does_not);
    RUN_TEST(test_row_quality_pattern_changes_between_rows);
    RUN_TEST(test_row_quality_is_deterministic);
    RUN_TEST(test_row_quality_never_emits_an_unrenderable_index);
    RUN_TEST(test_row_quality_handles_a_full_gradient);
    RUN_TEST(test_row_quality_odd_width_writes_the_final_pixel);
    RUN_TEST(test_row_quality_rejects_null);

    RUN_TEST(test_stock_cfg_is_identical_to_the_legacy_path);
    RUN_TEST(test_stock_output_matches_its_golden);
    RUN_TEST(test_default_output_matches_its_golden);
    RUN_TEST(test_manual_output_matches_its_golden);
    RUN_TEST(test_default_is_the_epdopt_palette_fully_compressed);
    RUN_TEST(test_every_palette_id_is_complete);
    RUN_TEST(test_palette_names_round_trip_and_are_unique);
    RUN_TEST(test_unknown_palette_name_is_refused);
    RUN_TEST(test_out_of_range_palette_falls_back_to_the_default);
    RUN_TEST(test_stock_palette_entry_has_no_compression);
    RUN_TEST(test_every_render_palette_has_black_first_and_white_second);
    RUN_TEST(test_manual_and_measured_share_their_anchors);
    RUN_TEST(test_measured_palette_layout_and_validity);
    RUN_TEST(test_measured_white_is_a_mid_grey_and_black_is_not_black);
    RUN_TEST(test_tone_compression_maps_the_ends_onto_the_panel);
    RUN_TEST(test_measured_render_differs_from_stock_on_a_real_gradient);
    RUN_TEST(test_measured_render_never_emits_an_unrenderable_index);
    RUN_TEST(test_cfg_rejects_null);

    RUN_TEST(test_fit_landscape_into_portrait_screen);
    RUN_TEST(test_fit_portrait_into_portrait_screen);
    RUN_TEST(test_fit_exact_match_is_untouched);
    RUN_TEST(test_fit_scales_small_images_up);
    RUN_TEST(test_fit_never_exceeds_the_screen);
    RUN_TEST(test_fit_rejects_bad_dimensions);

    RUN_TEST(test_reduction_is_the_binding_ratio_not_the_long_edge);
    RUN_TEST(test_reduction_steps_down_for_the_alignment_crop);
    RUN_TEST(test_reduction_never_goes_below_one);
    RUN_TEST(test_reduction_never_stores_less_than_is_drawn);

    RUN_TEST(test_rotate_turns_a_disagreeing_picture);
    RUN_TEST(test_rotate_leaves_an_agreeing_picture_alone);
    RUN_TEST(test_rotate_protects_a_near_square_composition);
    RUN_TEST(test_rotate_is_exact_at_the_threshold);
    RUN_TEST(test_rotate_rejects_bad_dimensions);
    RUN_TEST(test_rotate_always_increases_the_drawn_area);

    return UNITY_END();
}
