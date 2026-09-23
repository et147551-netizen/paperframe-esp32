#include "img_scale.h"

#include <string.h>

bool img_scale_fit_target(int32_t sw, int32_t sh, int32_t box, int32_t align,
                          int32_t *fw, int32_t *fh, int32_t *dw, int32_t *dh)
{
    if (sw <= 0 || sh <= 0 || box <= 0 || align < 1 || fw == NULL || fh == NULL ||
        dw == NULL || dh == NULL) {
        return false;
    }

    // Already inside the box: there is nothing to reduce, and saying so is not the same as
    // returning a scale of 1 and resampling anyway. A resample at 1.0 would still re-encode
    // and still lose a generation.
    if (sw <= box && sh <= box) {
        *fw = sw;
        *fh = sh;
        *dw = sw;
        *dh = sh;
        return true;
    }

    // The binding ratio is the LONG edge, because the box is square: scale = box / max(sw, sh),
    // so the long edge lands on `box` exactly and the short one follows it. Integer, and 64-bit
    // in the multiply so that a 12 MP edge times the box cannot overflow -- it would not at
    // these sizes, but it would the day the panel grew.
    int32_t f_w, f_h;
    if (sw >= sh) {
        f_w = box;
        f_h = (int32_t)(((int64_t)sh * box) / sw);
    } else {
        f_h = box;
        f_w = (int32_t)(((int64_t)sw * box) / sh);
    }
    if (f_w < 1) {
        f_w = 1;
    }
    if (f_h < 1) {
        f_h = 1;
    }

    const int32_t c_w = f_w / align * align;
    const int32_t c_h = f_h / align * align;
    if (c_w < align || c_h < align) {
        return false;
    }

    *fw = f_w;
    *fh = f_h;
    *dw = c_w;
    *dh = c_h;
    return true;
}

bool img_scale_area(const uint8_t *src, int32_t sw, int32_t sh, int32_t fw, int32_t fh,
                    uint8_t *dst, int32_t dw, int32_t dh)
{
    if (src == NULL || dst == NULL || sw <= 0 || sh <= 0 || fw <= 0 || fh <= 0 || dw <= 0 ||
        dh <= 0) {
        return false;
    }
    if (fw > sw || fh > sh || dw > fw || dh > fh) {
        return false; // this only ever reduces, and it never writes past the scale
    }

    // Source extent of one destination pixel, in 16.16 fixed point. Fixed point rather than
    // float because the accumulator below is integer anyway and mixing the two would cost a
    // conversion per pixel on a single-precision FPU.
    const uint32_t step_x = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)fw);
    const uint32_t step_y = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)fh);

    for (int32_t y = 0; y < dh; y++) {
        const uint64_t y0 = (uint64_t)y * step_y;
        uint64_t y1 = y0 + step_y;
        if (y1 > ((uint64_t)sh << 16)) {
            y1 = (uint64_t)sh << 16;
        }
        const int32_t sy0 = (int32_t)(y0 >> 16);
        int32_t sy1 = (int32_t)((y1 + 0xFFFF) >> 16); // ceil
        if (sy1 > sh) {
            sy1 = sh;
        }
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }

        for (int32_t x = 0; x < dw; x++) {
            const uint64_t x0 = (uint64_t)x * step_x;
            uint64_t x1 = x0 + step_x;
            if (x1 > ((uint64_t)sw << 16)) {
                x1 = (uint64_t)sw << 16;
            }
            const int32_t sx0 = (int32_t)(x0 >> 16);
            int32_t sx1 = (int32_t)((x1 + 0xFFFF) >> 16);
            if (sx1 > sw) {
                sx1 = sw;
            }
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }

            uint64_t acc_r = 0, acc_g = 0, acc_b = 0, acc_w = 0;
            for (int32_t sy = sy0; sy < sy1; sy++) {
                // How much of this source row the destination pixel covers, in 16.16. The
                // first and last rows are partial; everything between is whole.
                const uint64_t row_lo = (uint64_t)sy << 16;
                const uint64_t row_hi = row_lo + (1u << 16);
                const uint64_t top = (y0 > row_lo) ? y0 : row_lo;
                const uint64_t bot = (y1 < row_hi) ? y1 : row_hi;
                if (bot <= top) {
                    continue;
                }
                const uint64_t wy = bot - top;

                const uint8_t *row = src + ((size_t)sy * (size_t)sw + (size_t)sx0) * 3u;
                for (int32_t sx = sx0; sx < sx1; sx++) {
                    const uint64_t col_lo = (uint64_t)sx << 16;
                    const uint64_t col_hi = col_lo + (1u << 16);
                    const uint64_t left = (x0 > col_lo) ? x0 : col_lo;
                    const uint64_t right = (x1 < col_hi) ? x1 : col_hi;
                    if (right <= left) {
                        row += 3;
                        continue;
                    }
                    // 16.16 x 16.16 kept as a 32.32 weight; the accumulators are 64-bit so a
                    // whole destination pixel's worth of weight cannot wrap.
                    const uint64_t w = (wy * (right - left)) >> 16;
                    acc_r += (uint64_t)row[0] * w;
                    acc_g += (uint64_t)row[1] * w;
                    acc_b += (uint64_t)row[2] * w;
                    acc_w += w;
                    row += 3;
                }
            }

            uint8_t *out = dst + ((size_t)y * (size_t)dw + (size_t)x) * 3u;
            if (acc_w == 0) {
                // Unreachable for the ratios this is used at -- the bounds above guarantee at
                // least one source pixel -- but a zero divide is not the way to find that out.
                const uint8_t *p = src + ((size_t)sy0 * (size_t)sw + (size_t)sx0) * 3u;
                out[0] = p[0];
                out[1] = p[1];
                out[2] = p[2];
                continue;
            }
            const uint64_t half = acc_w / 2;
            out[0] = (uint8_t)((acc_r + half) / acc_w);
            out[1] = (uint8_t)((acc_g + half) / acc_w);
            out[2] = (uint8_t)((acc_b + half) / acc_w);
        }
    }
    return true;
}

