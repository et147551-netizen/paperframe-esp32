// Host-side tests for the logic that does not need the panel.
//
// Run: ~/.platformio/penv/Scripts/pio.exe test -e native
//
// These exist because .scratch/phase0-preflight/issues/03 writes a driver against
// hardware nobody has touched yet. They do not prove the driver works -- only the
// panel can do that -- but they fix the parts that are checkable, so arrival day is
// spent on the panel's behaviour rather than on our own typos.

#include <string.h>

#include <unity.h>

#include "epd_cmds.h"
#include "epd_format.h"
#include "epd_geom.h"
#if defined(BOARD_M5PAPER_COLOR)
#include "epd_init_list_el040ef1.h"
#elif defined(BOARD_RETERMINAL_E1002)
#include "epd_init_list_ed2208gca.h"
#else
#error "No board selected: define BOARD_M5PAPER_COLOR or BOARD_RETERMINAL_E1002"
#endif

void setUp(void) {}
void tearDown(void) {}

// ------------------------------------------------------------ init list golden

// TWO PANELS, TWO GOLDEN LISTS, AND THEY ARE NOT THE SAME KIND OF CHECK. Read this before
// trusting either.
//
// The EL040EF1 list below was transcribed a SECOND TIME, INDEPENDENTLY, from a source that is not
// the driver's. Until 2026-09-18 that source was M5GFX
// (refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.hpp:52-67 -- LovyanGFX, FreeBSD / BSD-2-Clause,
// (c) lovyan03, within M5GFX, MIT (c) 2021 M5Stack); since ticket 65 item 2 it is the panel
// vendor's own driver,
// refs/waveshare-e-Paper/E-paper_Separate_Program/4inch_e-Paper_E/ESP32/EPD_4in0e.cpp:127-186
// (Waveshare team, MIT, HEAD a794fbc). That makes the check STRONGER, not merely differently
// sourced: the two arrays no longer share a lineage at all, so an error in M5GFX's transcription of
// the vendor would now show up here rather than being reproduced twice.
//
// **AND IT DID. One byte below is deliberately NOT the vendor's**: PFS's first byte is 0x03, which
// is M5GFX's value where the vendor sends 0x00. That single deviation is documented at the row in
// epd_init_list_el040ef1.h, with a third source agreeing with the vendor and a datasheet reading of
// what the byte does. The driver ships 0x03, so this golden list carries 0x03 -- otherwise the test
// would fail on a difference that is recorded rather than accidental. If it fails on any OTHER byte,
// re-read EPD_4in0e.cpp -- do not "fix" it by copying one array over the other, which would defeat
// the entire exercise.
//
// The ED2208-GCA list is copied from THE SAME FILE the driver's list came from
// (refs/esp32-photoframe/.../driver_ed2208_gca.c). It is therefore a transcription check only:
// it catches a typo made while writing epd_init_list_ed2208gca.h and it CANNOT catch that
// reference being wrong for the real panel. Ticket 63 is the only thing that can. Said plainly
// here because a passing test that looks like the one above it is exactly how a weaker check
// gets read as a stronger one.
#if defined(BOARD_M5PAPER_COLOR)
static const uint8_t golden_init_list[] = {
    0xAA, 0x06, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18,
    0x01, 0x01, 0x3F,
    0x00, 0x02, 0x5F, 0x69,
    0x05, 0x04, 0x40, 0x1F, 0x1F, 0x2C,
    0x08, 0x04, 0x6F, 0x1F, 0x1F, 0x22,
    0x06, 0x04, 0x6F, 0x1F, 0x17, 0x17,
    0x03, 0x04, 0x03, 0x54, 0x00, 0x44,
    0x60, 0x02, 0x02, 0x00,
    0x30, 0x01, 0x08,
    0x50, 0x01, 0x3F,
    0xE3, 0x01, 0x2F,
    0x84, 0x01, 0x01,
    0xFF, 0xFF,
};
// The waveform-rate byte, the entry count, and where the init-list BTST2's last byte sits in
// the golden array. Per panel, because all three differ.
#define GOLDEN_PLL 0x08
#define GOLDEN_ENTRIES 12
#define GOLDEN_BTST2_LAST_INDEX 32
#define GOLDEN_BTST2_LAST 0x17
// This panel sends TRES separately, after the list and after a second BUSY wait.
#define GOLDEN_HAS_SEPARATE_TRES 1
#else // BOARD_RETERMINAL_E1002
static const uint8_t golden_init_list[] = {
    0xAA, 0x06, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18,
    0x01, 0x01, 0x3F,
    0x00, 0x02, 0x5F, 0x69,
    0x03, 0x04, 0x00, 0x54, 0x00, 0x44,
    0x05, 0x04, 0x40, 0x1F, 0x1F, 0x2C,
    0x06, 0x04, 0x6F, 0x1F, 0x16, 0x25,
    0x08, 0x04, 0x6F, 0x1F, 0x1F, 0x22,
    0x30, 0x01, 0x03,
    0x50, 0x01, 0x3F,
    0x60, 0x02, 0x02, 0x00,
    0x61, 0x04, 0x03, 0x20, 0x01, 0xE0,
    0x84, 0x01, 0x01,
    0xE3, 0x01, 0x2F,
    0xFF, 0xFF,
};
#define GOLDEN_PLL 0x03
#define GOLDEN_ENTRIES 13
// TRES lives IN the list on this panel, so there is no separate payload to check -- and the
// list's own bytes are what test_init_list_matches_golden() already covers.
#define GOLDEN_HAS_SEPARATE_TRES 0
#endif

