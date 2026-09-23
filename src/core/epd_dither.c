// The palette constants and the row-wise pair search below are ported from
// refs/M5GFX/src/lgfx/v1/panel/Panel_ED2208.cpp. That file carries LovyanGFX's own header --
// "Lovyan GFX ... Licence: [FreeBSD]", i.e. BSD-2-Clause, (c) lovyan03 -- inside the M5GFX
// repository, which is MIT (c) 2021 M5Stack. BSD-2-Clause requires the notice to travel with the
// source, so it travels here; its full text is LICENSES/LovyanGFX-BSD-2-Clause.txt. Ticket 65
// item 3 is the plan to stop depending on it.

#include "epd_dither.h"

#include <string.h>

// Panel_ED2208.cpp:45-52. Do not "improve" these towards sRGB primaries: they are the
// values M5GFX matches against, and changing them changes every rendered image.
const epd_palette_entry_t EPD_PALETTE[EPD_PALETTE_COUNT] = {
    {0, 0, 0, 0x0},       // black
    {255, 255, 255, 0x1}, // white
    {255, 243, 56, 0x2},  // yellow
    {191, 0, 0, 0x3},     // red
    {100, 64, 255, 0x5},  // blue
    {67, 138, 28, 0x6},   // green
};

// What the panel SHOWS, as opposed to what it is sent. Ticket 19 is the argument for
// why this table exists at all: every nearest-colour decision above is computed against
// a display that does not exist -- this panel's white is a mid grey and its red is dark
// and unsaturated.
//
// How these were made, and what they are worth (ticket 20):
//
//   * Black and white are the EL040EF1 module manual's own optical figures, L*a*b*
//     converted to sRGB. They are also the anchors of the measurement: every reading
//     below is normalised so that the panel's white is 1.0 and its black 0.0, so those
//     two map back to the anchors by construction rather than by measurement.
//   * The four chromatic entries are this unit, photographed through the fixed USB
//     camera over six refresh cycles and normalised against white and black patches
//     inside each photograph. Uncertainty is roughly +/-0.05 to +/-0.10 of the
//     black-to-white span, which is +/-6 to +/-13 here.
//
// They are NOT colorimetry. The bench colorimeter is a monitor probe and cannot read a
// reflective panel; the bar these were made against is a visible improvement on the
// glass, not a measured one. Two of them are worth looking at twice: the green is a
// desaturated teal rather than a leaf green, and the yellow is pale. Both match what the
// photographs show and neither matches the device palette above.
const epd_palette_entry_t EPD_PALETTE_MEASURED[EPD_PALETTE_COUNT] = {
    {33, 29, 47, 0x0},    // black  -- manual, and the measurement's zero
    {154, 164, 162, 0x1}, // white  -- manual, and the measurement's one
    {173, 170, 96, 0x2},  // yellow -- measured 1.157, 1.041, 0.422
    {104, 22, 33, 0x3},   // red    -- measured 0.589, -0.053, -0.120
    {28, 67, 130, 0x5},   // blue   -- measured -0.038, 0.284, 0.719
    {43, 73, 77, 0x6},    // green  -- measured 0.084, 0.325, 0.259
};

// The EL040EF1 module manual's optical characteristics, p17, as L*a*b* converted to
// sRGB under D65 -- the same six the acceptance targets in tools/colour_check.py are
// written against. White and black come out at exactly the anchors above, which is a
// check on both: they were derived independently and agree to the last digit.
//
// Where this disagrees with the measurement is the whole reason it is here. The manual's
// yellow is (161,153,3) against a measured (173,170,96): a difference of 93 in the blue
// channel, far outside the measurement's own +/-13, and the reason the measured palette
// floods warm highlights with yellow. Either this panel's yellow is genuinely pale or the
// camera over-reads blue on a saturated yellow patch, and only the glass can say which.
const epd_palette_entry_t EPD_PALETTE_MANUAL[EPD_PALETTE_COUNT] = {
    {33, 29, 47, 0x0},    // black  L*12.0 a*7.0  b*-11.0
    {154, 164, 162, 0x1}, // white  L*66.5 a*-4.0 b*0.0
    {161, 153, 3, 0x2},   // yellow L*62.0 a*-11.0 b*65.0
    {123, 25, 19, 0x3},   // red    L*26.5 a*41.0 b*30.0
    {24, 82, 139, 0x5},   // blue   L*34.0 a*3.5  b*-37.0
    {52, 91, 58, 0x6},    // green  L*35.0 a*-22.0 b*15.0
};