// ------------------------------------------------------------------ EXIF orientation

// {transpose, flip source x, flip source y} for each EXIF orientation, index 1-8. Index 0
// is the identity so an out-of-range value cannot reach past the table.
static const uint8_t ORIENT_OPS[9][3] = {
    {0, 0, 0}, // 0: not a value; identity
    {0, 0, 0}, // 1: top-left, as stored
    {0, 1, 0}, // 2: mirrored horizontally
    {0, 1, 1}, // 3: rotated 180
    {0, 0, 1}, // 4: mirrored vertically
    {1, 0, 0}, // 5: transposed
    {1, 0, 1}, // 6: rotated 90 clockwise -- the portrait phone photograph
    {1, 1, 1}, // 7: transverse
    {1, 1, 0}, // 8: rotated 90 anticlockwise
};

bool img_orient_swaps_axes(int orientation)
{
    if (orientation < 1 || orientation > 8) {
        return false;
    }
    return ORIENT_OPS[orientation][0] != 0;
}

bool img_orient_needed(int orientation)
{
    return orientation >= 2 && orientation <= 8;
}

bool img_orient_rgb(const uint8_t *src, int32_t sw, int32_t sh, int orientation, uint8_t *dst)
{
    if (src == NULL || dst == NULL || sw <= 0 || sh <= 0 || !img_orient_needed(orientation)) {
        return false;
    }

    const bool transpose = ORIENT_OPS[orientation][0] != 0;
    const bool flip_x = ORIENT_OPS[orientation][1] != 0;
    const bool flip_y = ORIENT_OPS[orientation][2] != 0;
    const int32_t dw = transpose ? sh : sw;
    const int32_t dh = transpose ? sw : sh;

    // Written destination-major so the destination is a straight forward walk and the
    // source is the scattered one. The other way round would leave holes if the mapping
    // were ever wrong; this way every destination pixel is written exactly once, which is
    // the property worth having in a transform whose failure is a picture rather than a
    // crash.
    for (int32_t y = 0; y < dh; y++) {
        uint8_t *out = dst + (size_t)y * (size_t)dw * 3u;
        for (int32_t x = 0; x < dw; x++) {
            const int32_t u = transpose ? y : x;
            const int32_t v = transpose ? x : y;
            const int32_t sx = flip_x ? (sw - 1 - u) : u;
            const int32_t sy = flip_y ? (sh - 1 - v) : v;
            const uint8_t *p = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 3u;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            out += 3;
        }
    }
    return true;
}