static void test_init_list_matches_golden(void)
{
    TEST_ASSERT_EQUAL_UINT(sizeof(golden_init_list), sizeof(epd_init_list));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(golden_init_list, epd_init_list,
                                  sizeof(golden_init_list));
}

// Walk the list the way the driver will, and check it is structurally sound: every
// entry has a length, the walk lands exactly on the terminator, and PLL carries the
// panel's stock rate. If PLL is not that, the baseline is not the baseline and issues/08 has
// no zero point -- on the EL040EF1, where issues/08 ran. On the ED2208-GCA there is no sweep
// to have a zero point of; the check is that the transcription kept the reference's byte.
static void test_init_list_walks_and_pll_is_stock(void)
{
    size_t i = 0;
    bool saw_pll = false;
    int entries = 0;

    while (i < sizeof(epd_init_list) && epd_init_list[i] != EPD_INIT_END) {
        const uint8_t cmd = epd_init_list[i];
        const uint8_t len = epd_init_list[i + 1];
        TEST_ASSERT_GREATER_THAN_UINT8(0, len);

        if (cmd == EPD_CMD_PLL) {
            saw_pll = true;
            TEST_ASSERT_EQUAL_UINT8(1, len);
            TEST_ASSERT_EQUAL_HEX8(GOLDEN_PLL, epd_init_list[i + 2]);
        }
        i += 2u + len;
        entries++;
    }

    TEST_ASSERT_EQUAL_UINT(sizeof(epd_init_list) - 2, i); // landed on the terminator
    TEST_ASSERT_EQUAL_INT(GOLDEN_ENTRIES, entries);
    TEST_ASSERT_TRUE(saw_pll);
    TEST_ASSERT_EQUAL_HEX8(EPD_INIT_END, epd_init_list[i]);
    TEST_ASSERT_EQUAL_HEX8(EPD_INIT_END, epd_init_list[i + 1]);
}

#if GOLDEN_HAS_SEPARATE_TRES
// TRES is sent separately, not from the list. 400 = 0x0190, 600 = 0x0258, MSB first. Asserted
// against EPD_WIDTH/EPD_HEIGHT rather than the literals, so the two definitions of the panel's
// size cannot drift apart -- ticket 43's seam 1, which is what this test now also guards.
static void test_tres_payload_encodes_the_panel_size(void)
{
    TEST_ASSERT_EQUAL_UINT(4, sizeof(epd_tres_payload));
    const int width = (epd_tres_payload[0] << 8) | epd_tres_payload[1];
    const int height = (epd_tres_payload[2] << 8) | epd_tres_payload[3];
    TEST_ASSERT_EQUAL_INT(EPD_WIDTH, width);
    TEST_ASSERT_EQUAL_INT(EPD_HEIGHT, height);
}

// The refresh-time BTST2 differs from the init-list BTST2 in its last byte: 0x27 vs 0x17. Easy
// to normalise by accident while tidying; that would silently change the stock baseline.
//
// EL040EF1-ONLY, and not because of a missing transcription: the ED2208-GCA reference does NOT
// resend BTST2 between PON and DRF at all. There is no second payload on that panel to compare,
// and adding one would be sending a booster payload nothing has ever sent to that part.
static void test_refresh_btst_differs_from_init_btst(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x27, epd_btst2_refresh_payload[3]);
    TEST_ASSERT_EQUAL_HEX8(GOLDEN_BTST2_LAST, golden_init_list[GOLDEN_BTST2_LAST_INDEX]);
}
#endif // GOLDEN_HAS_SEPARATE_TRES

