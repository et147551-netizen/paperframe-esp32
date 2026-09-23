// Ported from paperlesspaper/epdoptimize, src/dither/processing.ts.
// Copyright the epdoptimize authors, licensed Apache-2.0; see LICENSES/epdoptimize-Apache-2.0.txt.

#include "epd_colour.h"

#include <math.h>

// processing.ts:323-333. The reference builds this table once at module load and then
// indexes it with a byte, so the linearisation is table-exact rather than recomputed --
// which matters here because pow() on two toolchains need not agree to the last bit, and
// building the table once means any such difference is frozen into 256 values instead of
// varying per pixel.
//
// Built on first use rather than at compile time because C cannot call pow() in a static
// initialiser. If two tasks race here they compute identical values into the same array,
// so the race is benign; there is no path where a half-built table is observable, because
// the flag is set last and every entry is written before it.
static double s_srgb_to_linear[256];
static bool s_srgb_ready;

static void ensure_srgb_table(void)
{
    if (s_srgb_ready) {
        return;
    }
    for (int value = 0; value < 256; value++) {
        const double normalized = (double)value / 255.0;
        s_srgb_to_linear[value] = normalized > 0.04045
                                      ? pow((normalized + 0.055) / 1.055, 2.4)
                                      : normalized / 12.92;
    }
    s_srgb_ready = true;
}

uint8_t epd_clamp_byte(double value)
{
    if (!isfinite(value)) {
        return 0;
    }
    // Clamp, then round. See the header for why this is floor(v + 0.5).
    const double clamped = value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value);
    return (uint8_t)floor(clamped + 0.5);
}

double epd_luma709(double r, double g, double b)
{
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double epd_saturation(double r, double g, double b)
{
    const double max = fmax(fmax(r, g), b) / 255.0;
    const double min = fmin(fmin(r, g), b) / 255.0;
    return max == 0.0 ? 0.0 : (max - min) / max;
}

// processing.ts:335-336.
static double lab_forward_pivot(double value)
{
    return value > 0.008856 ? cbrt(value) : 7.787 * value + 16.0 / 116.0;
}

// processing.ts:400-402. The inverse pivot's threshold is expressed in the pivoted domain
// -- 0.206897 is cbrt(0.008856) truncated -- so it is not exactly the forward threshold's
// mirror. Kept as written.
static double lab_inverse_pivot(double value)
{
    return value > 0.206897 ? value * value * value : (value - 16.0 / 116.0) / 7.787;
}

void epd_rgb_to_lab(uint8_t r, uint8_t g, uint8_t b, double *l, double *a, double *bb)
{
    ensure_srgb_table();

    const double rn = s_srgb_to_linear[r];
    const double gn = s_srgb_to_linear[g];
    const double bn = s_srgb_to_linear[b];

    // processing.ts:370-380: XYZ is scaled to 0-100 here and divided by the white point
    // below, so the 100s do not cancel out into a simpler form -- 100/95.047 is not 1.
    const double x = (rn * 0.4124564 + gn * 0.3575761 + bn * 0.1804375) * 100.0;
    const double y = (rn * 0.2126729 + gn * 0.7151522 + bn * 0.072175) * 100.0;
    const double z = (rn * 0.0193339 + gn * 0.119192 + bn * 0.9503041) * 100.0;

    const double xp = lab_forward_pivot(x / 95.047);
    const double yp = lab_forward_pivot(y / 100.0);
    const double zp = lab_forward_pivot(z / 108.883);

    *l = 116.0 * yp - 16.0;
    *a = 500.0 * (xp - yp);
    *bb = 200.0 * (yp - zp);
}

double epd_rgb_to_lab_lightness(uint8_t r, uint8_t g, uint8_t b)
{
    ensure_srgb_table();

    // processing.ts:338-345. Y is computed without the *100 that rgbToXyz applies and
    // then pivoted directly, which is the same number: the /100 white point divide and
    // the *100 cancel exactly here, and the reference relies on that.
    const double y = s_srgb_to_linear[r] * 0.2126729 + s_srgb_to_linear[g] * 0.7151522 +
                     s_srgb_to_linear[b] * 0.072175;

    return 116.0 * lab_forward_pivot(y) - 16.0;
}

void epd_lab_to_rgb(double l, double a, double b, uint8_t *r, uint8_t *g, uint8_t *bb)
{
    // processing.ts:395-405.
    const double y0 = (l + 16.0) / 116.0;
    const double x0 = a / 500.0 + y0;
    const double z0 = y0 - b / 200.0;

    const double x = lab_inverse_pivot(x0) * 95.047;
    const double y = lab_inverse_pivot(y0) * 100.0;
    const double z = lab_inverse_pivot(z0) * 108.883;

    // processing.ts:407-421.
    const double xn = x / 100.0;
    const double yn = y / 100.0;
    const double zn = z / 100.0;

    double lr = xn * 3.2404542 + yn * -1.5371385 + zn * -0.4985314;
    double lg = xn * -0.969266 + yn * 1.8760108 + zn * 0.041556;
    double lb = xn * 0.0556434 + yn * -0.2040259 + zn * 1.0572252;

    // The gamma encode is applied to the unclamped linear value, so a negative channel
    // takes the 12.92 branch and comes out negative, and epd_clamp_byte() floors it to 0.
    // Clamping to 0-1 first would give a different byte for out-of-gamut colours.
    lr = lr > 0.0031308 ? 1.055 * pow(lr, 1.0 / 2.4) - 0.055 : 12.92 * lr;
    lg = lg > 0.0031308 ? 1.055 * pow(lg, 1.0 / 2.4) - 0.055 : 12.92 * lg;
    lb = lb > 0.0031308 ? 1.055 * pow(lb, 1.0 / 2.4) - 0.055 : 12.92 * lb;

    *r = epd_clamp_byte(lr * 255.0);
    *g = epd_clamp_byte(lg * 255.0);
    *bb = epd_clamp_byte(lb * 255.0);
}
