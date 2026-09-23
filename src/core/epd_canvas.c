#include "epd_canvas.h"

#include <string.h>

// Logical (x, y) -> physical byte offset, or -1 if outside the canvas.
//
// **THE ONLY PLACE THE ROTATION LIVES.** Everything else in this file is derived from these
// four lines symbolically, and the header says why a second copy elsewhere is forbidden.
//
// Rotation is a COUNT OF QUARTER TURNS from the panel's native orientation, 0..3 since
// ticket 69 (the owner's request of 2026-09-20; it was 0 or 1 before). Rotation 1 is the
// original, chosen to match what the shipping firmware shows: the recovered factory
// photograph is landscape on a portrait panel with the image upright. The other two are
// derived from it rather than invented, so that "one more quarter turn" means the same thing
// at every step:
//
//   r0: (x_p, y_p) = (x,             y)
//   r1: (x_p, y_p) = (y,             height - 1 - x)      <- 90 degrees, the original
//   r2: (x_p, y_p) = (width - 1 - x, height - 1 - y)      <- r0 turned 180
//   r3: (x_p, y_p) = (width - 1 - y, x)                   <- r1 turned 180
//
// r2 is r0's logical space point-reflected, and r3 is r1's, which is what makes 0 and 2 share
// a logical shape and 1 and 3 share the other. That is load-bearing well outside this file:
// SMB_RESIZE_FIT_EDGE stores one square because the logical dimensions still take only TWO
// values, and app_display.c's auto-rotate turns by `base_rotation ^ 1u` because XOR with 1
// maps 0<->1 and 2<->3 -- a quarter turn to the other shape from whichever base the frame is
// set to. `(base + 1) % 4` would map 1 to 2, the SAME shape, which is a 180-degree flip of
// the picture and no turn at all.
static int32_t offset_of(const epd_canvas_t *c, int32_t x, int32_t y)
{
    int32_t px, py;

    switch (c->rotation) {
    case 1:
        px = y;
        py = c->height - 1 - x;
        break;
    case 2:
        px = c->width - 1 - x;
        py = c->height - 1 - y;
        break;
    case 3:
        px = c->width - 1 - y;
        py = x;
        break;
    default:
        px = x;
        py = y;
        break;
    }

    if (px < 0 || px >= c->width || py < 0 || py >= c->height) {
        return -1;
    }
    return (py * c->width + px) * 3;
}

bool epd_canvas_init(epd_canvas_t *c, uint8_t *buffer, size_t bytes, int32_t width,
                     int32_t height)
{
    if (c == NULL || buffer == NULL || width <= 0 || height <= 0) {
        return false;
    }
    if (bytes < epd_canvas_bytes(width, height)) {
        return false;
    }

    c->rgb = buffer;
    c->width = width;
    c->height = height;
    c->rotation = 0;
    return true;
}

// The ODD rotations are the turned ones, which is read off offset_of() above: 1 and 3 put the
// logical x extent on the physical y axis and vice versa, 0 and 2 do not.
static bool is_turned(const epd_canvas_t *c)
{
    return (c->rotation & 1u) != 0u;
}

int32_t epd_canvas_logical_width(const epd_canvas_t *c)
{
    if (c == NULL) {
        return 0;
    }
    return is_turned(c) ? c->height : c->width;
}

int32_t epd_canvas_logical_height(const epd_canvas_t *c)
{
    if (c == NULL) {
        return 0;
    }
    return is_turned(c) ? c->width : c->height;
}

void epd_canvas_set_rotation(epd_canvas_t *c, uint8_t rotation)
{
    if (c == NULL || rotation > EPD_CANVAS_ROTATION_MAX) {
        return;
    }
    c->rotation = rotation;
}

void epd_canvas_fill(epd_canvas_t *c, uint8_t r, uint8_t g, uint8_t b)
{
    if (c == NULL || c->rgb == NULL) {
        return;
    }

    const size_t pixels = (size_t)c->width * (size_t)c->height;

    if (r == g && g == b) {
        memset(c->rgb, r, pixels * 3);
        return;
    }

    for (size_t i = 0; i < pixels; i++) {
        c->rgb[i * 3 + 0] = r;
        c->rgb[i * 3 + 1] = g;
        c->rgb[i * 3 + 2] = b;
    }
}

void epd_canvas_fill_rect(epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                          uint8_t r, uint8_t g, uint8_t b)
{
    if (c == NULL || c->rgb == NULL || w <= 0 || h <= 0) {
        return;
    }

    for (int32_t dy = 0; dy < h; dy++) {
        for (int32_t dx = 0; dx < w; dx++) {
            const int32_t off = offset_of(c, x + dx, y + dy);
            if (off < 0) {
                continue; // clipped
            }
            c->rgb[off + 0] = r;
            c->rgb[off + 1] = g;
            c->rgb[off + 2] = b;
        }
    }
}