// ---------------------------------------------------------------- frame packing

static void test_valid_colours(void)
{
    TEST_ASSERT_TRUE(epd_color_valid(EPD_COLOR_BLACK));
    TEST_ASSERT_TRUE(epd_color_valid(EPD_COLOR_WHITE));
    TEST_ASSERT_TRUE(epd_color_valid(EPD_COLOR_YELLOW));
    TEST_ASSERT_TRUE(epd_color_valid(EPD_COLOR_RED));
    TEST_ASSERT_TRUE(epd_color_valid(EPD_COLOR_BLUE));
    TEST_ASSERT_TRUE(epd_color_valid(EPD_COLOR_GREEN));
}

// Index 4 is orange and is valid on NEITHER panel here -- the reference firmware's own
// epaper.h defines the same six indices for its 7.3" ED2208-GCA and skips 0x4, so the "valid on
// the 7.3" part" note this comment used to carry was wrong. The handoff document's palette has
// 4=blue, which would render blue as an invalid index and green as blue.
static void test_index_four_is_rejected(void)
{
    TEST_ASSERT_FALSE(epd_color_valid(EPD_COLOR_ORANGE_UNUSED));
    TEST_ASSERT_FALSE(epd_color_valid(7));
    TEST_ASSERT_FALSE(epd_color_valid(0xFF));
}

static void test_pack_pair_is_high_nibble_first(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x35, epd_pack_pair(EPD_COLOR_RED, EPD_COLOR_BLUE));
    TEST_ASSERT_EQUAL_HEX8(0x01, epd_pack_pair(EPD_COLOR_BLACK, EPD_COLOR_WHITE));
    TEST_ASSERT_EQUAL_HEX8(0x10, epd_pack_pair(EPD_COLOR_WHITE, EPD_COLOR_BLACK));
}

// Two pixels per byte, from the panel's own dimensions. 400x600 is 120,000 bytes and 800x480
// is 192,000 -- computed here rather than hardcoded, so this checks the DERIVATION in
// epd_geom.h and not a number somebody typed twice.
static void test_frame_is_two_pixels_per_byte(void)
{
    TEST_ASSERT_EQUAL_UINT((EPD_WIDTH * EPD_HEIGHT) / 2, EPD_FRAME_BYTES);
    TEST_ASSERT_EQUAL_UINT(0, (EPD_WIDTH * EPD_HEIGHT) % 2);
}

static uint8_t frame[EPD_FRAME_BYTES];

static void test_pack_solid_fills_whole_frame(void)
{
    memset(frame, 0xAA, sizeof(frame));
    const size_t n = epd_pack_solid(frame, sizeof(frame), EPD_COLOR_GREEN);

    TEST_ASSERT_EQUAL_UINT(EPD_FRAME_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(0x66, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x66, frame[EPD_FRAME_BYTES / 2]);
    TEST_ASSERT_EQUAL_HEX8(0x66, frame[EPD_FRAME_BYTES - 1]);
}

static void test_pack_solid_rejects_bad_input(void)
{
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_solid(frame, sizeof(frame),
                                             EPD_COLOR_ORANGE_UNUSED));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_solid(frame, EPD_FRAME_BYTES - 1,
                                             EPD_COLOR_WHITE));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_solid(NULL, sizeof(frame), EPD_COLOR_WHITE));
}

// ------------------------------------------------------------------ chart packing

// The issues/20 layout: two reference rows bracketing the six colours under test.
static const uint8_t chart_2x5[] = {
    EPD_COLOR_WHITE,  EPD_COLOR_BLACK,
    EPD_COLOR_RED,    EPD_COLOR_YELLOW,
    EPD_COLOR_GREEN,  EPD_COLOR_BLUE,
    EPD_COLOR_WHITE,  EPD_COLOR_BLACK,
    EPD_COLOR_WHITE,  EPD_COLOR_BLACK,
};

