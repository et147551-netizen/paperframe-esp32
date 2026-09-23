// Host-side tests for the matte band: where the leftover block is, how many lines it holds, which
// content wins them, and that drawing it can never touch the photograph.
//
// **The rule under test changed on 2026-09-19**: the band lies along the bottom of the GLASS, or
// there is none and the photograph is centred (ticket 64, after the owner looked at five samples).
// So most of what these tests assert is about the logical-to-physical mapping, which is the part that
// was wrong before: the band went wherever the leftover was, which put it down the panel's right edge
// for a 9:16 source and across its top for a turned 4:3 one.
//
// Every number here is arithmetic over a fit, so all of it is checkable without a panel. The one
// thing these tests CANNOT settle is whether a line is readable on glass at 3.9 mm of cap height --
// that is a person's judgement and nobody has made it.

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "epd_band.h"
#include "epd_canvas.h"
#include "epd_dither.h"

void setUp(void) {}
void tearDown(void) {}

// The panel's own dimensions. Rotation 0 is native portrait; rotation 1 makes the canvas logically
// landscape, which is what `auto_rotate` does to a landscape photograph.
#define PW 400
#define PH 600
static uint8_t buffer[PW * PH * 3];
static epd_canvas_t canvas;

static void make_canvas(uint8_t rotation)
{
    memset(buffer, 0, sizeof(buffer));
    TEST_ASSERT_TRUE(epd_canvas_init(&canvas, buffer, sizeof(buffer), PW, PH));
    epd_canvas_set_rotation(&canvas, rotation);
    epd_canvas_fill(&canvas, 255, 255, 255);
}

static void plan(uint8_t rotation, int32_t img_w, int32_t img_h, epd_fit_t *fit, epd_band_t *band)
{
    make_canvas(rotation);
    epd_band_plan(&canvas, img_w, img_h, fit, band);
}

// ------------------------------------------------------- the logical-to-physical probe

// The probe is the whole of the new rule, so it is tested directly and at both rotations rather
// than only through its consequences.
//
// The expected answers are derived from `epd_canvas_physical_rect()`'s own mapping at rotation 1 --
// `py = height - x - w`, so logical x=0 lands at the BOTTOM of the glass -- and they agree with what
// the hardware did on 2026-09-19: with the leftover at logical HIGH x the band appeared at the
// panel's top, so the bottom is logical LOW x, which is `EPD_ALIGN_HIGH` for the photograph.
static void test_the_probe_knows_which_logical_edge_is_the_glass_bottom(void)
{
    epd_align_t align = EPD_ALIGN_CENTRE;

    make_canvas(0);
    TEST_ASSERT_TRUE(epd_band_bottom_align(&canvas, false, &align));
    TEST_ASSERT_EQUAL_INT(EPD_ALIGN_LOW, align);
    // Logical x runs ACROSS the glass at rotation 0, so a leftover there cannot be a band.
    TEST_ASSERT_FALSE(epd_band_bottom_align(&canvas, true, &align));

    make_canvas(1);
    TEST_ASSERT_TRUE(epd_band_bottom_align(&canvas, true, &align));
    TEST_ASSERT_EQUAL_INT(EPD_ALIGN_HIGH, align);
    TEST_ASSERT_FALSE(epd_band_bottom_align(&canvas, false, &align));
}

static void test_the_probe_refuses_nonsense(void)
{
    epd_align_t align = EPD_ALIGN_CENTRE;
    make_canvas(0);
    TEST_ASSERT_FALSE(epd_band_bottom_align(NULL, false, &align));
    TEST_ASSERT_FALSE(epd_band_bottom_align(&canvas, false, NULL));
}

// ---------------------------------------------------------------------- the geometry

// The owner's first requirement, and the one thing here that must not change: the band never
// makes the photograph smaller. Asserted across every sample rather than argued once.
static void test_the_photograph_is_never_smaller_than_centred(void)
{
    const int32_t sources[][2] = {{400, 600}, {600, 800}, {700, 700}, {450, 800}, {256, 192},
                                  {1920, 1080}, {1000, 1000}};
    for (uint8_t rot = 0; rot <= 1; rot++) {
        for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
            epd_fit_t fit;
            epd_band_t band;
            plan(rot, sources[i][0], sources[i][1], &fit, &band);
            const epd_fit_t centred =
                epd_fit_centre(sources[i][0], sources[i][1], epd_canvas_logical_width(&canvas),
                               epd_canvas_logical_height(&canvas));
            TEST_ASSERT_EQUAL_INT32(centred.width, fit.width);
            TEST_ASSERT_EQUAL_INT32(centred.height, fit.height);
        }
    }
}

