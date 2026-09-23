// Ported from paperlesspaper/epdoptimize, src/dither/processing.ts and src/dither/dither.ts.
// Copyright the epdoptimize authors, licensed Apache-2.0; see LICENSES/epdoptimize-Apache-2.0.txt.

#include "epd_epdopt.h"

#include <math.h>
#include <stdlib.h>

#include "epd_colour.h"

// ------------------------------------------------------------------ diffusion kernels
//
// diffusion-maps.ts:2-7 and :37-52. Factors are written as fractions rather than decimals
// for the same reason the reference does: 7/16 is exact in binary, 0.4375 typed by hand is
// an opportunity to get a digit wrong.

static const epd_diffusion_tap_t FLOYD_STEINBERG_TAPS[] = {
    {1, 0, 7.0 / 16.0},
    {-1, 1, 3.0 / 16.0},
    {0, 1, 5.0 / 16.0},
    {1, 1, 1.0 / 16.0},
};

static const epd_diffusion_tap_t STUCKI_TAPS[] = {
    {1, 0, 8.0 / 42.0},
    {2, 0, 4.0 / 42.0},

    {-2, 1, 2.0 / 42.0},
    {-1, 1, 4.0 / 42.0},
    {0, 1, 8.0 / 42.0},
    {1, 1, 4.0 / 42.0},
    {2, 1, 2.0 / 42.0},

    {-2, 2, 1.0 / 42.0},
    {-1, 2, 2.0 / 42.0},
    {0, 2, 4.0 / 42.0},
    {1, 2, 2.0 / 42.0},
    {2, 2, 1.0 / 42.0},
};

const epd_diffusion_kernel_t EPD_DIFFUSION_FLOYD_STEINBERG = {
    FLOYD_STEINBERG_TAPS, sizeof(FLOYD_STEINBERG_TAPS) / sizeof(FLOYD_STEINBERG_TAPS[0])};

const epd_diffusion_kernel_t EPD_DIFFUSION_STUCKI = {
    STUCKI_TAPS, sizeof(STUCKI_TAPS) / sizeof(STUCKI_TAPS[0])};

// ------------------------------------------------------------------------- presets
//
// processing.ts:130-147. `balanced`'s tone mapping is neutral -- exposure 0, saturation 0,
// contrast 0 all resolve to a multiplier of exactly 1 -- so the stage is an identity pass
// for this preset. It still runs, because it is where a different preset's look comes
// from and because running it proves the LUTs are identity rather than assuming so.
//
// Note that `strength`, `midpoint` and `highlight_compress` are carried here even though
// mode `contrast` never reads them. The reference resolves them with `??` fallbacks at the
// point of use (processing.ts:1116-1125); this struct has no `undefined`, so a preset that
// selects EPD_TONE_MODE_SCURVE must set all three explicitly. Upstream's fallbacks are
// strength 0.9, highlight_compress -1.5, midpoint 0.5, and a zeroed midpoint would be
// clamped to 0.01 and produce a quite different curve.

const epd_epdopt_t EPD_EPDOPT_BALANCED = {
    .palette = EPD_PALETTE_EPDOPT_AITJCIZE,

    .tone_enabled = true,
    .tone_mode = EPD_TONE_MODE_CONTRAST,
    .exposure = 0.0,
    .saturation = 0.0,
    .contrast = 0.0,
    .strength = 0.0,
    .shadow_boost = 0.0,
    .highlight_compress = -1.5,
    .midpoint = 0.5,

    .range_mode = EPD_RANGE_MODE_DISPLAY,
    .range_quality = EPD_RANGE_ACCURATE,
    .range_strength = 1.0,
    .low_percentile = 0.01,
    .high_percentile = 0.99,

    .kernel = &EPD_DIFFUSION_FLOYD_STEINBERG,
    .serpentine = false,
};

const epd_epdopt_t EPD_EPDOPT_BALANCED_FAST = {
    .palette = EPD_PALETTE_EPDOPT_AITJCIZE,

    .tone_enabled = true,
    .tone_mode = EPD_TONE_MODE_CONTRAST,
    .exposure = 0.0,
    .saturation = 0.0,
    .contrast = 0.0,
    .strength = 0.0,
    .shadow_boost = 0.0,
    .highlight_compress = -1.5,
    .midpoint = 0.5,

    .range_mode = EPD_RANGE_MODE_DISPLAY,
    .range_quality = EPD_RANGE_FAST,
    .range_strength = 1.0,
    .low_percentile = 0.01,
    .high_percentile = 0.99,

    .kernel = &EPD_DIFFUSION_FLOYD_STEINBERG,
    .serpentine = false,
};

