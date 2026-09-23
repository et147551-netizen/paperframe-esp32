// Host-side tests for the 5x7 font.
//
// **The dump at the end is the point of this file.** A hand-drawn glyph table cannot be tested
// for being the right shape -- only a person can say whether 'S' looks like an S -- so the last
// test prints all 49 glyphs as ASCII grids and that output is what gets proof-read. Everything
// above it tests the things a machine can check: pitch, clipping, rotation, and that a byte with
// no glyph draws something rather than nothing.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "epd_text.h"

void setUp(void) {}
void tearDown(void) {}

// Wide enough for two scale-1 glyphs and a gap and narrow enough that a third is cut, tall
// enough that a scale-2 glyph (14 rows) fits -- at 12 it did not, and the scale test's arithmetic
// silently became a clipping test.
#define W 16
#define H 16
static uint8_t buffer[W * H * 3];
static epd_canvas_t canvas;

static void make_canvas(void)
{
    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, buffer, sizeof(buffer), W, H));
    epd_canvas_fill(&canvas, 255, 255, 255);
}

// Logical, so it follows the canvas's rotation the way the drawing does.
static bool is_ink(int32_t x, int32_t y)
{
    const uint8_t *p = epd_canvas_pixel(&canvas, x, y);
    TEST_ASSERT_NOT_NULL(p);
    return p[0] == 0 && p[1] == 0 && p[2] == 0;
}

static int ink_count(void)
{
    int n = 0;
    for (size_t off = 0; off < sizeof(buffer); off += 3) {
        if (buffer[off] == 0 && buffer[off + 1] == 0 && buffer[off + 2] == 0) {
            n++;
        }
    }
    return n;
}

// ----------------------------------------------------------------------- metrics

static void test_width_counts_pitch_but_not_the_trailing_gap(void)
{
    TEST_ASSERT_EQUAL_INT32(5, epd_text_width("A", 1));
    TEST_ASSERT_EQUAL_INT32(11, epd_text_width("AB", 1));
    TEST_ASSERT_EQUAL_INT32(17, epd_text_width("ABC", 1));
    // Three at scale 3: 3 * 6 * 3 - 3.
    TEST_ASSERT_EQUAL_INT32(51, epd_text_width("ABC", 3));
}

static void test_width_and_height_refuse_nothing_gracefully(void)
{
    TEST_ASSERT_EQUAL_INT32(0, epd_text_width(NULL, 3));
    TEST_ASSERT_EQUAL_INT32(0, epd_text_width("", 3));
    TEST_ASSERT_EQUAL_INT32(0, epd_text_width("A", 0));
    TEST_ASSERT_EQUAL_INT32(0, epd_text_width("A", -2));
    TEST_ASSERT_EQUAL_INT32(0, epd_text_height(0));
    TEST_ASSERT_EQUAL_INT32(21, epd_text_height(3));
}

// ------------------------------------------------------------------------ drawing

// '1' is 00100 / 01100 / 00100 x4 / 01110 -- ten pixels, and its top row is a single pixel at
// column 2. That makes it the cheapest glyph to assert exactly.
static void test_draws_a_known_glyph_at_the_origin(void)
{
    make_canvas();
    epd_text_draw(&canvas, 0, 0, 1, "1", 0, 0, 0);

    TEST_ASSERT_TRUE(is_ink(2, 0));
    TEST_ASSERT_FALSE(is_ink(1, 0));
    TEST_ASSERT_FALSE(is_ink(3, 0));
    TEST_ASSERT_TRUE(is_ink(1, 1));
    TEST_ASSERT_TRUE(is_ink(2, 1));
    // The foot.
    TEST_ASSERT_TRUE(is_ink(1, 6));
    TEST_ASSERT_TRUE(is_ink(2, 6));
    TEST_ASSERT_TRUE(is_ink(3, 6));
    TEST_ASSERT_FALSE(is_ink(0, 6));
    TEST_ASSERT_EQUAL_INT(10, ink_count());
}

static void test_the_second_glyph_starts_one_pitch_right(void)
{
    make_canvas();
    epd_text_draw(&canvas, 0, 0, 1, " 1", 0, 0, 0);

    // The space drew nothing, and the '1' is a whole advance along.
    TEST_ASSERT_EQUAL_INT(10, ink_count());
    TEST_ASSERT_TRUE(is_ink(2 + EPD_TEXT_ADVANCE, 0));
    TEST_ASSERT_FALSE(is_ink(2, 0));
}

static void test_scale_multiplies_both_axes(void)
{
    make_canvas();
    epd_text_draw(&canvas, 0, 0, 2, "1", 0, 0, 0);
    // Every pixel becomes a 2x2 block.
    TEST_ASSERT_EQUAL_INT(10 * 4, ink_count());
    TEST_ASSERT_TRUE(is_ink(4, 0));
    TEST_ASSERT_TRUE(is_ink(5, 1));
    TEST_ASSERT_FALSE(is_ink(6, 0));
}