static void test_a_photograph_of_the_panels_ratio_gets_no_band(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 400, 600, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_NONE, band.kind);
    TEST_ASSERT_EQUAL_INT32(PW, fit.width);
    TEST_ASSERT_EQUAL_INT32(PH, fit.height);
}

// 3:4 on a portrait panel: the leftover is below the picture already, so the band is simply it.
static void test_a_three_to_four_source_bands_along_the_bottom(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_HORIZONTAL, band.kind);
    TEST_ASSERT_EQUAL_INT32(0, fit.y); // photograph pushed to the glass's top
    TEST_ASSERT_EQUAL_INT32(533, fit.height);
    TEST_ASSERT_EQUAL_INT32(0, band.x);
    TEST_ASSERT_EQUAL_INT32(533, band.y);
    TEST_ASSERT_EQUAL_INT32(PW, band.width);
    TEST_ASSERT_EQUAL_INT32(67, band.height);
    TEST_ASSERT_TRUE(band.photo_at_low);
}

static void test_a_square_source_bands_generously_along_the_bottom(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 700, 700, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_HORIZONTAL, band.kind);
    TEST_ASSERT_EQUAL_INT32(400, band.y);
    TEST_ASSERT_EQUAL_INT32(200, band.height);
}

// **The case the owner rejected.** A 9:16 source on a portrait panel leaves its blank at the
// SIDES, which cannot become a band along the bottom -- so there is none and the photograph stays
// centred, blank on both sides, exactly as it was before any of this existed.
static void test_a_nine_to_sixteen_source_gets_no_band_and_stays_centred(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 450, 800, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_NONE, band.kind);
    TEST_ASSERT_EQUAL_INT32(337, fit.width);
    TEST_ASSERT_EQUAL_INT32(600, fit.height);
    // Centred, not pushed: 63 px of slack split in two.
    TEST_ASSERT_EQUAL_INT32(31, fit.x);
}

// **The other case the owner rejected, and the one that proves the probe earns its keep.** A 4:3
// source turns the canvas, so its leftover is on the LOGICAL x axis -- which at rotation 1 runs down
// the glass. The band is therefore possible, and it has to land at the glass's bottom rather than its
// top, which is where passing the alignment through in logical terms used to put it.
static void test_a_turned_source_bands_along_the_glass_bottom_not_the_top(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(1, 256, 192, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_VERTICAL, band.kind);
    TEST_ASSERT_EQUAL_INT32(533, fit.width);
    TEST_ASSERT_EQUAL_INT32(400, fit.height);
    TEST_ASSERT_EQUAL_INT32(67, fit.x); // photograph at the logical HIGH end
    TEST_ASSERT_EQUAL_INT32(0, band.x);
    TEST_ASSERT_EQUAL_INT32(67, band.width);
    TEST_ASSERT_FALSE(band.photo_at_low);

    // And the assertion that actually says "the bottom of the glass": where the logical rect lands
    // physically. 400x67 at the foot of a 400x600 panel, indistinguishable from the 3:4 case above.
    int32_t px = 0, py = 0, pw = 0, ph = 0;
    TEST_ASSERT_TRUE(epd_canvas_physical_rect(&canvas, band.x, band.y, band.width, band.height, &px,
                                              &py, &pw, &ph));
    TEST_ASSERT_EQUAL_INT32(0, px);
    TEST_ASSERT_EQUAL_INT32(533, py);
    TEST_ASSERT_EQUAL_INT32(400, pw);
    TEST_ASSERT_EQUAL_INT32(67, ph);
}

static void test_a_bad_plan_yields_no_band(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 0, 0, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_NONE, band.kind);

    epd_band_plan(NULL, 600, 800, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_NONE, band.kind);
    epd_band_plan(&canvas, 600, 800, NULL, &band); // must not write through a NULL
}