// -------------------------------------------------------------------------- helpers

static double clamp_d(double value, double min, double max)
{
    return value < min ? min : (value > max ? max : value);
}

// processing.ts:441-448. Not the same function as dither.ts:1574's smoothstep, which
// omits the degenerate-edge guard; this is the one the range compressor uses.
static double smoothstep(double edge0, double edge1, double value)
{
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0 : 0.0;
    }
    const double x = clamp_d((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// JavaScript's Math.round, which is floor(v + 0.5) -- and unlike epd_clamp_byte()'s use of
// it, this one can see a negative argument, where floor(-0.5 + 0.5) is 0 and lround()
// would give -1.
static double js_round(double value)
{
    return floor(value + 0.5);
}

// dither.ts:981-1002, rgb branch. The reference compares Euclidean distances and this
// compares their squares; sqrt is monotonic, so the winner -- and every tie, since both
// use a strict `<` that keeps the earlier entry -- is identical. Palette order therefore
// decides ties, which is why the EPD_PALETTE_EPDOPT_* tables are in the library's role
// order rather than this project's.
static const epd_palette_entry_t *nearest_entry(const epd_palette_entry_t *palette,
                                                int32_t r, int32_t g, int32_t b)
{
    const epd_palette_entry_t *best = &palette[0];
    int32_t best_dist = INT32_MAX;

    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const int32_t dr = (int32_t)palette[i].r - r;
        const int32_t dg = (int32_t)palette[i].g - g;
        const int32_t db = (int32_t)palette[i].b - b;
        const int32_t dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = &palette[i];
        }
    }
    return best;
}

// ---------------------------------------------------------------------- tone mapping

// processing.ts:114-127.
static double exposure_to_multiplier(double adjustment)
{
    return pow(2.0, adjustment);
}

static double linear_to_multiplier(double adjustment)
{
    return fmax(0.0, adjustment + 1.0);
}

static double contrast_to_multiplier(double adjustment)
{
    return adjustment < 0.0 ? fmax(0.5, 1.0 + adjustment * 0.5) : adjustment + 1.0;
}

// processing.ts:637-669.
static void build_scurve_lookup(double strength, double shadow_boost,
                                double highlight_boost, double midpoint, uint8_t out[256])
{
    // SHADOW_TONE_RESPONSE, processing.ts:313.
    const double shadow_tone_response = 1.5;
    const double mid = clamp_d(midpoint, 0.01, 0.99);
    const double shadow_exponent =
        clamp_d(1.0 - strength * shadow_boost * shadow_tone_response, 0.15, 3.0);
    const double highlight_exponent = clamp_d(1.0 - strength * highlight_boost, 0.15, 3.0);

    for (int value = 0; value < 256; value++) {
        const double normalized = (double)value / 255.0;
        double result;

        if (normalized <= mid) {
            result = pow(normalized / mid, shadow_exponent) * mid;
        } else {
            const double highlight = (normalized - mid) / (1.0 - mid);
            result = mid + pow(highlight, highlight_exponent) * (1.0 - mid);
        }

        out[value] = epd_clamp_byte(result * 255.0);
    }
}