// The chart's cell geometry, derived rather than substituted. epd_pack_chart() decides a
// pixel's cell with `(x * cols) / EPD_WIDTH` and `(y * rows) / EPD_HEIGHT`, so these are that
// arithmetic inverted, and every expectation below is written in terms of them. They used to be
// literals computed for 400x600 by hand -- which made three of these tests assertions about the
// M5Paper Color rather than about the packer.
#define CELL_W(cols) (EPD_WIDTH / (cols))
#define CELL_H(rows) (EPD_HEIGHT / (rows))
// The first x that belongs to column `n` of `cols`: the smallest x with x*cols >= n*EPD_WIDTH,
// i.e. ceil(n*EPD_WIDTH/cols). At 3 columns this is 134 on a 400-wide panel and 267 on an
// 800-wide one -- both odd, so both fall in the low nibble of their byte, which is the whole
// point of test_pack_chart_handles_odd_boundaries().
#define COL_START(n, cols) (((n) * EPD_WIDTH + (cols) - 1) / (cols))

// Palette index of one physical pixel, straight out of the packed frame.
static uint8_t pixel_at(const uint8_t *packed, int x, int y)
{
    const uint8_t byte = packed[(size_t)y * (EPD_WIDTH / 2) + (size_t)(x / 2)];
    return (x % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
}

static void test_pack_chart_puts_each_patch_in_its_cell(void)
{
    memset(frame, 0xAA, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT(EPD_FRAME_BYTES,
                           epd_pack_chart(frame, sizeof(frame), chart_2x5, 2, 5));

    // Centre of each of the ten cells.
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 2; c++) {
            const int x = c * CELL_W(2) + CELL_W(2) / 2;
            const int y = r * CELL_H(5) + CELL_H(5) / 2;
            TEST_ASSERT_EQUAL_HEX8(chart_2x5[r * 2 + c], pixel_at(frame, x, y));
        }
    }
}

// A gap between cells would have to be drawn in one of the colours being measured,
// so there are no gaps: every pixel belongs to a patch. 0xAA survives nowhere.
static void test_pack_chart_leaves_no_pixel_undrawn(void)
{
    memset(frame, 0xAA, sizeof(frame));
    epd_pack_chart(frame, sizeof(frame), chart_2x5, 2, 5);
    for (size_t i = 0; i < EPD_FRAME_BYTES; i++) {
        TEST_ASSERT_NOT_EQUAL_HEX8(0xAA, frame[i]);
    }
}

// The boundary is the one place the arithmetic can be off by a pixel, and a patch
// bleeding one column into its neighbour is invisible in a photograph.
static void test_pack_chart_cell_boundaries_are_exact(void)
{
    epd_pack_chart(frame, sizeof(frame), chart_2x5, 2, 5);

    // The column boundary, which for an even cell width falls inside a single packed byte.
    // Sampled in the second row of cells, whose two patches are RED and YELLOW.
    const int mid_row_y = CELL_H(5) + CELL_H(5) / 2;
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_RED, pixel_at(frame, CELL_W(2) - 1, mid_row_y));
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_YELLOW, pixel_at(frame, CELL_W(2), mid_row_y));
    // The first row boundary: WHITE above it, RED below.
    const int col0_x = CELL_W(2) / 2;
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_WHITE, pixel_at(frame, col0_x, CELL_H(5) - 1));
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_RED, pixel_at(frame, col0_x, CELL_H(5)));
    // The far corners are the first and last patch, not a rounded-off eleventh cell.
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_WHITE, pixel_at(frame, 0, 0));
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_BLACK, pixel_at(frame, EPD_WIDTH - 1,
                                                     EPD_HEIGHT - 1));
}

// A cell boundary that does not fall on a byte boundary still lands on the right pixel.
//
// WHICH of the two boundaries that is depends on the panel, so the test checks both and asserts
// only that at least one is mid-byte. Three columns of 400 put them at x = 134 (even, so it
// coincides with a byte boundary) and x = 267 (odd, mid-byte); three of 800 put them at 267 and
// 534, the other way round. The comment here used to say the first boundary was "the low nibble
// of its byte", which was wrong even at 400 -- x = 134 is a HIGH nibble -- and the assertion
// below is deliberately the weaker, true one rather than the confident, false one.
static void test_pack_chart_handles_odd_boundaries(void)
{
    const uint8_t three[] = {EPD_COLOR_RED, EPD_COLOR_GREEN, EPD_COLOR_BLUE};
    TEST_ASSERT_EQUAL_UINT(EPD_FRAME_BYTES,
                           epd_pack_chart(frame, sizeof(frame), three, 3, 1));
    const int y = EPD_HEIGHT / 2;
    // At least one boundary is genuinely mid-byte, which is what makes this test worth having.
    TEST_ASSERT_TRUE(COL_START(1, 3) % 2 == 1 || COL_START(2, 3) % 2 == 1);
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_RED, pixel_at(frame, COL_START(1, 3) - 1, y));
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_GREEN, pixel_at(frame, COL_START(1, 3), y));
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_GREEN, pixel_at(frame, COL_START(2, 3) - 1, y));
    TEST_ASSERT_EQUAL_HEX8(EPD_COLOR_BLUE, pixel_at(frame, COL_START(2, 3), y));
}