// ------------------------------------------------------------- capacity and the scale

// The thin-side rule (owner, 2026-09-19): a band too thin for scale 4 gets smaller text rather
// than nothing. Scale 4 is the ceiling and scale 2 the floor -- 7 px is about 1 mm here.
static void test_the_large_scale_follows_a_thin_band_and_caps_at_four(void)
{
    epd_band_t b;
    memset(&b, 0, sizeof(b));
    b.kind = EPD_BAND_HORIZONTAL;
    b.width = 400;

    const int32_t cases[][2] = {{22, 2}, {29, 3}, {30, 3}, {36, 4}, {67, 4}, {200, 4}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        b.height = cases[i][0];
        TEST_ASSERT_EQUAL_INT32(cases[i][1], epd_band_large_scale(&b));
    }

    b.kind = EPD_BAND_NONE;
    TEST_ASSERT_EQUAL_INT32(0, epd_band_large_scale(&b));
}

// 22 px is the shortest band there can be, and it follows from the scale floor rather than being a
// number of its own. A source whose leftover is one pixel short of it gets no band at all.
static void test_a_band_under_the_floor_is_no_band(void)
{
    TEST_ASSERT_EQUAL_INT32(22, EPD_BAND_MIN_SHORT);

    // 400x580 drawn on 400x600 leaves 20 px, under the floor.
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 400, 580, &fit, &band);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_NONE, band.kind);
    TEST_ASSERT_EQUAL_INT32(10, fit.y); // centred, 10 px either side
}

// Inverted from epd_text_width()'s own rule, so the two agree AT the boundary: 16 glyphs at
// scale 4 are 16*24-4 = 380 <= 400 and 17 would be 404.
static void test_line_capacity_matches_the_text_metrics(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    TEST_ASSERT_EQUAL_INT32(16, epd_band_line_chars(&band, 4));
    TEST_ASSERT_EQUAL_INT32(33, epd_band_line_chars(&band, 2));
    TEST_ASSERT_LESS_OR_EQUAL_INT32(band.width, epd_text_width("0123456789ABCDEF", 4));
    TEST_ASSERT_GREATER_THAN_INT32(band.width, epd_text_width("0123456789ABCDEFG", 4));

    // A vertical band's long axis is its height, which is where a turned line runs.
    plan(1, 256, 192, &fit, &band);
    TEST_ASSERT_EQUAL_INT32(EPD_BAND_VERTICAL, band.kind);
    TEST_ASSERT_EQUAL_INT32(16, epd_band_line_chars(&band, 4));
}

// ------------------------------------------------------------------------ the layout

static epd_band_content_t full_content(void)
{
    epd_band_content_t c;
    memset(&c, 0, sizeof(c));
    c.city = "TOKYO";
    c.country = "JAPAN";
    c.taken = "19-08-14";
    c.ago = "7 YEARS AGO";
    c.now = "09-19";
    // The format app_display.c produces since 2026-09-19: no decimal place, and the two glyphs
    // that were added to the font for it. A test that still said `23.4C RH45` would be measuring
    // capacities the frame never asks for.
    c.climate = "23" EPD_TEXT_DEGREE "C 45%";
    c.battery_pct = 87;
    return c;
}

// A band built to an exact geometry, for the cases no source ratio on either panel produces. The
// plan() helper above is the right tool for "what does a 3:4 photograph do"; this one is for "what
// does a 500x36 band do", which is a question about the layout and not about any panel.
static epd_band_t mk_band(epd_band_kind_t kind, int32_t lng, int32_t shrt, bool photo_at_low)
{
    epd_band_t b;
    memset(&b, 0, sizeof(b));
    b.kind = kind;
    b.photo_at_low = photo_at_low;
    if (kind == EPD_BAND_VERTICAL) {
        b.width = shrt;
        b.height = lng;
    } else {
        b.width = lng;
        b.height = shrt;
    }
    return b;
}

