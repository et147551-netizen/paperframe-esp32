// An off-screen RGB888 image the size of the panel, and the handful of drawing
// operations the frame application actually needs.
//
// This is the piece M5GFX's M5Canvas was doing. Everything an image decoder produces
// lands here, and epd_dither.c reads rows back out of it on the way to the panel.
//
// The buffer is supplied by the caller rather than allocated here, for two reasons: on
// the device it has to come from PSRAM (400 x 600 x 3 = 720 KB will not fit in internal
// RAM), and on the host the tests want a small canvas on the stack. That keeps this file
// free of ESP-IDF so it builds under env:native with the rest of the pure logic.
//
// Rotation is a property of the canvas, not of the caller. It is a COUNT OF QUARTER TURNS
// from the panel's native orientation, 0..3 since ticket 69; on the M5Paper Color the panel
// is physically 400 x 600 portrait, so rotations 1 and 3 present it as 600 x 400 landscape
// the two ways round. Drawing operations take logical coordinates and this file maps them, so
// nothing above has to think about it -- and epd_canvas_row() always returns physical rows,
// because that is the only order the panel accepts.

#ifndef EPD_CANVAS_H
#define EPD_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_dither.h"

// Quarter turns from the panel's native orientation. **The number is panel-relative and the
// WORDS are not**: rotation 0 is portrait on the EL040EF1 and landscape on the ED2208-GCA, which
// is why nothing in this project stores "portrait" and why app_server.c derives the word from
// EPD_WIDTH > EPD_HEIGHT in both directions. Ticket 69; it was 0 or 1 until 2026-09-20.
#define EPD_CANVAS_ROTATION_MAX 3

typedef struct {
    uint8_t *rgb;       // width * height * 3, caller-owned
    int32_t width;      // physical, always the panel's native orientation
    int32_t height;
    uint8_t rotation;   // quarter turns from native, 0..EPD_CANVAS_ROTATION_MAX
} epd_canvas_t;

// Bytes a canvas of this size needs. Use it to size the allocation rather than
// repeating the arithmetic at the call site.
static inline size_t epd_canvas_bytes(int32_t width, int32_t height)
{
    return (size_t)width * (size_t)height * 3u;
}

// Binds a caller-owned buffer. Returns false if the buffer is NULL, the dimensions are
// non-positive, or `bytes` is smaller than epd_canvas_bytes() -- a short buffer must be
// refused here rather than discovered as heap corruption three functions later.
bool epd_canvas_init(epd_canvas_t *c, uint8_t *buffer, size_t bytes, int32_t width,
                     int32_t height);

// Logical dimensions, which swap with rotation. Drawing coordinates are in these.
int32_t epd_canvas_logical_width(const epd_canvas_t *c);
int32_t epd_canvas_logical_height(const epd_canvas_t *c);

// Quarter turns, 0..EPD_CANVAS_ROTATION_MAX; anything else is ignored and the current value kept.
//
// **Ignored, not clamped, and that silence is a trap worth knowing about** — ticket 69 §6. This
// guard was one of seven that each rejected `> 1` independently, in five files, and a build that
// widened some of them gave a frame whose rotation setting stuck in NVS, read back correctly over
// HTTP, and never turned the panel. If you are widening this range again, `git grep rotation` and
// count the sites before changing one.
void epd_canvas_set_rotation(epd_canvas_t *c, uint8_t rotation);

// Whole-canvas fill. Every image render starts with white, so a picture that does not
// cover the screen is matted rather than showing what was there before (FR-5.3).
void epd_canvas_fill(epd_canvas_t *c, uint8_t r, uint8_t g, uint8_t b);

// Axis-aligned rectangle in logical coordinates, clipped to the canvas. Used by the QR
// renderer, which draws one rect per module.
void epd_canvas_fill_rect(epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                          uint8_t r, uint8_t g, uint8_t b);

// Draws a decoded RGB888 image, scaled and positioned by `fit` (from epd_fit_centre()),
// in logical coordinates. Sampling is nearest-neighbour: the source has already been
// resized to roughly panel size by whoever uploaded it, and on a six-colour panel that
// is about to be dithered, a smoother resample buys nothing a viewer can see.
//
// Clips rather than trusting `fit` -- a corrupt image header must not write outside the
// buffer.
void epd_canvas_blit(epd_canvas_t *c, const uint8_t *src_rgb, int32_t src_w,
                     int32_t src_h, epd_fit_t fit);

// Mutable pointer to one logical pixel's three bytes, or NULL if (x, y) is outside the
// canvas. The rotation mapping lives in this file and nowhere else, which is why the
// error-diffusion pass in epd_epdopt.c goes through here rather than indexing `rgb`: it
// has to walk the image in the order the picture was drawn, not the order the panel is
// scanned in, or the diffusion direction would turn with the frame's rotation setting.
uint8_t *epd_canvas_pixel(epd_canvas_t *c, int32_t x, int32_t y);

// The physical rectangle that a logical one occupies.
//
// For the per-pixel adjustment stages (epd_adjust.h), which do not care what order they see the
// pixels in -- only which bytes are the picture rather than the white matte around it. Those
// stages walk physical rows with a stride, so they need the rectangle rather than a pixel
// accessor, and this is where the rotation mapping is applied to it: the alternative is a second
// copy of that mapping in a second file, which is what this header says not to do.
//
// Returns false for a rectangle that is degenerate or not wholly inside the logical canvas. It
// does not clip -- a caller whose fit rectangle does not fit has computed it against different
// dimensions than the canvas has, and silently shrinking it would hide that.
bool epd_canvas_physical_rect(const epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t *px, int32_t *py, int32_t *pw, int32_t *ph);

// Affine addressing of a logical rectangle: an origin, and a byte step per logical x and per
// logical y, such that pixel (x + dx, y + dy) of the rectangle is `origin + dx*step_x +
// dy*step_y`.
//
// For the error-diffusion stage (epd_diffuse.h), which unlike the adjustment stages **does** care
// what order it sees the pixels in -- it must walk the picture as it was drawn, or the diffusion
// direction would turn with the frame's rotation setting -- but cannot afford a function call per
// pixel and per tap. The mapping is affine, so two steps and a base express it exactly, and the
// only thing the rotation changes is which of them is negative: at rotation 1 `step_x` is, because
// a logical row runs backwards through memory; at rotation 2 **both** are; at rotation 3 it is
// `step_y`.
//
// Same validation as epd_canvas_physical_rect(), and it does not clip either.
bool epd_canvas_logical_walk(epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint8_t **origin, ptrdiff_t *step_x, ptrdiff_t *step_y);

// One physical row, for the dither stage. Returns NULL if y is out of range.
//
// Always physical: the panel is scanned in its native orientation regardless of what the
// application thinks it drew.
const uint8_t *epd_canvas_row(const epd_canvas_t *c, int32_t y);

#endif // EPD_CANVAS_H