static void test_pack_chart_rejects_bad_input(void)
{
    const uint8_t with_orange[] = {EPD_COLOR_WHITE, EPD_COLOR_ORANGE_UNUSED};
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, sizeof(frame), with_orange, 2, 1));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, EPD_FRAME_BYTES - 1, chart_2x5, 2, 5));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(NULL, sizeof(frame), chart_2x5, 2, 5));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, sizeof(frame), NULL, 2, 5));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, sizeof(frame), chart_2x5, 0, 5));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, sizeof(frame), chart_2x5, 2, 0));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, sizeof(frame), chart_2x5, 2,
                                             EPD_HEIGHT + 1));
}

// An invalid patch anywhere rejects the whole chart, rather than drawing the good
// part of it: a frame with one wrong patch photographs like a measurement.
static void test_pack_chart_rejects_before_drawing_anything(void)
{
    uint8_t bad[10];
    memcpy(bad, chart_2x5, sizeof(bad));
    bad[9] = EPD_COLOR_ORANGE_UNUSED;

    memset(frame, 0xAA, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT(0, epd_pack_chart(frame, sizeof(frame), bad, 2, 5));
    TEST_ASSERT_EQUAL_HEX8(0xAA, frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, frame[EPD_FRAME_BYTES - 1]);
}

// ------------------------------------------------------------ temperature codec

// Anchors straight from the datasheet's TSC table.
static void test_tsc_decode_datasheet_anchors(void)
{
    TEST_ASSERT_EQUAL_INT(0, epd_tsc_decode(0x00));
    TEST_ASSERT_EQUAL_INT(25, epd_tsc_decode(0x19));
    TEST_ASSERT_EQUAL_INT(60, epd_tsc_decode(0x3C));
    TEST_ASSERT_EQUAL_INT(-25, epd_tsc_decode(0xE7));
}

// Ticket 18: 0x00 and 0xFF are the two values the read returned for months, and
// they are what an undriven line settles to, not two temperatures. The decoder was
// never the bug -- the read was on the wrong pin -- so these two stay legal decodes
// and must not be turned into error cases here. A byte the panel did not send is
// caught by not reading a floating pin, which epd_read_temperature() now does not.
static void test_tsc_decode_accepts_the_floating_line_values(void)
{
    TEST_ASSERT_EQUAL_INT(0, epd_tsc_decode(0x00));
    TEST_ASSERT_EQUAL_INT(-1, epd_tsc_decode(0xFF));

    // The value the panel actually returned on MOSI at 29.3 C room air, 2026-09-03.
    TEST_ASSERT_EQUAL_INT(35, epd_tsc_decode(0x23));
}

static void test_tse_encode_signed_nibble(void)
{
    uint8_t out = 0xFF;

    TEST_ASSERT_TRUE(epd_tse_encode(0, &out));
    TEST_ASSERT_EQUAL_HEX8(0x00, out);
    TEST_ASSERT_TRUE(epd_tse_encode(7, &out));
    TEST_ASSERT_EQUAL_HEX8(0x07, out);
    TEST_ASSERT_TRUE(epd_tse_encode(-8, &out));
    TEST_ASSERT_EQUAL_HEX8(0x08, out);
    TEST_ASSERT_TRUE(epd_tse_encode(-1, &out));
    TEST_ASSERT_EQUAL_HEX8(0x0F, out);

    // psiegl's working probe writes TSE = 0x02: internal sensor, code +2. Measured
    // on hardware (ticket 18) that is +1.0 C, not +2 C -- TO[3:0] steps in half
    // degrees. The code is what this function builds, so the assertion is unchanged;
    // the unit in the old comment was wrong.
    TEST_ASSERT_TRUE(epd_tse_encode(2, &out));
    TEST_ASSERT_EQUAL_HEX8(0x02, out);
}

// Bit 7 selects the sensor and must stay 0 -- there is no external sensor on this
// board, J5 pins 24/25 are unconnected.
static void test_tse_never_selects_external_sensor(void)
{
    uint8_t out;
    for (int c = -8; c <= 7; c++) {
        TEST_ASSERT_TRUE(epd_tse_encode(c, &out));
        TEST_ASSERT_EQUAL_HEX8(0x00, out & 0x80);
    }
}

// The register physically cannot go past -8..+7, which bounds the whole of Phase 3.
static void test_tse_rejects_out_of_range(void)
{
    uint8_t out;
    TEST_ASSERT_FALSE(epd_tse_encode(8, &out));
    TEST_ASSERT_FALSE(epd_tse_encode(-9, &out));
    TEST_ASSERT_FALSE(epd_tse_encode(100, &out));
    TEST_ASSERT_FALSE(epd_tse_encode(0, NULL));
}

// ---------------------------------------------------------------- the FRS model

static void test_frs_linear_range(void)
{
    TEST_ASSERT_EQUAL_FLOAT(12.5f, epd_frs_rate_hz_datasheet(0x00));
    TEST_ASSERT_EQUAL_FLOAT(25.0f, epd_frs_rate_hz_datasheet(0x01));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_datasheet(0x03));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, epd_frs_rate_hz_datasheet(0x07));
    TEST_ASSERT_EQUAL_FLOAT(187.5f, epd_frs_rate_hz_datasheet(0x0E));
}

