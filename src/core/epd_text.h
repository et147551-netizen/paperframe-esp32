// A 5x7 bitmap font, drawn onto the canvas with the one primitive it already has.
//
// This exists because the panel had no text on it at all. The pairing screen was two QR codes
// and a count of filled squares beside each, which a phone can read and a person cannot: an
// operator on a PC has no camera, so the access point's password, the frame's address and the
// pairing code were all unreachable (ticket 66). `epd_canvas` has fills and blits and no
// glyphs, and ticket 64's clock option is blocked on the same absence.
//
// **The glyph table is hand-drawn rather than taken from anywhere.** Ticket 65 is about this
// project no longer carrying somebody else's text, and a font is exactly the kind of table that
// arrives with a licence attached. 49 glyphs is an afternoon; the host test's dump is how it
// gets proof-read.
//
// **The set is deliberately partial**: space, `. - : /`, `0-9`, `A-Z`, `a-f`, and -- since
// 2026-09-19 -- the degree sign and `%`. Everything the connect card puts on the glass is
// uppercase; DNS names and URL schemes are case-insensitive and uppercase survives the dither
// better.
//
// **The degree sign is BYTE 0xB0, not a literal `°` in the source.** The font indexes by byte, and
// a `°` typed into a UTF-8 source file is TWO bytes (0xC2 0xB0) -- which would draw a hollow box
// followed by a degree sign. Use EPD_TEXT_DEGREE below rather than writing the escape by hand, and
// note that `glyph_index()` goes through `unsigned char` so the comparison does not depend on
// `char`'s signedness.
//
// **`a-f` was added for the access point's password and the card no longer needs it.** That password
// was 16 lowercase hex characters, case-sensitive to the letter because WPA2 derives its PMK from
// the passphrase bytes; on 2026-09-19 the operator asked for the shortest typeable password and it
// became 8 uppercase Crockford base32 characters, so **nothing on the card is lowercase any more.**
// The six glyphs stay: they are drawn, proof-read and tested, they cost 42 bytes of rodata, and a
// caption or a clock in the matte (ticket 64, options 3 and 4) is the next thing likely to want
// letters. Anything past `f` is still absent, and the host test asserts that, so a lowercase word
// draws visible boxes rather than silently vanishing.
//
// ESP-IDF-free and host-tested, like the rest of src/core/.

#ifndef EPD_TEXT_H
#define EPD_TEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "epd_canvas.h"

#define EPD_TEXT_GLYPH_W 5
#define EPD_TEXT_GLYPH_H 7
// One column of gap between glyphs, so a string's pitch is 6 and not 5.
#define EPD_TEXT_ADVANCE 6

// The degree sign, as a string fragment to concatenate or to pass through a format. One byte, so
// epd_text_width() counts it as one glyph -- which a UTF-8 `°` would not be.
#define EPD_TEXT_DEGREE "\xB0"

// Ink width of `s` at this scale, in logical pixels: the trailing gap is not counted, so a
// centring calculation does not drift right by one column per line. 0 for NULL or "".
int32_t epd_text_width(const char *s, int32_t scale);

// Ink height, which is the same for every glyph -- there are no descenders in this set.
int32_t epd_text_height(int32_t scale);

// Draws `s` with (x, y) as the TOP-LEFT of the first glyph's box, in logical coordinates.
// Clipping is epd_canvas_fill_rect()'s, so a string that runs off the edge is cut rather than
// wrapped; the caller is expected to have asked epd_text_width() first.
//
// A byte with no glyph draws a hollow box. A missing glyph has to be visible: silently drawing
// nothing would make a wrong character look like a layout bug three refreshes later.
void epd_text_draw(epd_canvas_t *c, int32_t x, int32_t y, int32_t scale, const char *s,
                   uint8_t r, uint8_t g, uint8_t b);

// The same string turned 90° CLOCKWISE: the line runs DOWNWARD from (x, y) and the tops of the
// glyphs point towards +x -- the sense a reader gets by tilting their head to the right, and the
// same sense as Japanese vertical setting. One fixed direction whichever side of the photograph
// the band is on (ticket 64, operator 2026-09-19): the band's side varies with the picture, so a
// direction that followed it would change how the frame is read from one refresh to the next.
//
// **(x, y) is the top-left of the bounding box and the two extents SWAP**: the box is
// epd_text_height(scale) wide and epd_text_width(s, scale) tall. A caller fitting a line into a
// 67 x 400 band therefore checks its width against height() and its length against width().
//
// Clipping and the hollow missing-glyph box are epd_text_draw()'s -- both go through one loop, so
// the two cannot drift apart.
void epd_text_draw_rot90(epd_canvas_t *c, int32_t x, int32_t y, int32_t scale, const char *s,
                         uint8_t r, uint8_t g, uint8_t b);

// True when this byte has a glyph of its own rather than the hollow box.
bool epd_text_has_glyph(char ch);

// The seven rows of `ch`, bit 4 leftmost, bit 0 rightmost -- the hollow box when there is no
// glyph. For the host test's ASCII dump, which is the only proof-reading a hand-drawn table
// gets.
void epd_text_glyph_rows(char ch, uint8_t out[EPD_TEXT_GLYPH_H]);

#endif // EPD_TEXT_H