// **The M5Paper Color's band is 400 px long whichever way the canvas is turned**, and at scale 4 it
// holds sixteen characters. `TOKYO JAPAN 19-08-14` is twenty, so the country goes -- and the DATE
// stays, which is the point of dropping the country rather than truncating the string.
static void test_a_four_hundred_pixel_band_drops_the_country_and_keeps_the_date(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    const epd_band_content_t c = full_content();
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_INT32(1, l.count);
    TEST_ASSERT_EQUAL_INT32(4, l.scale);
    TEST_ASSERT_EQUAL_STRING("TOKYO 19-08-14", l.segments[0].text);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_SEG_TEXT, l.segments[0].kind);
}

// **A thicker band shows exactly the same line.** That is the owner's decision of 2026-09-19 --
// one line always, and a band with room to spare keeps its white rather than growing the type --
// and it is pinned here because it is the surprising half: a 200 px band and a 67 px band produce
// identical content, so anyone reading a generous band as under-filled is reading the decision.
static void test_a_thick_band_shows_the_same_single_line(void)
{
    epd_fit_t fit;
    epd_band_t thin;
    epd_band_t thick;
    plan(0, 600, 800, &fit, &thin);   // 400x67
    plan(0, 700, 700, &fit, &thick);  // 400x200
    TEST_ASSERT_EQUAL_INT32(67, thin.height);
    TEST_ASSERT_EQUAL_INT32(200, thick.height);

    const epd_band_content_t c = full_content();
    epd_band_layout_t a;
    epd_band_layout_t b;
    epd_band_layout(&thin, &c, &a);
    epd_band_layout(&thick, &c, &b);

    TEST_ASSERT_EQUAL_INT32(a.count, b.count);
    TEST_ASSERT_EQUAL_INT32(a.scale, b.scale);
    TEST_ASSERT_EQUAL_STRING(a.segments[0].text, b.segments[0].text);
}

// The reTerminal E1002's only band: 800 px at scale 3 holds the photograph, the room, the icon and
// the elapsed years at once. This is the arm that says the one-line design is not a reduction
// everywhere -- on this board it carries MORE than the stacked version's 30 px band could, which
// showed one line and dropped the room and the battery for want of a second.
//
// **Until 2026-09-22 this band held the country and not `ago`** (`TOKYO JAPAN 19-08-14`, three
// segments). The battery icon then shrank to two thirds of the line at the owner's request, the
// length it gave back admits `ago`, and `ago` outranks the country, which is spliced last and only
// into what is left. The owner chose the years over the country when asked.
static void test_an_eight_hundred_pixel_band_carries_all_four_segments(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 800, 30, true);
    const epd_band_content_t c = full_content();
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_INT32(4, l.count);
    TEST_ASSERT_EQUAL_INT32(3, l.scale);
    TEST_ASSERT_EQUAL_STRING("TOKYO 19-08-14", l.segments[0].text);
    TEST_ASSERT_EQUAL_STRING("09-19 23" EPD_TEXT_DEGREE "C 45%", l.segments[1].text);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_SEG_BATTERY, l.segments[2].kind);
    TEST_ASSERT_EQUAL_INT32(87, l.segments[2].battery_pct);
    TEST_ASSERT_EQUAL_STRING("", l.segments[2].text);
    TEST_ASSERT_EQUAL_STRING("7 YEARS AGO", l.segments[3].text);
}

// The splice is refused when the LINE cannot take the longer string, not when the segment cannot --
// which is the whole reason it happens last and re-measures the total.
static void test_the_country_is_spliced_only_when_the_whole_line_has_room(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 800, 30, true);
    epd_band_content_t c = full_content();
    c.city = "RIO DE JANEIRO";
    c.country = "BRAZIL";
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    // The city and the date still lead, and BRAZIL did not get on: `RIO DE JANEIRO BRAZIL 19-08-14`
    // is thirty characters and the three segments already placed leave room for twenty-three.
    TEST_ASSERT_EQUAL_STRING("RIO DE JANEIRO 19-08-14", l.segments[0].text);
    TEST_ASSERT_EQUAL_INT32(3, l.count);
}