// epdoptimize's five Spectra 6 calibrations, in that library's canonical role order --
// black, white, blue, green, red, yellow. See the header for why the order matters.
// Values are the `color` field of each entry; the `deviceColor` field carries no
// information, being the ideal primary that this panel's nibble already names.

const epd_palette_entry_t EPD_PALETTE_EPDOPT_AITJCIZE[EPD_PALETTE_COUNT] = {
    {2, 2, 2, 0x0},       // #020202 black
    {190, 200, 200, 0x1}, // #BEC8C8 white
    {5, 64, 158, 0x5},    // #05409E blue
    {39, 102, 60, 0x6},   // #27663C green
    {135, 19, 0, 0x3},    // #871300 red
    {205, 202, 0, 0x2},   // #CDCA00 yellow
};

const epd_palette_entry_t EPD_PALETTE_EPDOPT_SPECTRA6[EPD_PALETTE_COUNT] = {
    {31, 34, 38, 0x0},    // #1F2226 black
    {185, 199, 201, 0x1}, // #B9C7C9 white
    {35, 63, 142, 0x5},   // #233F8E blue
    {53, 86, 58, 0x6},    // #35563A green
    {98, 32, 30, 0x3},    // #62201E red
    {193, 187, 30, 0x2},  // #C1BB1E yellow
};

const epd_palette_entry_t EPD_PALETTE_EPDOPT_LEGACY[EPD_PALETTE_COUNT] = {
    {25, 30, 33, 0x0},    // #191E21 black
    {232, 232, 232, 0x1}, // #e8e8e8 white
    {33, 87, 186, 0x5},   // #2157ba blue
    {18, 95, 32, 0x6},    // #125f20 green
    {178, 19, 24, 0x3},   // #b21318 red
    {239, 222, 68, 0x2},  // #efde44 yellow
};

const epd_palette_entry_t EPD_PALETTE_EPDOPT_BOEBER[EPD_PALETTE_COUNT] = {
    {31, 34, 38, 0x0},    // #1f2226 black
    {214, 214, 214, 0x1}, // #d6d6d6 white
    {65, 108, 225, 0x5},  // #416ce1 blue
    {6, 116, 6, 0x6},     // #067406 green
    {234, 72, 67, 0x3},   // #ea4843 red
    {219, 213, 41, 0x2},  // #dbd529 yellow
};

const epd_palette_entry_t EPD_PALETTE_EPDOPT_ORIGINAL[EPD_PALETTE_COUNT] = {
    {0, 0, 0, 0x0},       // black
    {255, 255, 255, 0x1}, // white
    {0, 0, 255, 0x5},     // blue
    {0, 255, 0, 0x6},     // green
    {255, 0, 0, 0x3},     // red
    {255, 255, 0, 0x2},   // yellow
};

const epd_render_t EPD_RENDER_STOCK = {EPD_PALETTE, EPD_TONE_NONE};
const epd_render_t EPD_RENDER_MEASURED_NONE = {EPD_PALETTE_MEASURED, EPD_TONE_NONE};
const epd_render_t EPD_RENDER_MEASURED_HALF = {EPD_PALETTE_MEASURED, EPD_TONE_HALF};
const epd_render_t EPD_RENDER_MEASURED = {EPD_PALETTE_MEASURED, EPD_TONE_FULL};
const epd_render_t EPD_RENDER_MANUAL = {EPD_PALETTE_MANUAL, EPD_TONE_FULL};
const epd_render_t EPD_RENDER_EPDOPT = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_FULL};
const epd_render_t EPD_RENDER_DEFAULT = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_FULL};

