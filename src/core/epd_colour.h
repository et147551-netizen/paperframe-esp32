// Colour space arithmetic, ported byte-for-byte from epdoptimize.
//
// This is not a general colour library and should not become one. Every function here
// exists because refs/epdoptimize/src/dither/processing.ts:320-433 has it, and each one
// reproduces that file's arithmetic exactly -- including the choices a fresh
// implementation would make differently. The point is bit-identical output against a
// reference implementation we can run on this machine (tools/epdopt_reference.mjs), which
// is what replaces judging colour by eye on a 15.6 s refresh.
//
// So: do not "fix" the constants. 0.008856 and 7.787 are the pre-1998 CIE L*a*b* pivot
// rather than the exact 216/24389 and 24389/27 that later references use; 0.206897 is
// that pivot's cube root, truncated. The differences land in the last bit of a byte,
// which is precisely what the parity test compares.
//
// Nothing here depends on ESP-IDF, so it builds under env:native with epd_dither.c.

#ifndef EPD_COLOUR_H
#define EPD_COLOUR_H

#include <stdbool.h>
#include <stdint.h>

// processing.ts:315-318. Clamp first, then round -- the order matters, because rounding
// 255.6 before clamping would still give 256.
//
// JavaScript's Math.round breaks ties towards +infinity, so this is floor(v + 0.5) and
// not lround() (which rounds half away from zero) and not a cast (which truncates).
// Clamping happens first, so the negative half of that distinction never arises.
uint8_t epd_clamp_byte(double value);

// processing.ts:320-321. Rec. 709 luma, unrounded -- callers that need a byte pass the
// result through epd_clamp_byte() themselves, as the reference does.
double epd_luma709(double r, double g, double b);

// processing.ts:435-439. HSV-style saturation on 0-255 channels: (max - min) / max, and
// zero when the pixel is black. Drives the chroma protection in the range compressor.
double epd_saturation(double r, double g, double b);

// processing.ts:390-393. sRGB (D65) to CIE L*a*b*. Input is 0-255 and integral: the
// reference indexes a 256-entry linearisation table with it, so a fractional channel has
// no defined meaning here either.
void epd_rgb_to_lab(uint8_t r, uint8_t g, uint8_t b, double *l, double *a, double *bb);

// processing.ts:423-426. The inverse, ending in three epd_clamp_byte() calls. Out-of-gamut
// L*a*b* triples clip channel-wise, which is what the reference does and is why the chroma
// guard in the range compressor exists at all.
void epd_lab_to_rgb(double l, double a, double b, uint8_t *r, uint8_t *g, uint8_t *bb);

// processing.ts:338-345. L* alone, skipping the a*/b* work. The reference keeps this
// separate for its `auto`-mode histogram pass, where only lightness is wanted, and the
// two must agree: this computes Y from the same linearisation table and pivot.
double epd_rgb_to_lab_lightness(uint8_t r, uint8_t g, uint8_t b);

#endif // EPD_COLOUR_H