// The canvas clips; this checks the font does not do anything worse on its way there, such as
// walking a negative index or stopping at the first clipped pixel.
static void test_clips_at_every_edge(void)
{
    const int32_t lw = W;
    const int32_t lh = H;

    make_canvas();
    epd_text_draw(&canvas, -3, -3, 1, "8", 0, 0, 0);
    const int top_left = ink_count();
    TEST_ASSERT_TRUE(top_left > 0);

    make_canvas();
    epd_text_draw(&canvas, lw - 2, lh - 2, 1, "8", 0, 0, 0);
    TEST_ASSERT_TRUE(ink_count() > 0);

    make_canvas();
    epd_text_draw(&canvas, lw + 4, 0, 1, "8", 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());

    make_canvas();
    epd_text_draw(&canvas, 0, lh + 4, 1, "8", 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());
}

// A long string is cut at the edge rather than wrapping or running off the buffer. The canvas
// is 16 logical pixels wide, so the third glyph is already past it.
static void test_a_string_longer_than_the_canvas_is_cut(void)
{
    make_canvas();
    epd_text_draw(&canvas, 0, 0, 1, "8888888888", 0, 0, 0);
    // Two whole 8s (12 pixels each) plus the four columns of the third that fit.
    TEST_ASSERT_TRUE(ink_count() > 0);
    TEST_ASSERT_TRUE(is_ink(15, 0) || is_ink(15, 3));
}

static void test_rotation_moves_the_text_with_the_picture(void)
{
    make_canvas();
    epd_canvas_set_rotation(&canvas, 1);
    epd_text_draw(&canvas, 0, 0, 1, "1", 0, 0, 0);

    // Same logical coordinates, same ten pixels -- the mapping is epd_canvas's and this is the
    // check that the font goes through it rather than around it.
    TEST_ASSERT_EQUAL_INT(10, ink_count());
    TEST_ASSERT_TRUE(is_ink(2, 0));
    TEST_ASSERT_TRUE(is_ink(1, 6));

    // And it landed in the physical corner rotation 1 maps logical (0,0) to, which for this
    // canvas is (0, H - 1): logical (2, 0) -> physical (0, H - 1 - 2).
    const size_t off = ((size_t)(H - 1 - 2) * W + 0) * 3;
    TEST_ASSERT_EQUAL_UINT8(0, buffer[off]);
    epd_canvas_set_rotation(&canvas, 0);
}

static void test_bad_arguments_draw_nothing(void)
{
    make_canvas();
    epd_text_draw(&canvas, 0, 0, 0, "8", 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());
    epd_text_draw(&canvas, 0, 0, 1, NULL, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());
    epd_text_draw(NULL, 0, 0, 1, "8", 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());
}

// ------------------------------------------------------------------ the glyph set

static void test_the_set_is_exactly_what_the_card_needs(void)
{
    static const char *present =
        " .-:/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef" EPD_TEXT_DEGREE "%";
    for (const char *p = present; *p; p++) {
        TEST_ASSERT_TRUE_MESSAGE(epd_text_has_glyph(*p), p);
    }

    // The degree sign is ONE byte and it is 0xB0. A `°` typed into this file would be 0xC2 0xB0
    // in UTF-8, and the first of those two bytes has no glyph -- so the assertion below is what
    // stops a caller writing the literal and getting a hollow box in front of every temperature.
    TEST_ASSERT_EQUAL_size_t(1, strlen(EPD_TEXT_DEGREE));
    TEST_ASSERT_EQUAL_UINT8(0xB0, (unsigned char)EPD_TEXT_DEGREE[0]);
    TEST_ASSERT_FALSE(epd_text_has_glyph((char)0xC2));

    // g-z lowercase are absent on purpose: the only lowercase on the glass is the access
    // point's password, which is hex. If a future card wants a lowercase word, the table grows
    // -- it must not be the hollow box that tells you, on the panel, after a flash.
    static const char *absent = "ghijklmnopqrstuvwxyz_=?+*,;'\"@";
    for (const char *p = absent; *p; p++) {
        TEST_ASSERT_FALSE_MESSAGE(epd_text_has_glyph(*p), p);
    }
}