// One table rather than three switch statements, so a palette cannot be added to the enum
// and then be missing from the API, the UI or the renderer -- which is the failure mode a
// per-function switch has. test_every_palette_id_is_complete walks it against the enum.
static const struct {
    const char *name;
    const char *label;
    const epd_palette_entry_t *palette;
    uint8_t tone;
} PALETTE_TABLE[EPD_PALETTE_ID_COUNT] = {
    [EPD_PALETTE_ID_AITJCIZE] = {"aitjcize", "Calibrated - balanced",
                                 EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_FULL},
    // Its own endpoints are 0 and 255, so tone compression is an identity here whatever
    // this column says. EPD_TONE_FULL is written anyway rather than EPD_TONE_NONE, because
    // the two are the same for this palette and the value that is not a special case is the
    // one that keeps reading correctly if the endpoints ever change.
    [EPD_PALETTE_ID_ORIGINAL] = {"original", "Uncalibrated - cleanest neutrals, dark shadows",
                                 EPD_PALETTE_EPDOPT_ORIGINAL, EPD_TONE_FULL},
    // The label is what the settings page puts in its Colour palette list, so it says what the
    // palette IS. It read "ticket 19's choice" until 2026-09-21 -- an internal reference in a
    // string a user picks from (found while reviewing the English copy for ticket 74). The ticket
    // number belongs in a comment, and this is the comment: ticket 19 chose this one.
    [EPD_PALETTE_ID_MANUAL] = {"manual", "Module manual's figures",
                               EPD_PALETTE_MANUAL, EPD_TONE_FULL},
    [EPD_PALETTE_ID_SPECTRA6] = {"spectra6", "Calibrated - warmer, reds tend orange",
                                 EPD_PALETTE_EPDOPT_SPECTRA6, EPD_TONE_FULL},
    [EPD_PALETTE_ID_LEGACY] = {"legacy", "Calibrated - lighter, higher contrast",
                               EPD_PALETTE_EPDOPT_LEGACY, EPD_TONE_FULL},
    [EPD_PALETTE_ID_BOEBER] = {"boeber", "Calibrated (default) - brighter primaries",
                               EPD_PALETTE_EPDOPT_BOEBER, EPD_TONE_FULL},
    [EPD_PALETTE_ID_STOCK] = {"stock", "Device primaries, no tone compression",
                              EPD_PALETTE, EPD_TONE_NONE},
};

static bool id_in_range(epd_palette_id_t id)
{
    return id >= 0 && id < EPD_PALETTE_ID_COUNT;
}

const char *epd_palette_id_name(epd_palette_id_t id)
{
    return id_in_range(id) ? PALETTE_TABLE[id].name : NULL;
}

const char *epd_palette_id_label(epd_palette_id_t id)
{
    return id_in_range(id) ? PALETTE_TABLE[id].label : NULL;
}

bool epd_palette_id_from_name(const char *name, epd_palette_id_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < EPD_PALETTE_ID_COUNT; i++) {
        if (strcmp(name, PALETTE_TABLE[i].name) == 0) {
            *out = (epd_palette_id_t)i;
            return true;
        }
    }
    return false;
}

epd_render_t epd_render_for_palette(epd_palette_id_t id)
{
    if (!id_in_range(id)) {
        return EPD_RENDER_DEFAULT;
    }
    const epd_render_t cfg = {PALETTE_TABLE[id].palette, PALETTE_TABLE[id].tone};
    return cfg;
}

// Entry 0 is black and entry 1 white in both tables, which is what makes the reachable
// range readable straight off the palette.
#define PAL_BLACK 0
#define PAL_WHITE 1

// Input 0 lands on the panel's black and input 255 on its white, with everything between
// spread across the gap. Clipping instead -- which is what matching an uncompressed range
// against a compressed palette amounts to -- throws away every highlight above the
// panel's white and every shadow below its black, and those are the two ends a viewer
// looks at first.
static inline int32_t tone_compress(int32_t v, int32_t lo, int32_t hi, int32_t amount)
{
    const int32_t full = lo + (v * (hi - lo)) / 255;
    return v + ((full - v) * amount) / 255;
}

static inline void apply_tone(const epd_render_t *cfg, int32_t *r, int32_t *g, int32_t *b)
{
    if (cfg == NULL || cfg->tone == EPD_TONE_NONE) {
        return;
    }
    const epd_palette_entry_t k = cfg->palette[PAL_BLACK];
    const epd_palette_entry_t w = cfg->palette[PAL_WHITE];
    const int32_t a = cfg->tone;
    *r = tone_compress(*r, k.r, w.r, a);
    *g = tone_compress(*g, k.g, w.g, a);
    *b = tone_compress(*b, k.b, w.b, a);
}