void epd_epdopt_tone_map(uint8_t *rgb, size_t pixels, const epd_epdopt_t *cfg)
{
    if (rgb == NULL || cfg == NULL || !cfg->tone_enabled) {
        return;
    }

    const double exposure = exposure_to_multiplier(cfg->exposure);
    const double saturation = linear_to_multiplier(cfg->saturation);
    const double contrast = contrast_to_multiplier(cfg->contrast);
    const epd_tone_mode_t mode = cfg->tone_mode;

    // processing.ts:1116-1125: the S-curve runs for mode `scurve` and for an unset mode,
    // and only when its strength is non-zero.
    const bool has_scurve =
        (mode == EPD_TONE_MODE_UNSET || mode == EPD_TONE_MODE_SCURVE) && cfg->strength != 0.0;

    uint8_t exposure_lut[256];
    uint8_t tone_lut[256];
    uint8_t scurve_lut[256];

    if (has_scurve) {
        build_scurve_lookup(cfg->strength, cfg->shadow_boost, cfg->highlight_compress,
                            cfg->midpoint, scurve_lut);
    }

    for (int value = 0; value < 256; value++) {
        exposure_lut[value] = epd_clamp_byte((double)value * exposure);

        double tone_value = (double)value;
        if (mode != EPD_TONE_MODE_OFF) {
            if (mode == EPD_TONE_MODE_UNSET || mode == EPD_TONE_MODE_CONTRAST) {
                tone_value = epd_clamp_byte((tone_value - 128.0) * contrast + 128.0);
            }
            if (has_scurve) {
                tone_value = scurve_lut[(int)tone_value];
            }
        }
        tone_lut[value] = epd_clamp_byte(tone_value);
    }

    // processing.ts:1160-1218. The saturation == 1 case is not an optimisation of the
    // general one: the general path round-trips through HSL and back, and that round trip
    // is lossy, so taking it with a neutral saturation would change pixels. The reference
    // branches for exactly this reason and so does this.
    for (size_t i = 0; i < pixels; i++) {
        uint8_t *p = &rgb[i * 3];

        if (saturation == 1.0) {
            p[0] = tone_lut[exposure_lut[p[0]]];
            p[1] = tone_lut[exposure_lut[p[1]]];
            p[2] = tone_lut[exposure_lut[p[2]]];
            continue;
        }

        const double r0 = (double)exposure_lut[p[0]] / 255.0;
        const double g0 = (double)exposure_lut[p[1]] / 255.0;
        const double b0 = (double)exposure_lut[p[2]] / 255.0;

        const double max = fmax(fmax(r0, g0), b0);
        const double min = fmin(fmin(r0, g0), b0);
        const double lightness = (max + min) / 2.0;
        double r = r0;
        double g = g0;
        double b = b0;

        if (max != min) {
            const double delta = max - min;
            const double sat = lightness > 0.5 ? delta / (2.0 - max - min)
                                               : delta / fmax(max + min, 0.000001);
            double hue;
            if (max == r0) {
                hue = ((g0 - b0) / delta + (g0 < b0 ? 6.0 : 0.0)) / 6.0;
            } else if (max == g0) {
                hue = ((b0 - r0) / delta + 2.0) / 6.0;
            } else {
                hue = ((r0 - g0) / delta + 4.0) / 6.0;
            }

            const double new_sat = clamp_d(sat * saturation, 0.0, 1.0);
            const double c = (1.0 - fabs(2.0 * lightness - 1.0)) * new_sat;
            const double x = c * (1.0 - fabs(fmod(hue * 6.0, 2.0) - 1.0));
            const double m = lightness - c / 2.0;
            const int sector = (int)floor(hue * 6.0);

            switch (sector) {
            case 0:
                r = c + m; g = x + m; b = m;
                break;
            case 1:
                r = x + m; g = c + m; b = m;
                break;
            case 2:
                r = m; g = c + m; b = x + m;
                break;
            case 3:
                r = m; g = x + m; b = c + m;
                break;
            case 4:
                r = x + m; g = m; b = c + m;
                break;
            default:
                r = c + m; g = m; b = x + m;
                break;
            }
        }

        p[0] = tone_lut[epd_clamp_byte(r * 255.0)];
        p[1] = tone_lut[epd_clamp_byte(g * 255.0)];
        p[2] = tone_lut[epd_clamp_byte(b * 255.0)];
    }
}

// ----------------------------------------------------------------- range compression

// processing.ts:786-816, for the case both endpoints are unspecified: the darkest and
// lightest palette entries by Rec. 709 luma, first-wins on a tie.
static void palette_endpoints(const epd_palette_entry_t *palette,
                              const epd_palette_entry_t **black,
                              const epd_palette_entry_t **white)
{
    const epd_palette_entry_t *darkest = &palette[0];
    const epd_palette_entry_t *lightest = &palette[0];

    for (size_t i = 1; i < EPD_PALETTE_COUNT; i++) {
        const double luma = epd_luma709(palette[i].r, palette[i].g, palette[i].b);
        if (luma < epd_luma709(darkest->r, darkest->g, darkest->b)) {
            darkest = &palette[i];
        }
        if (luma > epd_luma709(lightest->r, lightest->g, lightest->b)) {
            lightest = &palette[i];
        }
    }

    *black = darkest;
    *white = lightest;
}