// A missing glyph must be visible. Silently drawing nothing is how a wrong character survives to
// the glass, where finding it costs a 15 s refresh and a scan.
static void test_an_unknown_byte_draws_a_hollow_box(void)
{
    make_canvas();
    epd_text_draw(&canvas, 0, 0, 1, "?", 0, 0, 0);

    // The frame is ink and the middle is not.
    TEST_ASSERT_TRUE(is_ink(0, 0));
    TEST_ASSERT_TRUE(is_ink(4, 0));
    TEST_ASSERT_TRUE(is_ink(0, 6));
    TEST_ASSERT_TRUE(is_ink(4, 6));
    TEST_ASSERT_FALSE(is_ink(2, 3));

    uint8_t rows[EPD_TEXT_GLYPH_H];
    epd_text_glyph_rows('?', rows);
    TEST_ASSERT_EQUAL_UINT8(0x1F, rows[0]);
    TEST_ASSERT_EQUAL_UINT8(0x11, rows[3]);
    TEST_ASSERT_EQUAL_UINT8(0x1F, rows[6]);
}

// Every glyph in the set has to be drawn between the two rows that carry an ascender and a
// baseline, or a line of text steps up and down. Space is the exception.
static void test_no_glyph_is_empty_except_space(void)
{
    static const char *set =
        ".-:/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef" EPD_TEXT_DEGREE "%";
    for (const char *p = set; *p; p++) {
        uint8_t rows[EPD_TEXT_GLYPH_H];
        epd_text_glyph_rows(*p, rows);
        uint8_t any = 0;
        for (int i = 0; i < EPD_TEXT_GLYPH_H; i++) {
            TEST_ASSERT_TRUE_MESSAGE(rows[i] <= 0x1F, "a glyph row has a bit past column 5");
            any |= rows[i];
        }
        TEST_ASSERT_TRUE_MESSAGE(any != 0, p);
    }

    uint8_t rows[EPD_TEXT_GLYPH_H];
    epd_text_glyph_rows(' ', rows);
    for (int i = 0; i < EPD_TEXT_GLYPH_H; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, rows[i]);
    }
}

// Two glyphs that must not be the same picture, because the password is the one place the case
// matters and a reader has no context to correct from.
static void test_lowercase_hex_differs_from_its_capital(void)
{
    // Strings rather than char pairs, so the failure message is NUL-terminated.
    static const char *pairs[] = {"aA", "bB", "cC", "dD", "eE", "fF"};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        uint8_t lower[EPD_TEXT_GLYPH_H], upper[EPD_TEXT_GLYPH_H];
        epd_text_glyph_rows(pairs[i][0], lower);
        epd_text_glyph_rows(pairs[i][1], upper);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, memcmp(lower, upper, sizeof(lower)), pairs[i]);
    }
}

// THE PROOF-READING PASS. Not an assertion -- a person reads this output once, when the table is
// written or changed. `pio test -e native -v` shows it.
// Side by side in bands, not one above the other: the mistakes this catches are a glyph that
// looks like its neighbour ('8' against 'B', 'S' against '5'), and those are only visible when
// the two are on the same lines.
#define DUMP_BAND 12

static void test_dump_every_glyph_for_a_human(void)
{
    static const char *set =
        " .-:/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef" EPD_TEXT_DEGREE "%?";
    const size_t n = strlen(set);

    printf("\n--- glyph dump (proof-read this) ---\n");
    for (size_t base = 0; base < n; base += DUMP_BAND) {
        const size_t end = base + DUMP_BAND < n ? base + DUMP_BAND : n;
        for (size_t i = base; i < end; i++) {
            // 0xB0 is not printable on this console, so it gets a name rather than the byte.
            if ((unsigned char)set[i] == 0xB0u) {
                printf("  deg ");
            } else {
                printf("  '%c' ", set[i]);
            }
        }
        putchar('\n');
        for (int y = 0; y < EPD_TEXT_GLYPH_H; y++) {
            for (size_t i = base; i < end; i++) {
                uint8_t rows[EPD_TEXT_GLYPH_H];
                epd_text_glyph_rows(set[i], rows);
                putchar(' ');
                for (int x = 0; x < EPD_TEXT_GLYPH_W; x++) {
                    putchar((rows[y] & (1u << (EPD_TEXT_GLYPH_W - 1 - x))) ? '#' : '.');
                }
            }
            putchar('\n');
        }
        putchar('\n');
    }
    printf("--- end glyph dump ---\n");
    TEST_PASS();
}

// ----------------------------------------------------------------- turned 90 degrees