// 0x03 landing on 50 Hz -- the power-on default and the "other" fallback -- is the
// non-trivial prediction the circulating table already satisfies.
static void test_frs_power_on_default_is_50hz(void)
{
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_datasheet(0x03));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_datasheet(0x3C));
}

// The two readings agree everywhere except 0x08-0x0F. That is what makes the 0x0F
// measurement in issues/08 decisive rather than merely confirmatory.
static void test_frs_readings_disagree_only_above_0x07(void)
{
    for (uint8_t a = 0x00; a <= 0x07; a++) {
        TEST_ASSERT_EQUAL_FLOAT(epd_frs_rate_hz_datasheet(a),
                                epd_frs_rate_hz_observed(a));
    }
    // The stock setting, 0x08: 112.5 Hz by the datasheet, 50 Hz by the table.
    TEST_ASSERT_EQUAL_FLOAT(112.5f, epd_frs_rate_hz_datasheet(0x08));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_observed(0x08));

    // 0x0F: 200 Hz by the datasheet, 50 Hz by the table.
    TEST_ASSERT_EQUAL_FLOAT(200.0f, epd_frs_rate_hz_datasheet(0x0F));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_observed(0x0F));
}

// Explicit encodings above the linear range. Swept on this panel on 2026-09-04, and the
// answer is that they are not decoded: both measure the stock 0x08's refresh time to
// within the arms' own spread, and their chart scans are the 0x08 chart to within the
// scanner floor. So the datasheet reading keeps its 200/100 Hz and the observed reading
// says 50 Hz, which is what the panel does.
static void test_frs_explicit_encodings(void)
{
    TEST_ASSERT_EQUAL_FLOAT(200.0f, epd_frs_rate_hz_datasheet(0x39));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, epd_frs_rate_hz_datasheet(0x3A));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_observed(0x39));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_observed(0x3A));

    // The prediction that a shared decode would have satisfied -- 0x3A measuring the same
    // as 0x07 -- is what the sweep refuted. 0x3A measured 15,007 ms against 0x07's
    // 8,388 ms, so this holds for the datasheet's model only.
    TEST_ASSERT_EQUAL_FLOAT(epd_frs_rate_hz_datasheet(0x07),
                            epd_frs_rate_hz_datasheet(0x3A));

    // 0x3C is 50 Hz in both readings: the power-on default.
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_observed(0x3C));
}

static void test_frs_unmapped_values_fall_back_to_50hz(void)
{
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_datasheet(0x20));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, epd_frs_rate_hz_datasheet(0x3F));
}

// Reproduces the two anchor points of the research §11.2 fit. These are predictions
// from someone else's single-sample table, not measurements -- the tolerance is loose
// on purpose, and phase0-preflight/issues/04 re-fits this properly.
static void test_frs_predicted_ms_reproduces_fit_anchors(void)
{
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 19083.0f, epd_frs_predicted_ms(50.0f));
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 9819.0f, epd_frs_predicted_ms(100.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, epd_frs_predicted_ms(0.0f));
}