// **A long city name is CUT to fit and the excess discarded** (owner, 2026-09-20). It used to be
// refused, which on a 400 px band lost the city and the capture date together to a name of fourteen
// characters -- the band fell through to the room and the photograph said nothing at all.
static void test_a_long_city_name_is_cut_to_fit(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);   // 400x67, scale 4, 392 px usable
    epd_band_content_t c = full_content();
    c.city = "RIO DE JANEIRO";
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    // `RIO DE JANEIRO 19-08-14` is 548 px at scale 4; `RIO DE 19-08-14` is 356 and fits.
    TEST_ASSERT_EQUAL_STRING("RIO DE 19-08-14", l.segments[0].text);
}

// **The DATE is never the thing cut.** Half of `19-08-14` is unreadable where half a place name is
// merely a shorter word, so every candidate prefix is of the city and the date is appended whole.
static void test_the_capture_date_survives_any_city_name(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    static const char *names[] = {
        "RIO DE JANEIRO", "SAN FRANCISCO", "STRATFORD UPON AVON",
        "LLANFAIRPWLLGWYNGYLL", "A",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        epd_band_content_t c = full_content();
        c.city = names[i];
        epd_band_layout_t l;
        epd_band_layout(&band, &c, &l);
        TEST_ASSERT_GREATER_THAN_INT32(0, l.count);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(l.segments[0].text, "19-08-14"), names[i]);
        // And what is drawn really does fit, which is the claim the cut exists to make.
        TEST_ASSERT_LESS_OR_EQUAL_INT32(band.width - EPD_BAND_EDGE * 2,
                                        epd_text_width(l.segments[0].text, l.scale));
    }
}

// A cut must not land on a space: a trailing one draws as a gap before the date and reads as a
// missing word rather than a shortened one. `STRATFORD UPON AVON` cut to eleven characters would be
// `STRATFORD U`, to ten `STRATFORD ` -- the second is the one to avoid.
static void test_a_cut_never_leaves_a_trailing_space(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    static const char *names[] = {"STRATFORD UPON AVON", "RIO DE JANEIRO", "SAO TOME AND PRINCIPE"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        epd_band_content_t c = full_content();
        c.city = names[i];
        epd_band_layout_t l;
        epd_band_layout(&band, &c, &l);
        const char *t = l.segments[0].text;
        TEST_ASSERT_NULL_MESSAGE(strstr(t, "  "), names[i]);
    }
}

// A band so short that not one character of the city fits beside the date. The date alone is still
// the photograph saying something, which is the point of cutting rather than refusing.
static void test_a_band_with_room_for_the_date_alone_shows_the_date(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 200, 40, true);
    epd_band_content_t c = full_content();
    c.city = "RIO DE JANEIRO";
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_STRING("19-08-14", l.segments[0].text);
}

// The 800 px band never reaches the cut, and that is worth pinning: a rule that fired everywhere
// would be silently shortening names on the board that has room for them.
static void test_a_long_name_is_not_cut_where_it_fits(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 800, 30, true);
    epd_band_content_t c = full_content();
    c.city = "RIO DE JANEIRO";
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_STRING("RIO DE JANEIRO 19-08-14", l.segments[0].text);
}

// The room has its own two rungs, and this is the band that exercises the second: `09-19 23C 45%`
// will not fit beside the photograph and `09-19` will.
static void test_the_room_degrades_to_the_date_alone(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 500, 36, true);
    const epd_band_content_t c = full_content();
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_INT32(2, l.count);
    TEST_ASSERT_EQUAL_STRING("TOKYO 19-08-14", l.segments[0].text);
    TEST_ASSERT_EQUAL_STRING("09-19", l.segments[1].text);
}

// Most of an existing library has no EXIF, so this is the common case until the import-time read has
// been over the card. **Nothing is "promoted" any more** -- with one line there is no larger line to
// promote into, and the room simply becomes the first segment.
static void test_with_no_photograph_metadata_the_room_leads(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    epd_band_content_t c = full_content();
    c.city = NULL;
    c.country = NULL;
    c.taken = NULL;
    c.ago = "";
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_INT32(1, l.count);
    TEST_ASSERT_EQUAL_STRING("09-19 23" EPD_TEXT_DEGREE "C 45%", l.segments[0].text);
    TEST_ASSERT_EQUAL_INT32(4, l.scale);
}