// processing.ts:450-451.
static double chroma_protection(double r, double g, double b)
{
    return smoothstep(0.18, 0.68, epd_saturation(r, g, b)) * 0.85;
}

// processing.ts:714-728.
static bool is_protected_chroma_fit(double source_luma, uint8_t r, uint8_t g, uint8_t b,
                                    double source_saturation)
{
    if (source_saturation < 0.16) {
        return true;
    }

    const double result_saturation = epd_saturation(r, g, b);
    const double minimum_saturation = fmax(0.12, source_saturation * 0.72);
    if (result_saturation >= minimum_saturation) {
        return true;
    }

    return epd_luma709(r, g, b) <= source_luma + 4.0;
}

// processing.ts:730-784. Compressing lightness towards the panel's white desaturates a
// saturated colour, so when it would, the amount is bisected down until it stops -- five
// steps, and if none of them is acceptable the pixel is left exactly as it was.
static void lab_to_rgb_with_chroma_guard(uint8_t source_r, uint8_t source_g,
                                         uint8_t source_b, double source_l, double a,
                                         double b, double target_l, double amount,
                                         uint8_t out[3])
{
    const double source_saturation = epd_saturation(source_r, source_g, source_b);
    const double source_luma = epd_luma709(source_r, source_g, source_b);

    uint8_t r, g, bb;
    epd_lab_to_rgb(source_l + (target_l - source_l) * amount, a, b, &r, &g, &bb);

    if (target_l <= source_l ||
        is_protected_chroma_fit(source_luma, r, g, bb, source_saturation)) {
        out[0] = r;
        out[1] = g;
        out[2] = bb;
        return;
    }

    double low = 0.0;
    double high = amount;
    out[0] = source_r;
    out[1] = source_g;
    out[2] = source_b;

    // CHROMA_GUARD_STEPS, processing.ts:712.
    for (int step = 0; step < 5; step++) {
        const double mid = (low + high) / 2.0;
        uint8_t cr, cg, cb;
        epd_lab_to_rgb(source_l + (target_l - source_l) * mid, a, b, &cr, &cg, &cb);

        if (is_protected_chroma_fit(source_luma, cr, cg, cb, source_saturation)) {
            low = mid;
            out[0] = cr;
            out[1] = cg;
            out[2] = cb;
        } else {
            high = mid;
        }
    }
}

// processing.ts:671-690. Bins are hundredths of an L* unit, so 10001 of them.
#define LIGHTNESS_HISTOGRAM_SCALE 100
#define LIGHTNESS_HISTOGRAM_BINS (100 * LIGHTNESS_HISTOGRAM_SCALE + 1)

static double percentile_from_histogram(const uint32_t *histogram, size_t bins,
                                        uint32_t count, double p)
{
    if (count == 0) {
        return 0.0;
    }

    const double target = clamp_d(js_round((double)(count - 1) * p), 0.0, (double)(count - 1));
    double seen = 0.0;

    for (size_t index = 0; index < bins; index++) {
        seen += histogram[index];
        if (seen > target) {
            return (double)index / (double)LIGHTNESS_HISTOGRAM_SCALE;
        }
    }

    return 100.0;
}

// processing.ts:692-710.
static double percentile_from_byte_histogram(const uint32_t *histogram, uint32_t count,
                                             double p)
{
    if (count == 0) {
        return 0.0;
    }

    const double target = clamp_d(js_round((double)(count - 1) * p), 0.0, (double)(count - 1));
    double seen = 0.0;

    for (size_t index = 0; index < 256; index++) {
        seen += histogram[index];
        if (seen > target) {
            return (double)index;
        }
    }

    return 255.0;
}