void epd_dither_tone_rect(uint8_t *origin, int32_t w, int32_t h, ptrdiff_t step_x,
                          ptrdiff_t step_y, const epd_render_t *cfg)
{
    if (origin == NULL || cfg == NULL || cfg->palette == NULL || w <= 0 || h <= 0 ||
        cfg->tone == EPD_TONE_NONE) {
        return;
    }

    for (int32_t y = 0; y < h; y++) {
        uint8_t *row = origin + (ptrdiff_t)y * step_y;
        for (int32_t x = 0; x < w; x++) {
            uint8_t *px = row + (ptrdiff_t)x * step_x;
            int32_t r = px[0], g = px[1], b = px[2];
            apply_tone(cfg, &r, &g, &b);
            px[0] = (uint8_t)r;
            px[1] = (uint8_t)g;
            px[2] = (uint8_t)b;
        }
    }
}

// M5GFX seeds its own search with white and a maximum distance. There is no seed entry here
// because the first iteration always beats UINT32_MAX -- the largest possible distance is
// 3 * 255^2 -- so the seed was never observable, and dropping it is what lets the diffusion
// stage share this search instead of keeping a second copy of the tie-break rule.
const epd_palette_entry_t *epd_nearest_entry_cfg(const epd_palette_entry_t *palette, int32_t r,
                                                 int32_t g, int32_t b)
{
    uint32_t min_dist = UINT32_MAX;
    const epd_palette_entry_t *best = &palette[0];

    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const int32_t dr = r - (int32_t)palette[i].r;
        const int32_t dg = g - (int32_t)palette[i].g;
        const int32_t db = b - (int32_t)palette[i].b;
        const uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db);
        if (dist < min_dist) {
            min_dist = dist;
            best = &palette[i];
        }
    }
    return best;
}

uint8_t epd_nearest_index_cfg(const epd_palette_entry_t *palette, int32_t r, int32_t g,
                              int32_t b)
{
    return epd_nearest_entry_cfg(palette, r, g, b)->index;
}

uint8_t epd_nearest_index(int32_t r, int32_t g, int32_t b)
{
    return epd_nearest_index_cfg(EPD_PALETTE, r, g, b);
}

static uint8_t pair_index_cfg(const epd_palette_entry_t *palette,
                              int32_t r0, int32_t g0, int32_t b0,
                              int32_t r1, int32_t g1, int32_t b1)
{
    int32_t dr0[EPD_PALETTE_COUNT], dg0[EPD_PALETTE_COUNT], db0[EPD_PALETTE_COUNT];
    int32_t dr1[EPD_PALETTE_COUNT], dg1[EPD_PALETTE_COUNT], db1[EPD_PALETTE_COUNT];
    int32_t indiv0[EPD_PALETTE_COUNT], indiv1[EPD_PALETTE_COUNT];

    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const epd_palette_entry_t p = palette[i];
        dr0[i] = r0 - (int32_t)p.r;
        dg0[i] = g0 - (int32_t)p.g;
        db0[i] = b0 - (int32_t)p.b;
        indiv0[i] = dr0[i] * dr0[i] + dg0[i] * dg0[i] + db0[i] * db0[i];

        dr1[i] = r1 - (int32_t)p.r;
        dg1[i] = g1 - (int32_t)p.g;
        db1[i] = b1 - (int32_t)p.b;
        indiv1[i] = dr1[i] * dr1[i] + dg1[i] * dg1[i] + db1[i] * db1[i];
    }

    uint32_t min_dist = UINT32_MAX;
    uint8_t best = 0x1;

    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        for (size_t j = 0; j < EPD_PALETTE_COUNT; j++) {
            const int32_t dr = dr0[i] + dr1[j];
            const int32_t dg = dg0[i] + dg1[j];
            const int32_t db = db0[i] + db1[j];
            const uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db) +
                                  (uint32_t)(indiv0[i] + indiv1[j]);
            if (dist < min_dist) {
                min_dist = dist;
                best = (uint8_t)((palette[i].index << 4) | palette[j].index);
            }
        }
    }
    return best;
}