// A country with no city is meaningless -- geo_city_nearest() returns both or neither -- and must
// not become a segment on its own.
static void test_a_country_without_a_city_is_not_spliced(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 800, 30, true);
    epd_band_content_t c = full_content();
    c.city = NULL;
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    for (int32_t i = 0; i < l.count; i++) {
        TEST_ASSERT_NULL(strstr(l.segments[i].text, "JAPAN"));
    }
}

static void test_an_unknown_battery_draws_no_icon(void)
{
    const epd_band_t band = mk_band(EPD_BAND_HORIZONTAL, 800, 30, true);
    epd_band_content_t c = full_content();
    // Negative is "this board cannot read its cell", which is not the same as an empty one.
    c.battery_pct = -1;
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    for (int32_t i = 0; i < l.count; i++) {
        TEST_ASSERT_EQUAL_INT(EPD_BAND_SEG_TEXT, l.segments[i].kind);
    }
}

static void test_a_fault_owns_the_band_alone(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(0, 700, 700, &fit, &band);
    epd_band_content_t c = full_content();
    c.fault = "SYNC FAILED";
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);

    TEST_ASSERT_EQUAL_INT32(1, l.count);
    TEST_ASSERT_EQUAL_STRING("SYNC FAILED", l.segments[0].text);
}

static void test_no_band_and_no_content_lay_out_nothing(void)
{
    epd_fit_t fit;
    epd_band_t none;
    plan(0, 450, 800, &fit, &none);
    const epd_band_content_t c = full_content();
    epd_band_layout_t l;
    epd_band_layout(&none, &c, &l);
    TEST_ASSERT_EQUAL_INT32(0, l.count);

    epd_band_t band;
    plan(0, 600, 800, &fit, &band);
    epd_band_content_t empty;
    memset(&empty, 0, sizeof(empty));
    empty.battery_pct = -1;
    epd_band_layout(&band, &empty, &l);
    TEST_ASSERT_EQUAL_INT32(0, l.count);

    epd_band_layout(NULL, &c, &l);
    TEST_ASSERT_EQUAL_INT32(0, l.count);
}

// -------------------------------------------------------------------------- the draw

static int ink_in(int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    int n = 0;
    for (int32_t y = y0; y < y0 + h; y++) {
        for (int32_t x = x0; x < x0 + w; x++) {
            const uint8_t *p = epd_canvas_pixel(&canvas, x, y);
            TEST_ASSERT_NOT_NULL(p);
            if (p[0] == 0 && p[1] == 0 && p[2] == 0) {
                n++;
            }
        }
    }
    return n;
}

// The assertion that matters: ink lands in the band and NOWHERE in the photograph. Everything
// else about the band is cosmetic; drawing over the picture is the one failure that would make the
// feature worse than nothing. Run at both rotations, because the two are different arithmetic.
static void test_the_draw_never_touches_the_photograph(void)
{
    const int32_t sources[][3] = {{600, 800, 0}, {700, 700, 0}, {256, 192, 1}};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        epd_fit_t fit;
        epd_band_t band;
        plan((uint8_t)sources[i][2], sources[i][0], sources[i][1], &fit, &band);
        TEST_ASSERT_NOT_EQUAL(EPD_BAND_NONE, band.kind);

        const epd_band_content_t c = full_content();
        epd_band_layout_t l;
        epd_band_layout(&band, &c, &l);
        epd_band_draw(&canvas, &band, &l);

        TEST_ASSERT_GREATER_THAN_INT(0, ink_in(band.x, band.y, band.width, band.height));
        TEST_ASSERT_EQUAL_INT(0, ink_in(fit.x, fit.y, fit.width, fit.height));
    }
}

