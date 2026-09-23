#include "epd_text.h"

#include <string.h>

// The glyphs, five bits per row, bit 4 leftmost. Hand-drawn (see the header on why they are not
// somebody else's), and laid out in four contiguous blocks so glyph_index() is arithmetic rather
// than a 47-case switch:
//
//   0        space
//   1 - 4    . - : /
//   5 - 14   0-9
//   15 - 40  A-Z
//   41 - 46  a-f
//   47 - 48  degree (byte 0xB0) and %
//
// The pictures in the comments are the table: if one of them and its hex disagree, the hex is
// what draws and the picture is the bug. test/test_epd_text dumps every glyph for exactly that
// reason -- an ASCII grid of 49 characters is readable in one screen, and a transposed bit is
// invisible in the hex.
//
// **The last two are appended rather than inserted in ASCII order**, which the blocks above are
// not either: the point of the layout is that glyph_index() is arithmetic per block, and `%`
// (0x25) sits between `/` and `0` in ASCII where inserting it would move 44 indices for nothing.

#define G_SPACE 0
#define G_PUNCT 1  /* '.' */
#define G_DIGIT 5
#define G_UPPER 15
#define G_LOWER 41
#define G_EXTRA 47 /* degree, then '%' */
#define GLYPH_COUNT 49