uint8_t epd_pair_index(int32_t r0, int32_t g0, int32_t b0,
                       int32_t r1, int32_t g1, int32_t b1)
{
    return pair_index_cfg(EPD_PALETTE, r0, g0, b0, r1, g1, b1);
}

void epd_dither_row_none_cfg(const uint8_t *src_rgb, uint8_t *dst, size_t width,
                             const epd_render_t *cfg)
{
    if (src_rgb == NULL || dst == NULL || cfg == NULL || cfg->palette == NULL) {
        return;
    }

    for (size_t x = 0; x < width; x += 2) {
        const uint8_t *p0 = &src_rgb[x * 3];
        int32_t r = p0[0], g = p0[1], b = p0[2];
        apply_tone(cfg, &r, &g, &b);
        const uint8_t c0 = epd_nearest_index_cfg(cfg->palette, r, g, b);

        uint8_t c1 = 0x1; // odd width pads with white
        if (x + 1 < width) {
            const uint8_t *p1 = &src_rgb[(x + 1) * 3];
            r = p1[0];
            g = p1[1];
            b = p1[2];
            apply_tone(cfg, &r, &g, &b);
            c1 = epd_nearest_index_cfg(cfg->palette, r, g, b);
        }
        dst[x >> 1] = (uint8_t)((c0 << 4) | c1);
    }
}

void epd_dither_row_none(const uint8_t *src_rgb, uint8_t *dst, size_t width)
{
    epd_dither_row_none_cfg(src_rgb, dst, width, &EPD_RENDER_STOCK);
}

// The ordered-bias constants from Panel_ED2208.cpp:189-193. This is not a Bayer matrix:
// the bias walks a long cycle whose period is coprime with the row width, which is why
// it does not produce the tiling a small matrix would. The three channels are offset by
// a third of the cycle from each other, so the noise is chromatic rather than luminance
// only.
#define X_STEP (127 * 29)
#define Y_STEP (129 * 48)
#define STEP_VALUE (129 * 127)
#define STEP_DIFF (STEP_VALUE / 3)

void epd_dither_row_quality(const uint8_t *src_rgb, uint8_t *dst, size_t width, size_t y,
                            uint8_t strength)
{
    epd_dither_row_quality_cfg(src_rgb, dst, width, y, strength, &EPD_RENDER_STOCK);
}

void epd_dither_row_quality_cfg(const uint8_t *src_rgb, uint8_t *dst, size_t width,
                                size_t y, uint8_t strength, const epd_render_t *cfg)
{
    if (src_rgb == NULL || dst == NULL || cfg == NULL || cfg->palette == NULL) {
        return;
    }

    int32_t bias_base = (int32_t)((y * (size_t)Y_STEP) % (size_t)STEP_VALUE);
    int32_t r0 = 0, g0 = 0, b0 = 0;

    // Runs to x == width inclusive: the pair is emitted on odd x, so a final even pixel
    // needs one more iteration to be flushed. The extra iteration reads the neutral 128
    // rather than off the end of the row.
    for (size_t x = 0; x <= width; x++) {
        int32_t r = 128, g = 128, b = 128;
        if (x < width) {
            const uint8_t *p = &src_rgb[x * 3];
            r = p[0];
            g = p[1];
            b = p[2];
        }

        bias_base -= X_STEP;
        if (bias_base < 0) {
            bias_base += STEP_VALUE;
        }

        int32_t bias_r = bias_base;
        int32_t bias_g = bias_base - STEP_DIFF;
        if (bias_g < 0) {
            bias_g += STEP_VALUE;
        }
        int32_t bias_b = bias_g - STEP_DIFF;
        if (bias_b < 0) {
            bias_b += STEP_VALUE;
        }

        bias_b = bias_b * 2 - (STEP_VALUE - 1);
        bias_g = bias_g * 2 - (STEP_VALUE - 1);
        bias_r = bias_r * 2 - (STEP_VALUE - 1);

        int32_t bias = bias_r + bias_g + bias_b;
        bias = (bias * strength) >> 16;
        bias_r = (bias_r * strength) >> 16;
        bias_g = (bias_g * strength) >> 16;
        bias_b = (bias_b * strength) >> 16;

        r += bias + bias_r;
        g += bias + bias_g;
        b += bias + bias_b;

        // Bias first, then compress. The ordered-bias amplitude is tuned against a
        // 0-255 range; compressing it along with the pixel keeps it the same fraction
        // of the range instead of making the dither twice as loud in a range half as
        // wide.
        apply_tone(cfg, &r, &g, &b);

        if (x & 1) {
            dst[x >> 1] = pair_index_cfg(cfg->palette, r0, g0, b0, r, g, b);
        } else {
            r0 = r;
            g0 = g;
            b0 = b;
        }
    }
}