// processing.ts:912-975. No L*a*b*, no pow(), no cbrt(): the whole pass is one Rec. 709
// luma per pixel and a ratio. It is a different algorithm from the accurate path rather
// than an approximation of it -- it scales all three channels by a common factor, so it
// cannot desaturate the way a lightness move in L*a*b* does, and it needs no chroma guard
// for the same reason.
static void range_compress_fast(uint8_t *rgb, size_t pixels, const epd_epdopt_t *cfg)
{
    const double strength = clamp_d(cfg->range_strength, 0.0, 1.0);

    const epd_palette_entry_t *black;
    const epd_palette_entry_t *white;
    palette_endpoints(cfg->palette, &black, &white);

    const double black_y = epd_luma709(black->r, black->g, black->b);
    const double white_y = epd_luma709(white->r, white->g, white->b);
    const double target_range = white_y - black_y;
    if (target_range <= 0.0) {
        return;
    }

    double source_black_y = 0.0;
    double source_white_y = 255.0;

    if (cfg->range_mode == EPD_RANGE_MODE_AUTO) {
        uint32_t histogram[256] = {0};
        uint32_t count = 0;
        for (size_t i = 0; i < pixels; i++) {
            const uint8_t *p = &rgb[i * 3];
            histogram[epd_clamp_byte(epd_luma709(p[0], p[1], p[2]))] += 1;
            count += 1;
        }
        source_black_y = percentile_from_byte_histogram(histogram, count, cfg->low_percentile);
        source_white_y = percentile_from_byte_histogram(histogram, count, cfg->high_percentile);
    }

    const double source_range = source_white_y - source_black_y;
    if (source_range <= 0.0001) {
        return;
    }

    for (size_t i = 0; i < pixels; i++) {
        uint8_t *p = &rgb[i * 3];
        const double r = p[0];
        const double g = p[1];
        const double b = p[2];
        const double y = epd_luma709(r, g, b);
        const double normalized_y = clamp_d((y - source_black_y) / source_range, 0.0, 1.0);
        const double target_y = black_y + normalized_y * target_range;
        const double effective_strength = strength * (1.0 - chroma_protection(r, g, b));
        const double next_y = y + (target_y - y) * effective_strength;

        double ratio = y > 0.0 ? next_y / y : 0.0;
        const double max_channel = fmax(fmax(r, g), b);
        if (max_channel > 0.0) {
            ratio = fmin(ratio, 255.0 / max_channel);
        }

        p[0] = epd_clamp_byte(r * ratio);
        p[1] = epd_clamp_byte(g * ratio);
        p[2] = epd_clamp_byte(b * ratio);
    }
}

bool epd_epdopt_range_compress(uint8_t *rgb, size_t pixels, const epd_epdopt_t *cfg)
{
    if (rgb == NULL || cfg == NULL || cfg->palette == NULL ||
        cfg->range_mode == EPD_RANGE_MODE_OFF) {
        return true;
    }

    const double strength = clamp_d(cfg->range_strength, 0.0, 1.0);
    if (strength == 0.0) {
        return true;
    }

    if (cfg->range_quality == EPD_RANGE_FAST) {
        range_compress_fast(rgb, pixels, cfg);
        return true;
    }

    const epd_palette_entry_t *black;
    const epd_palette_entry_t *white;
    palette_endpoints(cfg->palette, &black, &white);

    double black_l, black_a, black_b;
    double white_l, white_a, white_b;
    epd_rgb_to_lab(black->r, black->g, black->b, &black_l, &black_a, &black_b);
    epd_rgb_to_lab(white->r, white->g, white->b, &white_l, &white_a, &white_b);

    const double target_range = white_l - black_l;
    if (target_range <= 0.0) {
        return true;
    }

    double source_black_l = 0.0;
    double source_white_l = 100.0;

    if (cfg->range_mode == EPD_RANGE_MODE_AUTO) {
        // 40 KB, and internal RAM is what this project runs out of first. Heap rather
        // than static so a build that only ever uses EPD_RANGE_MODE_DISPLAY does not
        // carry it, and a hard failure rather than a fallback so a run that could not
        // measure the image cannot be mistaken for one that did.
        uint32_t *histogram = calloc(LIGHTNESS_HISTOGRAM_BINS, sizeof(uint32_t));
        if (histogram == NULL) {
            return false;
        }

        uint32_t count = 0;
        for (size_t i = 0; i < pixels; i++) {
            const uint8_t *p = &rgb[i * 3];
            const double l = epd_rgb_to_lab_lightness(p[0], p[1], p[2]);
            const double bin = clamp_d(js_round(l * LIGHTNESS_HISTOGRAM_SCALE), 0.0,
                                       LIGHTNESS_HISTOGRAM_BINS - 1);
            histogram[(size_t)bin] += 1;
            count += 1;
        }

        source_black_l = percentile_from_histogram(histogram, LIGHTNESS_HISTOGRAM_BINS,
                                                   count, cfg->low_percentile);
        source_white_l = percentile_from_histogram(histogram, LIGHTNESS_HISTOGRAM_BINS,
                                                  count, cfg->high_percentile);
        free(histogram);
    }

    const double source_range = source_white_l - source_black_l;
    if (source_range <= 0.0001) {
        return true;
    }

    for (size_t i = 0; i < pixels; i++) {
        uint8_t *p = &rgb[i * 3];
        const uint8_t r = p[0];
        const uint8_t g = p[1];
        const uint8_t b = p[2];

        double l, a, bb;
        epd_rgb_to_lab(r, g, b, &l, &a, &bb);

        const double normalized_l = clamp_d((l - source_black_l) / source_range, 0.0, 1.0);
        const double compressed_l = black_l + normalized_l * target_range;
        const double effective_strength = strength * (1.0 - chroma_protection(r, g, b));

        uint8_t out[3];
        lab_to_rgb_with_chroma_guard(r, g, b, l, a, bb, compressed_l, effective_strength,
                                     out);
        p[0] = out[0];
        p[1] = out[1];
        p[2] = out[2];
    }

    return true;
}