void epd_canvas_blit(epd_canvas_t *c, const uint8_t *src_rgb, int32_t src_w, int32_t src_h,
                     epd_fit_t fit)
{
    if (c == NULL || c->rgb == NULL || src_rgb == NULL) {
        return;
    }
    if (src_w <= 0 || src_h <= 0 || fit.width <= 0 || fit.height <= 0) {
        return;
    }

    for (int32_t dy = 0; dy < fit.height; dy++) {
        // Nearest-neighbour source row. Computed from the destination extent rather than
        // from fit.scale so that rounding cannot walk off the end of the source on the
        // final row.
        int32_t sy = (int32_t)(((int64_t)dy * src_h) / fit.height);
        if (sy < 0) {
            sy = 0;
        } else if (sy >= src_h) {
            sy = src_h - 1;
        }

        for (int32_t dx = 0; dx < fit.width; dx++) {
            int32_t sx = (int32_t)(((int64_t)dx * src_w) / fit.width);
            if (sx < 0) {
                sx = 0;
            } else if (sx >= src_w) {
                sx = src_w - 1;
            }

            const int32_t off = offset_of(c, fit.x + dx, fit.y + dy);
            if (off < 0) {
                continue;
            }

            const uint8_t *s = &src_rgb[((size_t)sy * (size_t)src_w + (size_t)sx) * 3];
            c->rgb[off + 0] = s[0];
            c->rgb[off + 1] = s[1];
            c->rgb[off + 2] = s[2];
        }
    }
}

uint8_t *epd_canvas_pixel(epd_canvas_t *c, int32_t x, int32_t y)
{
    if (c == NULL || c->rgb == NULL) {
        return NULL;
    }
    const int32_t off = offset_of(c, x, y);
    if (off < 0) {
        return NULL;
    }
    return &c->rgb[off];
}

bool epd_canvas_physical_rect(const epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t *px, int32_t *py, int32_t *pw, int32_t *ph)
{
    if (c == NULL || px == NULL || py == NULL || pw == NULL || ph == NULL) {
        return false;
    }
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > epd_canvas_logical_width(c) ||
        y + h > epd_canvas_logical_height(c)) {
        return false;
    }

    // Each case is offset_of()'s corresponding line applied to the rectangle's two extents, and
    // nothing here is written out independently -- a second copy of the rotation is exactly what
    // this file exists to prevent. An extent [a, a+n-1] under `k - 1 - v` becomes
    // [k - a - n, k - a - 1], which is where every `- w` and `- h` below comes from.
    switch (c->rotation) {
    case 1:
        // (x_p, y_p) = (y, height - 1 - x): the logical x extent becomes a physical y extent
        // counted from the far edge, and the two dimensions swap.
        *px = y;
        *pw = h;
        *py = c->height - x - w;
        *ph = w;
        break;
    case 2:
        // (x_p, y_p) = (width - 1 - x, height - 1 - y): both extents reverse, neither swaps.
        *px = c->width - x - w;
        *pw = w;
        *py = c->height - y - h;
        *ph = h;
        break;
    case 3:
        // (x_p, y_p) = (width - 1 - y, x): the dimensions swap as at rotation 1, but it is the
        // logical y extent that is counted from the far edge rather than the x one.
        *px = c->width - y - h;
        *pw = h;
        *py = x;
        *ph = w;
        break;
    default:
        *px = x;
        *py = y;
        *pw = w;
        *ph = h;
        break;
    }
    return true;
}

bool epd_canvas_logical_walk(epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint8_t **origin, ptrdiff_t *step_x, ptrdiff_t *step_y)
{
    if (c == NULL || c->rgb == NULL || origin == NULL || step_x == NULL || step_y == NULL) {
        return false;
    }
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > epd_canvas_logical_width(c) ||
        y + h > epd_canvas_logical_height(c)) {
        return false;
    }

    // Read straight off offset_of() above, which is the only place the rotation lives:
    //
    //   rotation 0: offset(x, y) = (y * width + x) * 3
    //   rotation 1: offset(x, y) = ((height - 1 - x) * width + y) * 3
    //   rotation 2: offset(x, y) = ((height - 1 - y) * width + (width - 1 - x)) * 3
    //   rotation 3: offset(x, y) = (x * width + (width - 1 - y)) * 3
    //
    // All four are affine in x and y, so the partial derivatives are the two steps -- and the
    // sign of each is the whole of what changes. At the ODD rotations the y step is a single
    // pixel and the x step strides a physical row, which is the reason this walk costs what it
    // does: a logical row there touches one pixel per cache line. At rotation 2 both steps are
    // negative; at rotation 3 it is the y step that is negative where at rotation 1 it was the
    // x step. A mis-derived sign here is the likeliest way to get four directions wrong, which
    // is why test_canvas checks these against epd_canvas_pixel() at the rectangle's corners
    // rather than checking them against a table written out by the same hand.
    const int32_t off = offset_of(c, x, y);
    if (off < 0) {
        return false;
    }

    const ptrdiff_t row = (ptrdiff_t)c->width * 3;
    *origin = &c->rgb[off];
    switch (c->rotation) {
    case 1:
        *step_x = -row;
        *step_y = 3;
        break;
    case 2:
        *step_x = -3;
        *step_y = -row;
        break;
    case 3:
        *step_x = row;
        *step_y = -3;
        break;
    default:
        *step_x = 3;
        *step_y = row;
        break;
    }
    return true;
}

const uint8_t *epd_canvas_row(const epd_canvas_t *c, int32_t y)
{
    if (c == NULL || c->rgb == NULL || y < 0 || y >= c->height) {
        return NULL;
    }
    return &c->rgb[(size_t)y * (size_t)c->width * 3];
}
