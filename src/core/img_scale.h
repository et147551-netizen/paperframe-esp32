// Downscaling by an arbitrary ratio, not by an integer factor.
//
// Its own translation unit for the same reason img_dims.c is: img_resize.c pulls in
// esp_jpeg_enc and epd_image and so cannot be in the env:native build, and this is
// arithmetic that is wrong quietly rather than loudly -- a resampler with an off-by-one in
// its source bounds produces a picture, just not the right one. Nothing here has ESP-IDF in
// it, so test/test_img_scale can have it.
//
// **Why it exists.** img_resize.c reduces by an integer box factor, and for JPEG that factor
// is ALWAYS 1: choose_jpeg_scale() leaves the decode between 1x and 2x the fit, so a factor
// of 2 would undershoot. A 12 MP photograph therefore stores at 1008x752 where the panel
// draws it at 536x400 -- 2.8x the pixels, measured on hardware 2026-09-09 (ticket 49). The
// ratio that is left over is between 1 and 2 and an integer filter cannot express it.

#ifndef IMG_SCALE_H
#define IMG_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The size to store a `sw` x `sh` picture at so that a `box` x `box` fit never has to
// upscale it, cropped to a multiple of `align`.
//
// Two sizes come back and they are not the same thing:
//
//   * `fw`/`fh` is the EXACT fit -- what epd_fit_centre() would draw the source at inside a
//     square box. It is the scale, and both axes use it, which is what keeps the aspect
//     ratio exact.
//   * `dw`/`dh` is that cropped down to the alignment the JPEG encoder needs. Cropping
//     rather than rounding, because rounding each axis independently distorts the picture
//     by up to 16 pixels on the short edge and a crop of at most 15 loses nothing anyone
//     can see. It is also exactly what the integer path already does.
//
// A square box because the stored file has to serve either `orientation` (SMB_RESIZE_FIT_EDGE,
// ticket 49) -- which is also what makes ticket 51's draw-time rotation free.
//
// Returns false, and touches nothing, for non-positive inputs or when the crop would leave an
// axis below `align`. Returns true with `dw == sw && dh == sh` when there is nothing to do,
// which the caller should treat as "keep what you have" rather than as a resample.
bool img_scale_fit_target(int32_t sw, int32_t sh, int32_t box, int32_t align,
                          int32_t *fw, int32_t *fh, int32_t *dw, int32_t *dh);

// Area-average downscale of a packed RGB888 image.
//
// Destination pixel (x, y) is the average of the source rectangle
// [x*sw/fw, (x+1)*sw/fw) x [y*sh/fh, (y+1)*sh/fh), weighted by how much of each edge source
// pixel the rectangle actually covers. **Fractional weights rather than integer bounds**: at
// the ratios this is for -- between 1 and 2 -- a box with integer bounds averages some
// destination pixels over one source pixel and some over four, which is visible as uneven
// sharpness across the picture. The weighting is what makes a 1.68 reduction look even.
//
// `fw`/`fh` set the SCALE and `dw`/`dh` set how much is written, so passing dw < fw crops the
// right-hand edge instead of stretching the picture into it. `dw <= fw` and `dh <= fh` are
// required; so is fw <= sw and fh <= sh, because this only ever reduces.
//
// src and dst must not overlap.
bool img_scale_area(const uint8_t *src, int32_t sw, int32_t sh, int32_t fw, int32_t fh,
                    uint8_t *dst, int32_t dw, int32_t dh);

// ------------------------------------------------------------------ EXIF orientation
//
// The transform TIFF tag 0x0112 asks for (img_exif_orientation(), ticket 53), applied to a
// packed RGB888 image. All eight values, including the four mirrored ones: once the loop is
// index-driven a flip costs nothing over a rotation, and a value that quietly fell through
// to "leave it alone" would be a silently wrong picture rather than a missing feature.

// Whether this orientation exchanges width and height -- 5 to 8. The caller has to know
// before it allocates, which is why this is separate from the transform.
bool img_orient_swaps_axes(int orientation);

// Whether there is anything to do: 2-8. Orientation 1 and anything out of range are the
// identity, and img_orient_rgb() refuses them rather than making a pointless copy.
bool img_orient_needed(int orientation);

// Writes the transformed image into `dst`, which must hold `dw * dh * 3` bytes where
// `dw`/`dh` are `sw`/`sh` exchanged when img_orient_swaps_axes() says so. src and dst must
// not overlap -- this cannot be done in place for a 90 degree turn, which is what bounds
// where it can be called from: after the reduction, never on a full-resolution decode.
bool img_orient_rgb(const uint8_t *src, int32_t sw, int32_t sh, int orientation,
                    uint8_t *dst);

#endif // IMG_SCALE_H