static const uint8_t GLYPHS[GLYPH_COUNT][EPD_TEXT_GLYPH_H] = {
    /* ' ' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* '.' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    /* '-' */ {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00},
    /* ':' */ {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},
    /* '/' */ {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},

    /* '0'  .###.  #...#  #..##  #.#.#  ##..#  #...#  .###.  */
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    /* '1' */ {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* '2' */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    // '3' is the rounded form, not the flat-topped one: with a full top row it and '7' share
    // their first four rows, and at four pixels a module on a dithered panel that is the pair
    // most likely to be read wrongly. Proof-read off the dump in test_epd_text.
    /* '3' */ {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    /* '4' */ {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* '5' */ {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    /* '6' */ {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    /* '7' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* '8' */ {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    /* '9' */ {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},

    /* 'A'  ..#..  .#.#.  #...#  #...#  #####  #...#  #...#  */
    {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11},
    /* 'B' */ {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    /* 'C' */ {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    /* 'D' */ {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    /* 'E' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    /* 'F' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    /* 'G' */ {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E},
    /* 'H' */ {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* 'I' */ {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 'J' */ {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    /* 'K' */ {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    /* 'L' */ {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    /* 'M' */ {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    /* 'N' */ {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11},
    /* 'O' */ {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* 'P' */ {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    /* 'Q' */ {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    /* 'R' */ {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    /* 'S' */ {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    /* 'T' */ {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* 'U' */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* 'V' */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    /* 'W' */ {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},
    /* 'X' */ {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    /* 'Y' */ {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    /* 'Z' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},

    // a-f only. Added for a 16-lowercase-hex access-point password, which is no longer what the
    // card shows (see the header: it is 8 uppercase Crockford base32 characters since 2026-09-19),
    // and kept for the reason given there. Each is drawn to be unmistakable against its capital at
    // three or four pixels a module: 'a', 'c' and 'e' sit two rows lower, 'b', 'd' and 'f'
    // keep an ascender the capital does not have.
    /* 'a' */ {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
    /* 'b' */ {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},
    /* 'c' */ {0x00, 0x00, 0x0F, 0x10, 0x10, 0x10, 0x0F},
    /* 'd' */ {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
    /* 'e' */ {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    /* 'f' */ {0x06, 0x08, 0x08, 0x1C, 0x08, 0x08, 0x08},

    // Added 2026-09-19 for the matte band's room line, which the owner asked to read
    // `25°C 51%` rather than `25C RH51` (ticket 64). Both are uppercase-neutral, so nothing
    // about the connect card changes.
    //
    /* 0xB0  .###.  .#.#.  .###.  .....  .....  .....  .....  */
    // A 3x3 ring in the TOP three rows, which is what makes it a degree sign rather than a
    // small 'o': at scale 3 that is a 9 px ring in the top 9 px of a 21 px line.
    {0x0E, 0x0A, 0x0E, 0x00, 0x00, 0x00, 0x00},
    /* '%'  ##...  ##..#  ...#.  ..#..  .#...  #..##  ...##  */
    // Two 2x2 blocks and one unbroken diagonal between them -- row 1 col 4, row 2 col 3, row 3
    // col 2, row 4 col 1, row 5 col 0. A gap anywhere in that run reads as two unrelated dots.
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},
};

// Drawn for a byte with no glyph of its own. Hollow rather than solid so it reads as "a
// character is missing here" rather than as ink.
static const uint8_t GLYPH_UNKNOWN[EPD_TEXT_GLYPH_H] = {0x1F, 0x11, 0x11, 0x11,
                                                        0x11, 0x11, 0x1F};

static int glyph_index(char ch)
{
    // **Through `unsigned char`, and that is load-bearing now rather than tidiness.** The degree
    // sign is byte 0xB0 (EPD_TEXT_DEGREE), and `char` is signed on this target, so comparing
    // `ch == '\xB0'` relies on two implementation-defined conversions agreeing. 176 does not.
    const unsigned char u = (unsigned char)ch;

    if (u == ' ') {
        return G_SPACE;
    }
    if (u == '.') {
        return G_PUNCT + 0;
    }
    if (u == '-') {
        return G_PUNCT + 1;
    }
    if (u == ':') {
        return G_PUNCT + 2;
    }
    if (u == '/') {
        return G_PUNCT + 3;
    }
    if (u >= '0' && u <= '9') {
        return G_DIGIT + (u - '0');
    }
    if (u >= 'A' && u <= 'Z') {
        return G_UPPER + (u - 'A');
    }
    if (u >= 'a' && u <= 'f') {
        return G_LOWER + (u - 'a');
    }
    if (u == 0xB0u) {
        return G_EXTRA + 0;
    }
    if (u == '%') {
        return G_EXTRA + 1;
    }
    return -1;
}

bool epd_text_has_glyph(char ch)
{
    return glyph_index(ch) >= 0;
}

void epd_text_glyph_rows(char ch, uint8_t out[EPD_TEXT_GLYPH_H])
{
    if (out == NULL) {
        return;
    }
    const int idx = glyph_index(ch);
    memcpy(out, idx >= 0 ? GLYPHS[idx] : GLYPH_UNKNOWN, EPD_TEXT_GLYPH_H);
}

int32_t epd_text_width(const char *s, int32_t scale)
{
    if (s == NULL || scale <= 0) {
        return 0;
    }
    const int32_t n = (int32_t)strlen(s);
    if (n <= 0) {
        return 0;
    }
    // The trailing inter-glyph gap is not ink, so it is not width: counting it would push
    // every centred line one column right, per line, invisibly.
    return n * EPD_TEXT_ADVANCE * scale - scale;
}

int32_t epd_text_height(int32_t scale)
{
    return scale > 0 ? EPD_TEXT_GLYPH_H * scale : 0;
}

// The upright and the turned draw differ only in where a glyph pixel lands, so the loop is
// written once and the mapping chosen inside it. Two copies would drift: a fix made to the
// connect card's draw and not to the matte band's would stay invisible until both were on the
// same panel.
static void draw_string(epd_canvas_t *c, int32_t x, int32_t y, int32_t scale, const char *s,
                        bool rot90, uint8_t r, uint8_t g, uint8_t b)
{
    if (c == NULL || s == NULL || scale <= 0) {
        return;
    }

    int32_t along = 0;
    for (const char *p = s; *p; p++, along += EPD_TEXT_ADVANCE * scale) {
        const int idx = glyph_index(*p);
        const uint8_t *rows = idx >= 0 ? GLYPHS[idx] : GLYPH_UNKNOWN;

        for (int32_t gy = 0; gy < EPD_TEXT_GLYPH_H; gy++) {
            const uint8_t bits = rows[gy];
            if (bits == 0) {
                continue;
            }
            for (int32_t gx = 0; gx < EPD_TEXT_GLYPH_W; gx++) {
                if (!(bits & (uint8_t)(1u << (EPD_TEXT_GLYPH_W - 1 - gx)))) {
                    continue;
                }
                // Clockwise: the line's own axis becomes +y, and a glyph's rows run towards
                // -x so that row 0 -- the top of the letter -- ends up at the largest x.
                const int32_t px =
                    rot90 ? x + (EPD_TEXT_GLYPH_H - 1 - gy) * scale : x + along + gx * scale;
                const int32_t py = rot90 ? y + along + gx * scale : y + gy * scale;

                // One rect per pixel, which is what the QR renderer does with the same
                // primitive. A row-run coalescer would halve the calls and save
                // microseconds against a 15 s refresh.
                epd_canvas_fill_rect(c, px, py, scale, scale, r, g, b);
            }
        }
    }
}

void epd_text_draw(epd_canvas_t *c, int32_t x, int32_t y, int32_t scale, const char *s,
                   uint8_t r, uint8_t g, uint8_t b)
{
    draw_string(c, x, y, scale, s, false, r, g, b);
}

void epd_text_draw_rot90(epd_canvas_t *c, int32_t x, int32_t y, int32_t scale, const char *s,
                         uint8_t r, uint8_t g, uint8_t b)
{
    draw_string(c, x, y, scale, s, true, r, g, b);
}