// The icon is drawn through the same band-space mapping as the glyphs, so it turns with them. A
// turned icon drawn with the upright mapping would land outside a 67 px band, which is what this
// catches -- and it must be ink, not an empty rectangle.
static void test_the_battery_icon_draws_inside_a_turned_band(void)
{
    epd_fit_t fit;
    epd_band_t band;
    plan(1, 256, 192, &fit, &band);

    // Everything else silenced, because a 400 px band at scale 4 does NOT have room for the
    // photograph's segment AND the icon -- the icon is 40 px plus a 24 px gap against 60 px left
    // over. Leaving the room's line in would have made this a test of the layout declining to
    // place an icon, dressed up as a test of drawing one.
    epd_band_content_t c = full_content();
    c.city = NULL;
    c.country = NULL;
    c.taken = NULL;
    c.ago = NULL;
    c.now = NULL;
    c.climate = NULL;
    c.battery_pct = 100;
    epd_band_layout_t l;
    epd_band_layout(&band, &c, &l);
    TEST_ASSERT_EQUAL_INT32(1, l.count);
    TEST_ASSERT_EQUAL_INT(EPD_BAND_SEG_BATTERY, l.segments[l.count - 1].kind);

    epd_band_draw(&canvas, &band, &l);
    TEST_ASSERT_GREATER_THAN_INT(0, ink_in(band.x, band.y, band.width, band.height));
    TEST_ASSERT_EQUAL_INT(0, ink_in(fit.x, fit.y, fit.width, fit.height));
}

// A full battery must have more ink than an empty one, and an empty one must still have its
// outline: the fill IS the reading, so "0 %" and "no reading" have to look different.
static void test_the_icon_fill_follows_the_charge(void)
{
    epd_fit_t fit;
    epd_band_t band;
    int counts[3] = {0, 0, 0};
    const int pcts[3] = {0, 50, 100};

    for (int i = 0; i < 3; i++) {
        plan(0, 600, 800, &fit, &band);
        epd_band_content_t c = full_content();
        c.city = NULL;
        c.taken = NULL;
        c.ago = NULL;
        c.now = NULL;
        c.climate = NULL;
        c.battery_pct = pcts[i];
        epd_band_layout_t l;
        epd_band_layout(&band, &c, &l);
        TEST_ASSERT_EQUAL_INT32(1, l.count);
        epd_band_draw(&canvas, &band, &l);
        counts[i] = ink_in(band.x, band.y, band.width, band.height);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, counts[0]);           // an outline at 0 %
    TEST_ASSERT_GREATER_THAN_INT(counts[0], counts[1]);   // more at half
    TEST_ASSERT_GREATER_THAN_INT(counts[1], counts[2]);   // more again at full
}

static int red_in(int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    int n = 0;
    for (int32_t y = y0; y < y0 + h; y++) {
        for (int32_t x = x0; x < x0 + w; x++) {
            const uint8_t *p = epd_canvas_pixel(&canvas, x, y);
            TEST_ASSERT_NOT_NULL(p);
            if (p[0] == 255 && p[1] == 0 && p[2] == 0) {
                n++;
            }
        }
    }
    return n;
}

// Red at a third or less, black above it -- the boundary on both sides, since that is where a
// `<` against `<=` would hide.
static void test_the_icon_fill_is_red_at_a_third_or_less(void)
{
    const int pcts[4] = {10, EPD_BAND_BATTERY_LOW_PCT, EPD_BAND_BATTERY_LOW_PCT + 1, 100};
    const bool red[4] = {true, true, false, false};
    for (int i = 0; i < 4; i++) {
        epd_fit_t fit;
        epd_band_t band;
        plan(0, 600, 800, &fit, &band);
        epd_band_content_t c = full_content();
        c.city = NULL;
        c.taken = NULL;
        c.ago = NULL;
        c.now = NULL;
        c.climate = NULL;
        c.battery_pct = pcts[i];
        epd_band_layout_t l;
        epd_band_layout(&band, &c, &l);
        TEST_ASSERT_EQUAL_INT32(1, l.count);
        epd_band_draw(&canvas, &band, &l);
        const int n = red_in(band.x, band.y, band.width, band.height);
        if (red[i]) {
            TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, n, "expected a red fill");
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, "expected no red");
        }
    }
}