// -------------------------------------------------------------------- CSV output

static void test_csv_header_column_count(void)
{
    int commas = 0;
    for (const char *p = EPD_CSV_HEADER; *p; p++) {
        if (*p == ',') {
            commas++;
        }
    }
    TEST_ASSERT_EQUAL_INT(11, commas); // 12 columns: 10, plus seq and t_pon2drf_ms
}

static void test_seq_name(void)
{
    TEST_ASSERT_EQUAL_STRING("stock", epd_seq_name(0));
    TEST_ASSERT_EQUAL_STRING("busy", epd_seq_name(1));
    // Anything else must be visible as unknown rather than silently reported as one
    // of the two real modes -- a mislabelled arm is a wrong measurement.
    TEST_ASSERT_EQUAL_STRING("?", epd_seq_name(2));
    TEST_ASSERT_EQUAL_STRING("?", epd_seq_name(255));
}

static void test_csv_row_matches_header_shape(void)
{
    const epd_timings_t t = {
        .run = 7,
        .frs = 0x08,
        .seq = 1,
        .temp_c = 25,
        .reset_us = 202000,
        .init_us = 3500,
        .xfer_us = 240000,
        .pon_us = 85000,
        .drf_us = 18500000,
        .pof_us = 90000,
        .total_us = 19120500,
        .pon_to_drf_us = 85200,
    };
    char buf[256];
    const int n = epd_csv_row(buf, sizeof(buf), &t);

    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(buf), n);

    int commas = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == ',') {
            commas++;
        }
    }
    TEST_ASSERT_EQUAL_INT(11, commas);

    // us -> ms, frs printed as hex so a sweep is readable without decoding, and the
    // sequencing arm as its name for the same reason.
    TEST_ASSERT_EQUAL_STRING("7,0x08,25,202.000,3.500,240.000,85.000,18500.000,90.000,"
                             "19120.500,busy,85.200",
                             buf);
}

static void test_csv_row_rejects_null(void)
{
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-1, epd_csv_row(buf, sizeof(buf), NULL));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_list_matches_golden);
    RUN_TEST(test_init_list_walks_and_pll_is_stock);
#if GOLDEN_HAS_SEPARATE_TRES
    RUN_TEST(test_tres_payload_encodes_the_panel_size);
    RUN_TEST(test_refresh_btst_differs_from_init_btst);
#endif

    RUN_TEST(test_valid_colours);
    RUN_TEST(test_index_four_is_rejected);
    RUN_TEST(test_pack_pair_is_high_nibble_first);
    RUN_TEST(test_frame_is_two_pixels_per_byte);
    RUN_TEST(test_pack_solid_fills_whole_frame);
    RUN_TEST(test_pack_solid_rejects_bad_input);

    RUN_TEST(test_pack_chart_puts_each_patch_in_its_cell);
    RUN_TEST(test_pack_chart_leaves_no_pixel_undrawn);
    RUN_TEST(test_pack_chart_cell_boundaries_are_exact);
    RUN_TEST(test_pack_chart_handles_odd_boundaries);
    RUN_TEST(test_pack_chart_rejects_bad_input);
    RUN_TEST(test_pack_chart_rejects_before_drawing_anything);

    RUN_TEST(test_tsc_decode_datasheet_anchors);
    RUN_TEST(test_tsc_decode_accepts_the_floating_line_values);
    RUN_TEST(test_tse_encode_signed_nibble);
    RUN_TEST(test_tse_never_selects_external_sensor);
    RUN_TEST(test_tse_rejects_out_of_range);

    RUN_TEST(test_frs_linear_range);
    RUN_TEST(test_frs_power_on_default_is_50hz);
    RUN_TEST(test_frs_readings_disagree_only_above_0x07);
    RUN_TEST(test_frs_explicit_encodings);
    RUN_TEST(test_frs_unmapped_values_fall_back_to_50hz);
    RUN_TEST(test_frs_predicted_ms_reproduces_fit_anchors);

    RUN_TEST(test_csv_header_column_count);
    RUN_TEST(test_seq_name);
    RUN_TEST(test_csv_row_matches_header_shape);
    RUN_TEST(test_csv_row_rejects_null);

    return UNITY_END();
}
