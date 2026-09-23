#include "epd_diffuse.h"

// diffusion-maps.ts:2-7, and epd_epdopt.c:17-22 which is the same table with double factors.
// The numerators are over 16. Each tap lands on a distinct neighbour, so the order they are
// applied in cannot matter; it is kept the same as the port's anyway, so the two tables can be
// diffed by eye.
#define FS_DENOM_SHIFT 4
#define FS_TAP_COUNT 4

static const struct {
    int8_t dx;
    int8_t dy;
    int32_t num;
} FS_TAPS[FS_TAP_COUNT] = {
    {1, 0, 7},
    {-1, 1, 3},
    {0, 1, 5},
    {1, 1, 1},
};

// The exactness argument, in three lines. See epd_diffuse.h for why this is the same value as
// epd_clamp_byte((double)base + error * num / 16.0) and not merely close to it.
static inline uint8_t add_error(int32_t base, int32_t error, int32_t num)
{
    const int32_t n = (base << FS_DENOM_SHIFT) + error * num;
    if (n < 0) {
        return 0;
    }
    if (n > (255 << FS_DENOM_SHIFT)) {
        return 255;
    }
    return (uint8_t)((n + (1 << (FS_DENOM_SHIFT - 1))) >> FS_DENOM_SHIFT);
}

void epd_diffuse_rect(uint8_t *origin, int32_t w, int32_t h, ptrdiff_t step_x, ptrdiff_t step_y,
                      const epd_diffuse_t *cfg, int32_t y_start, int32_t y_count)
{
    if (origin == NULL || cfg == NULL || cfg->palette == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (y_start < 0 || y_count <= 0 || y_start >= h) {
        return;
    }
    int32_t y_end = y_start + y_count;
    if (y_end > h) {
        y_end = h;
    }

    for (int32_t y = y_start; y < y_end; y++) {
        const bool reverse = cfg->serpentine && (y % 2 == 1);
        const int32_t x_start = reverse ? w - 1 : 0;
        const int32_t x_end = reverse ? -1 : w;
        const int32_t x_step = reverse ? -1 : 1;

        // Per-tap byte offset from the current pixel, computed once a row rather than once a
        // pixel. The reference mirrors dx with the scan direction (epd_epdopt.c:644), which is
        // what keeps a serpentine pass pushing error *forwards* on a right-to-left row.
        ptrdiff_t tap_off[FS_TAP_COUNT];
        for (size_t t = 0; t < FS_TAP_COUNT; t++) {
            const int32_t dx = reverse ? -(int32_t)FS_TAPS[t].dx : (int32_t)FS_TAPS[t].dx;
            tap_off[t] = (ptrdiff_t)dx * step_x + (ptrdiff_t)FS_TAPS[t].dy * step_y;
        }

        uint8_t *row = origin + (ptrdiff_t)y * step_y;

        for (int32_t x = x_start; x != x_end; x += x_step) {
            uint8_t *px = row + (ptrdiff_t)x * step_x;

            const int32_t old_r = px[0];
            const int32_t old_g = px[1];
            const int32_t old_b = px[2];
            const epd_palette_entry_t *chosen =
                epd_nearest_entry_cfg(cfg->palette, old_r, old_g, old_b);

            px[0] = chosen->r;
            px[1] = chosen->g;
            px[2] = chosen->b;

            const int32_t err_r = old_r - (int32_t)chosen->r;
            const int32_t err_g = old_g - (int32_t)chosen->g;
            const int32_t err_b = old_b - (int32_t)chosen->b;

            for (size_t t = 0; t < FS_TAP_COUNT; t++) {
                const int32_t dx = reverse ? -(int32_t)FS_TAPS[t].dx : (int32_t)FS_TAPS[t].dx;
                const int32_t nx = x + dx;
                const int32_t ny = y + (int32_t)FS_TAPS[t].dy;
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
                    continue;
                }

                uint8_t *np = px + tap_off[t];
                const int32_t num = FS_TAPS[t].num;
                np[0] = add_error(np[0], err_r, num);
                np[1] = add_error(np[1], err_g, num);
                np[2] = add_error(np[2], err_b, num);
            }
        }
    }
}