// ------------------------------------------------------------------- error diffusion

void epd_epdopt_diffuse(epd_canvas_t *c, const epd_epdopt_t *cfg)
{
    if (c == NULL || c->rgb == NULL || cfg == NULL || cfg->palette == NULL ||
        cfg->kernel == NULL || cfg->kernel->taps == NULL) {
        return;
    }

    const int32_t width = epd_canvas_logical_width(c);
    const int32_t height = epd_canvas_logical_height(c);

    for (int32_t y = 0; y < height; y++) {
        const bool reverse = cfg->serpentine && (y % 2 == 1);
        const int32_t x_start = reverse ? width - 1 : 0;
        const int32_t x_end = reverse ? -1 : width;
        const int32_t x_step = reverse ? -1 : 1;

        for (int32_t x = x_start; x != x_end; x += x_step) {
            uint8_t *px = epd_canvas_pixel(c, x, y);
            if (px == NULL) {
                continue;
            }

            const int32_t old_r = px[0];
            const int32_t old_g = px[1];
            const int32_t old_b = px[2];
            const epd_palette_entry_t *chosen =
                nearest_entry(cfg->palette, old_r, old_g, old_b);

            px[0] = chosen->r;
            px[1] = chosen->g;
            px[2] = chosen->b;

            const double error_r = (double)(old_r - (int32_t)chosen->r);
            const double error_g = (double)(old_g - (int32_t)chosen->g);
            const double error_b = (double)(old_b - (int32_t)chosen->b);

            for (size_t t = 0; t < cfg->kernel->count; t++) {
                const epd_diffusion_tap_t tap = cfg->kernel->taps[t];
                const int32_t dx = reverse ? -(int32_t)tap.dx : (int32_t)tap.dx;
                const int32_t nx = x + dx;
                const int32_t ny = y + (int32_t)tap.dy;
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                    continue;
                }

                uint8_t *np = epd_canvas_pixel(c, nx, ny);
                if (np == NULL) {
                    continue;
                }

                // The error lands back in the image as clamped bytes, not in a float
                // error plane. That is lossy -- every neighbour is rounded at each of its
                // up-to-four updates, and the loss is part of the reference's output. A
                // float plane here would be a better dither and a failing parity test.
                np[0] = epd_clamp_byte((double)np[0] + error_r * tap.factor);
                np[1] = epd_clamp_byte((double)np[1] + error_g * tap.factor);
                np[2] = epd_clamp_byte((double)np[2] + error_b * tap.factor);
            }
        }
    }
}

bool epd_epdopt_render(epd_canvas_t *c, const epd_epdopt_t *cfg, bool diffuse)
{
    if (c == NULL || c->rgb == NULL || cfg == NULL) {
        return false;
    }

    // processing.ts:1225-1249 fixes this order: tone mapping, then range compression.
    // Reversing it would compress a range the tone curve then moves back out of.
    const size_t pixels = (size_t)c->width * (size_t)c->height;

    epd_epdopt_tone_map(c->rgb, pixels, cfg);
    if (!epd_epdopt_range_compress(c->rgb, pixels, cfg)) {
        return false;
    }
    if (diffuse) {
        epd_epdopt_diffuse(c, cfg);
    }
    return true;
}