// The mapping is checked against the glyph table itself rather than against a hand-listed set of
// coordinates, so it covers every bit of the glyph and cannot be made to pass by editing a
// literal. And it is checked to be FALSIFIABLE in the same test: the anticlockwise mapping is
// asserted NOT to hold, which is the one wrong answer that a coordinate list would have accepted
// silently. 'A' is symmetric left-to-right and not top-to-bottom, so the two differ.
static void test_rot90_turns_the_glyph_clockwise(void)
{
    make_canvas();
    epd_text_draw_rot90(&canvas, 0, 0, 1, "A", 0, 0, 0);

    uint8_t rows[EPD_TEXT_GLYPH_H];
    epd_text_glyph_rows('A', rows);

    int anticlockwise_disagreements = 0;
    for (int32_t gy = 0; gy < EPD_TEXT_GLYPH_H; gy++) {
        for (int32_t gx = 0; gx < EPD_TEXT_GLYPH_W; gx++) {
            const bool set = (rows[gy] & (uint8_t)(1u << (EPD_TEXT_GLYPH_W - 1 - gx))) != 0;
            // Clockwise: the glyph's rows run towards -x, so row 0 -- the top of the letter --
            // lands at the largest x, and the glyph's own columns run down +y.
            TEST_ASSERT_EQUAL(set, is_ink(EPD_TEXT_GLYPH_H - 1 - gy, gx));
            // Anticlockwise would put row 0 at x = 0 and run the columns up instead.
            if (is_ink(gy, EPD_TEXT_GLYPH_W - 1 - gx) != set) {
                anticlockwise_disagreements++;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, anticlockwise_disagreements);
}

// Composition rather than a coordinate: "AB" turned must be exactly "A" turned plus "B" turned one
// pitch DOWN. If the advance went sideways, or the pitch were the glyph width instead of the
// height, the two buffers would differ.
static void test_rot90_advances_downward_by_one_pitch(void)
{
    make_canvas();
    epd_text_draw_rot90(&canvas, 0, 0, 1, "AB", 0, 0, 0);
    uint8_t together[sizeof(buffer)];
    memcpy(together, buffer, sizeof(buffer));

    make_canvas();
    epd_text_draw_rot90(&canvas, 0, 0, 1, "A", 0, 0, 0);
    epd_text_draw_rot90(&canvas, 0, EPD_TEXT_ADVANCE, 1, "B", 0, 0, 0);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(together, buffer, sizeof(buffer));
}

// The bounding box's extents swap, which is the part a caller sizing a band gets wrong: it has to
// check the band's WIDTH against epd_text_height() and its length against epd_text_width().
static void test_rot90_bounding_box_swaps_the_extents(void)
{
    make_canvas();
    epd_text_draw_rot90(&canvas, 0, 0, 1, "AB", 0, 0, 0);

    const int32_t box_w = epd_text_height(1);
    const int32_t box_h = epd_text_width("AB", 1);
    TEST_ASSERT_EQUAL_INT32(EPD_TEXT_GLYPH_H, box_w);
    TEST_ASSERT_EQUAL_INT32(11, box_h);

    for (int32_t y = 0; y < H; y++) {
        for (int32_t x = 0; x < W; x++) {
            if (x >= box_w || y >= box_h) {
                TEST_ASSERT_FALSE(is_ink(x, y));
            }
        }
    }
    // And it drew something, so the loop above is not passing on an empty canvas.
    TEST_ASSERT_GREATER_THAN_INT(0, ink_count());
}

static void test_rot90_clips_and_refuses_nothing_gracefully(void)
{
    make_canvas();
    epd_text_draw_rot90(&canvas, -20, -20, 2, "ABC", 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());

    make_canvas();
    epd_text_draw_rot90(NULL, 0, 0, 1, "A", 0, 0, 0);
    epd_text_draw_rot90(&canvas, 0, 0, 1, NULL, 0, 0, 0);
    epd_text_draw_rot90(&canvas, 0, 0, 0, "A", 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, ink_count());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_width_counts_pitch_but_not_the_trailing_gap);
    RUN_TEST(test_width_and_height_refuse_nothing_gracefully);

    RUN_TEST(test_draws_a_known_glyph_at_the_origin);
    RUN_TEST(test_the_second_glyph_starts_one_pitch_right);
    RUN_TEST(test_scale_multiplies_both_axes);
    RUN_TEST(test_clips_at_every_edge);
    RUN_TEST(test_a_string_longer_than_the_canvas_is_cut);
    RUN_TEST(test_rotation_moves_the_text_with_the_picture);
    RUN_TEST(test_bad_arguments_draw_nothing);

    RUN_TEST(test_rot90_turns_the_glyph_clockwise);
    RUN_TEST(test_rot90_advances_downward_by_one_pitch);
    RUN_TEST(test_rot90_bounding_box_swaps_the_extents);
    RUN_TEST(test_rot90_clips_and_refuses_nothing_gracefully);

    RUN_TEST(test_the_set_is_exactly_what_the_card_needs);
    RUN_TEST(test_an_unknown_byte_draws_a_hollow_box);
    RUN_TEST(test_no_glyph_is_empty_except_space);
    RUN_TEST(test_lowercase_hex_differs_from_its_capital);
    RUN_TEST(test_dump_every_glyph_for_a_human);

    return UNITY_END();
}