// The red fill is drawn before the quantise, so it is only red on the glass if the quantiser maps
// pure red to the red ink. It does on every palette for the two NEAREST passes -- the exact nearest
// that `dither_diffuse` (the default) runs outside the region, and the Nearest row path.
//
// **Not for the ordered pair search**, and that is measured rather than assumed: with
// `dither_diffuse` off, `spectra6` turns some of a flat red into red-yellow pairs, and a sweep of
// reds from 100 to 255 with 0-40 in green and blue found NO value that stays pure red on every
// palette at every bias phase. That is the same trade the band's black text already makes on that
// path (see app_display.c), so it is recorded here rather than worked around.
static void test_pure_red_quantises_to_the_red_ink_on_every_palette(void)
{
    enum { W = 64 };
    uint8_t row[W * 3];
    uint8_t packed[W / 2];
    for (int i = 0; i < W; i++) {
        row[i * 3] = 255;
        row[i * 3 + 1] = 0;
        row[i * 3 + 2] = 0;
    }
    for (int id = 0; id < EPD_PALETTE_ID_COUNT; id++) {
        const epd_render_t cfg = epd_render_for_palette((epd_palette_id_t)id);
        TEST_ASSERT_EQUAL_HEX8(0x3, epd_nearest_index_cfg(cfg.palette, 255, 0, 0));
        epd_dither_row_none_cfg(row, packed, W, &cfg);
        for (int i = 0; i < W / 2; i++) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x33, packed[i], epd_palette_id_name(id));
        }
    }
}

static void test_drawing_nothing_is_safe(void)
{
    epd_fit_t fit;
    epd_band_t none;
    plan(0, 450, 800, &fit, &none);
    epd_band_layout_t l;
    memset(&l, 0, sizeof(l));

    epd_band_draw(&canvas, &none, &l);
    epd_band_draw(NULL, &none, &l);
    epd_band_draw(&canvas, NULL, &l);
    epd_band_draw(&canvas, &none, NULL);
    TEST_ASSERT_EQUAL_INT(0, ink_in(0, 0, epd_canvas_logical_width(&canvas),
                                    epd_canvas_logical_height(&canvas)));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_the_probe_knows_which_logical_edge_is_the_glass_bottom);
    RUN_TEST(test_the_probe_refuses_nonsense);

    RUN_TEST(test_the_photograph_is_never_smaller_than_centred);
    RUN_TEST(test_a_photograph_of_the_panels_ratio_gets_no_band);
    RUN_TEST(test_a_three_to_four_source_bands_along_the_bottom);
    RUN_TEST(test_a_square_source_bands_generously_along_the_bottom);
    RUN_TEST(test_a_nine_to_sixteen_source_gets_no_band_and_stays_centred);
    RUN_TEST(test_a_turned_source_bands_along_the_glass_bottom_not_the_top);
    RUN_TEST(test_a_bad_plan_yields_no_band);

    RUN_TEST(test_the_large_scale_follows_a_thin_band_and_caps_at_four);
    RUN_TEST(test_a_band_under_the_floor_is_no_band);
    RUN_TEST(test_line_capacity_matches_the_text_metrics);

    RUN_TEST(test_a_four_hundred_pixel_band_drops_the_country_and_keeps_the_date);
    RUN_TEST(test_a_thick_band_shows_the_same_single_line);
    RUN_TEST(test_an_eight_hundred_pixel_band_carries_all_four_segments);
    RUN_TEST(test_the_country_is_spliced_only_when_the_whole_line_has_room);
    RUN_TEST(test_a_long_city_name_is_cut_to_fit);
    RUN_TEST(test_the_capture_date_survives_any_city_name);
    RUN_TEST(test_a_cut_never_leaves_a_trailing_space);
    RUN_TEST(test_a_band_with_room_for_the_date_alone_shows_the_date);
    RUN_TEST(test_a_long_name_is_not_cut_where_it_fits);
    RUN_TEST(test_the_room_degrades_to_the_date_alone);
    RUN_TEST(test_with_no_photograph_metadata_the_room_leads);
    RUN_TEST(test_a_country_without_a_city_is_not_spliced);
    RUN_TEST(test_an_unknown_battery_draws_no_icon);
    RUN_TEST(test_a_fault_owns_the_band_alone);
    RUN_TEST(test_no_band_and_no_content_lay_out_nothing);

    RUN_TEST(test_the_draw_never_touches_the_photograph);
    RUN_TEST(test_the_battery_icon_draws_inside_a_turned_band);
    RUN_TEST(test_the_icon_fill_is_red_at_a_third_or_less);
    RUN_TEST(test_pure_red_quantises_to_the_red_ink_on_every_palette);    RUN_TEST(test_the_icon_fill_follows_the_charge);
    RUN_TEST(test_drawing_nothing_is_safe);

    return UNITY_END();
}
