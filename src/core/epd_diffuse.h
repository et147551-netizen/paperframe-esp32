// Floyd-Steinberg error diffusion in integers, for the device.
//
// This is to src/core/epd_epdopt.c's diffusion stage what src/core/epd_adjust.c is to its first two: the
// same function, rewritten to be affordable on this board. The port measured **11.2 s** a
// photograph for the diffusion alone (docs/measurements.md:181-190, from 23 966 ms with
// diffusion against 12 826 ms without, one capture, same picture) and its cost is not the
// algorithm -- the inner loop is `double` on a single-precision FPU, and epd_clamp_byte() does an
// `isfinite` and a `floor` twelve times per pixel.
//
// **For Floyd-Steinberg the whole of that is removable exactly, and that is why this file
// exists.** The reference computes epd_clamp_byte((double)np + error * factor), where `error` is
// an exact integer in [-255, 255], `factor` is k/16, and epd_clamp_byte() clamps to [0, 255] and
// *then* rounds with floor(v + 0.5). Writing N = 16*np + error*k, which is an exact integer:
//
//     N < 0      -> 0
//     N > 4080   -> 255                       (255 * 16; the reference clamps before rounding)
//     otherwise  -> (N + 8) >> 4              (arithmetic shift is floor division)
//
// because floor(N/16 + 0.5) == floor((N + 8)/16). No multiply by a fraction, no isfinite, no
// floor, and **bit-identical to the double implementation**, which test/test_diffuse/ asserts
// against epdoptimize's own bytes rather than against the port.
//
// Note what does *not* work: narrowing epd_diffusion_tap_t.factor to float buys nothing, because
// `double * float` promotes and every operation stays double. The win is the integer form.
//
// **Stucki is deliberately absent.** Its factors are k/42, which is not exact in binary, so the
// same rewrite would not be bit-identical and would need a mismatch budget instead of an
// equality test. epdoptimize asks for Stucki on highContrastPhoto
// (refs/epdoptimize/src/auto-processing.ts:613-622); such a request falls back to
// Floyd-Steinberg here and the caller logs it. That is a recorded divergence, not an oversight.
//
// Free of ESP-IDF, so it builds and is tested under env:native.

#ifndef EPD_DIFFUSE_H
#define EPD_DIFFUSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_dither.h"

typedef struct {
    // Six entries. Ties go to the earlier entry, so a differently ordered palette quantises a
    // tie-breaking pixel differently -- see EPD_PALETTE_EPDOPT_* in epd_dither.h.
    const epd_palette_entry_t *palette;
    // Every errorDiffusion arm of epdoptimize's layered auto carries serpentine
    // (auto-processing.ts:388-391). The library's own presets do not, which is why this is a
    // parameter and not a constant.
    bool serpentine;
} epd_diffuse_t;

// Diffuses a w x h rectangle, in place, replacing every pixel with its palette colour.
//
// **Addressing is affine, and that is what makes one function serve both scan orders.** Pixel
// (x, y) is `origin + x*step_x + y*step_y`, in bytes, and the steps may be negative: at canvas
// rotation 1 a logical row walks *backwards* through memory (epd_canvas.c's offset_of()), so
// walking the picture in the order it was drawn -- which is the order epdoptimize diffuses in,
// and the order the demo page's canvas is in -- needs a negative step_x. Use
// epd_canvas_logical_walk() to get the three values; do not compute them at a call site.
//
// `y_start` and `y_count` process a band of rows, and **banding is exact**: all diffusion state
// lives in the pixels themselves, because the reference accumulates error through clamped bytes
// rather than a float plane (epd_epdopt.c:656-659), and Floyd-Steinberg reaches only dy = +1.
// Serpentine direction comes from the absolute row index, so a band boundary on an odd row is
// not a special case. `h` stays the full height whatever the band is -- a tap from the last row
// of a band writes into the first row of the next one, which is required rather than tolerated.
//
// A NULL or degenerate argument does nothing.
void epd_diffuse_rect(uint8_t *origin, int32_t w, int32_t h, ptrdiff_t step_x, ptrdiff_t step_y,
                      const epd_diffuse_t *cfg, int32_t y_start, int32_t y_count);

#endif // EPD_DIFFUSE_H