// The slack on one axis, placed. Negative slack cannot happen for a `contain` fit, but the
// clamp costs nothing and keeps a rounded width of screen_w+1 from producing a negative offset.
static int32_t place(int32_t slack, epd_align_t align)
{
    if (slack <= 0) {
        return 0;
    }
    switch (align) {
    case EPD_ALIGN_LOW:
        return 0;
    case EPD_ALIGN_HIGH:
        return slack;
    case EPD_ALIGN_CENTRE:
    default:
        return slack / 2;
    }
}

epd_fit_t epd_fit_align(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h,
                        epd_align_t x_align, epd_align_t y_align)
{
    epd_fit_t fit = {0.0f, 0, 0, 0, 0};
    if (img_w <= 0 || img_h <= 0 || screen_w <= 0 || screen_h <= 0) {
        return fit;
    }

    const float sx = (float)screen_w / (float)img_w;
    const float sy = (float)screen_h / (float)img_h;
    fit.scale = (sx < sy) ? sx : sy;

    fit.width = (int32_t)((float)img_w * fit.scale);
    fit.height = (int32_t)((float)img_h * fit.scale);
    fit.x = place(screen_w - fit.width, x_align);
    fit.y = place(screen_h - fit.height, y_align);
    return fit;
}

epd_fit_t epd_fit_centre(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h)
{
    return epd_fit_align(img_w, img_h, screen_w, screen_h, EPD_ALIGN_CENTRE, EPD_ALIGN_CENTRE);
}

int32_t epd_fit_reduction(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h,
                          int32_t align)
{
    const epd_fit_t fit = epd_fit_centre(img_w, img_h, screen_w, screen_h);
    if (fit.width <= 0 || fit.height <= 0) {
        return 1;
    }
    if (align < 1) {
        align = 1;
    }

    // floor of the LARGER ratio. Integer division is that floor, and max() commutes with
    // it because floor is monotonic -- so this needs no float and cannot round the wrong
    // way at a boundary the way (int)(1.0f / fit.scale) can.
    int32_t f = img_w / screen_w;
    const int32_t fh = img_h / screen_h;
    if (fh > f) {
        f = fh;
    }

    // The alignment crop is what makes this a loop rather than one division: 4032x3024
    // into 400x600 gives f = 10 and 403x302, and 302 cropped to a multiple of 16 is 288,
    // which is below the 300 the panel draws. Stepping down is at most a few iterations
    // because each one gains a whole factor.
    while (f > 1) {
        if ((img_w / f) / align * align >= fit.width &&
            (img_h / f) / align * align >= fit.height) {
            break;
        }
        f--;
    }
    return f < 1 ? 1 : f;
}

bool epd_fit_wants_rotate(int32_t img_w, int32_t img_h, int32_t screen_w, int32_t screen_h)
{
    if (img_w <= 0 || img_h <= 0 || screen_w <= 0 || screen_h <= 0) {
        return false;
    }
    if ((img_w > img_h) == (screen_w > screen_h)) {
        return false; // already the same way up, or one of them is square
    }

    const int32_t lo = (img_w < img_h) ? img_w : img_h;
    const int32_t hi = (img_w < img_h) ? img_h : img_w;

    // Integer, and 64-bit because a 12 MP edge times 130 does not fit the intent of a
    // 32-bit multiply even where it happens to fit the type.
    return (int64_t)hi * 100 >= (int64_t)lo * EPD_ROTATE_RATIO_X100;
}
