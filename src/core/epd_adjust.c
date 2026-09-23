// The single-precision rewrite of the per-pixel stages ported from paperlesspaper/epdoptimize
// (src/dither/processing.ts and src/dither/dither.ts). Copyright the epdoptimize authors,
// licensed Apache-2.0; see LICENSES/epdoptimize-Apache-2.0.txt.
//
// Tone mapping and range compression are kept structurally line-for-line with src/core/epd_epdopt.c
// so the two can be read side by side: the point of those two is that only the types changed,
// and a reader has to be able to check that claim. Where they deviate -- the clamp, and AUTO
// mode's histogram bins -- the comment says so at the point of deviation.
//
// **Three more stages arrived with the auto flow and they have no counterpart in
// epd_epdopt.c**: paper normalisation, luma level compression, and white preservation. The
// two-level chain the first two enjoy therefore does not exist for them, so
// test/test_adjust/ pins them straight to the library's own bytes within a stated tolerance
// rather than to a double port written only to be compared against. Writing three more parity
// ports to have something to compare against would be three more things to keep correct for no
// gain that reaches the panel.

#include "epd_adjust.h"

#include <math.h>
#include <string.h>

// **`-O2` was tried on these two functions and made the pair slower. Do not try it again
// without reading this.**
//
// The project compiles at `-Og` (`CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` in every generated
// sdkconfig, from IDF's default rather than from sdkconfig.defaults), so every timing figure
// this repository holds is an `-Og` figure. That made the optimisation level the obvious suspect
// for the range stage costing ~1050 cycles a pixel for about fifteen float operations. It is
// not the cause. Measured on hardware 2026-09-05, `env:photo`, ten runs each, one variable --
// `__attribute__((optimize("O2")))` on both functions.
//
// **All the millisecond figures in this file are from that day's 160 MHz / 32-byte-cache-line
// build**, which is not what the project builds now: the two stages together cost 668.9 ms at
// 240 MHz with 64-byte lines. They are left as taken -- each belongs to an arm that isolated one
// variable, and rescaling them by a clock ratio afterwards would turn measurements into
// estimates. What the arms establish is *which* thing cost what, and that does not move:
//
//   | build | tone_ms | range_ms | total_ms |
//   | -Og   |    66.7 |   1581.0 |   1647.7 |
//   | -O2   |    43.0 |   1672.6 |   1715.6 |
//
// So `-O2` buys 1.55x on the tone stage and loses 5.8 % on the range stage, for a net loss. The
// arm is trustworthy in both directions: the tone figure moving proves the attribute reached the
// compiler, and `adj-double` -- which lives in epd_epdopt.c and was not touched -- reported
// 12397.1 ms against 12396.8 ms in the previous run, so nothing else moved between them.
//
// The cause was in `nm -u` all along: **`fmaxf` and `fminf` are external calls**, not
// instructions, because without `-ffast-math` GCC must preserve their NaN semantics. The range
// stage made seven of them per pixel -- 1.7 million calls a photograph. Replacing them with
// comparisons (max_f/min_f below) took the same arm, same conditions, ten runs:
//
//   | build              | tone_ms | range_ms | total_ms |
//   | -Og, fmaxf/fminf   |    66.7 |   1581.0 |   1647.7 |
//   | -Og, max_f/min_f   |    66.7 |    944.0 |   1010.7 |
//
// **The tone figure is the control and it did not move at all**: the balanced preset takes the
// LUT path, which calls neither, so an identical 66.7 ms is what says the 40 % came from the
// change and not from the weather. Against the double port's 12 514 ms, this is now 12.4x.
//
// What is left is ~630 cycles a pixel, and the remaining suspect is the two `__divsf3` calls --
// **this SoC's FPU has no divide instruction**. The saturation one is exactly removable, because
// its divisor is an integer 0-255 and a 256-entry reciprocal table indexes it without changing a
// value. It has not been done: 944 ms is already well inside the 2 s the plan pre-registered as
// the point where inline application would be refused, and an unnecessary table is a thing that
// can be wrong.
//
// One more thing this does *not* generalise to: `src/core/epd_dither.c`, the row-wise quantiser that
// actually ships, contains no fmax/fmin at all -- it is integer throughout -- so the finding buys
// it nothing and there is no project-wide sweep waiting here. `epd_epdopt.c` and `epd_colour.c`
// do use them and must keep them: those are the byte-for-byte port, where a NaN-semantics change
// is a parity risk for no gain on a host.

const epd_adjust_t EPD_ADJUST_BALANCED = {
    .palette = EPD_PALETTE_EPDOPT_AITJCIZE,

    .tone_enabled = true,
    .tone_mode = EPD_TONE_MODE_CONTRAST,
    .exposure = 0.0f,
    .saturation = 0.0f,
    .contrast = 0.0f,
    .strength = 0.0f,
    .shadow_boost = 0.0f,
    .highlight_compress = -1.5f,
    .midpoint = 0.5f,

    .range_mode = EPD_RANGE_MODE_DISPLAY,
    .range_strength = 1.0f,
    .low_percentile = 0.01f,
    .high_percentile = 0.99f,
};

// -------------------------------------------------------------------------- helpers

static inline float clamp_f(float value, float min, float max)
{
    return value < min ? min : (value > max ? max : value);
}

// **These exist because `fmaxf` and `fminf` are function calls on this toolchain**, and the
// range stage below wanted seven of them per pixel -- 1.7 million calls a photograph. GCC will
// not inline them without `-ffast-math`, because a NaN argument has to come back as the other
// operand and `?:` does not do that.
//
// Safe here, and only because of what the arguments are: every value that reaches these is
// derived from image bytes, so it is finite and in 0-255 or a product of such values. There is
// no path that can hand them a NaN. Do not lift them into a header for general use.
static inline float max_f(float a, float b)
{
    return a > b ? a : b;
}

static inline float min_f(float a, float b)
{
    return a < b ? a : b;
}

// **The one deliberate deviation from epd_clamp_byte(), and it is worth naming.** That
// function does an isfinite() and a floor() per channel, twelve times per pixel in the
// diffusion pass, and the profile is why this file exists at all. Written as `!(v > 0.0f)`
// rather than `v <= 0.0f` so a NaN takes the zero branch instead of reaching the cast, where
// the conversion would be undefined -- the isfinite() check earns its keep in exactly one
// place and this is how it is paid for.
static inline uint8_t clamp_byte_f(float value)
{
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t)(int32_t)(value + 0.5f);
}

// processing.ts:320-321. Rec. 709 luma.
static inline float luma709_f(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// processing.ts:435-439. HSV-style saturation on 0-255 channels.
static inline float saturation_f(float r, float g, float b)
{
    const float max = max_f(max_f(r, g), b);
    const float min = min_f(min_f(r, g), b);
    return max == 0.0f ? 0.0f : (max - min) / max;
}

// processing.ts:441-448, the range compressor's smoothstep with its degenerate-edge guard.
static float smoothstep_f(float edge0, float edge1, float value)
{
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0f : 0.0f;
    }
    const float x = clamp_f((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// processing.ts:450-451.
static inline float chroma_protection_f(float r, float g, float b)
{
    return smoothstep_f(0.18f, 0.68f, saturation_f(r, g, b)) * 0.85f;
}

// ---------------------------------------------------------------------- tone mapping
//
// processing.ts:114-127.

static inline float exposure_to_multiplier(float adjustment)
{
    return powf(2.0f, adjustment);
}

static inline float linear_to_multiplier(float adjustment)
{
    return max_f(0.0f, adjustment + 1.0f);
}

static inline float contrast_to_multiplier(float adjustment)
{
    return adjustment < 0.0f ? max_f(0.5f, 1.0f + adjustment * 0.5f) : adjustment + 1.0f;
}

// processing.ts:637-669. 256 powf() calls, once per image: at the measured ~53 us a pixel for
// the old per-pixel double work, the whole of this costs less than five pixels did.
static void build_scurve_lookup(float strength, float shadow_boost, float highlight_boost,
                                float midpoint, uint8_t out[256])
{
    // SHADOW_TONE_RESPONSE, processing.ts:313.
    const float shadow_tone_response = 1.5f;
    const float mid = clamp_f(midpoint, 0.01f, 0.99f);
    const float shadow_exponent =
        clamp_f(1.0f - strength * shadow_boost * shadow_tone_response, 0.15f, 3.0f);
    const float highlight_exponent = clamp_f(1.0f - strength * highlight_boost, 0.15f, 3.0f);

    for (int value = 0; value < 256; value++) {
        const float normalized = (float)value / 255.0f;
        float result;

        if (normalized <= mid) {
            result = powf(normalized / mid, shadow_exponent) * mid;
        } else {
            const float highlight = (normalized - mid) / (1.0f - mid);
            result = mid + powf(highlight, highlight_exponent) * (1.0f - mid);
        }

        out[value] = clamp_byte_f(result * 255.0f);
    }
}

void epd_adjust_tone(uint8_t *rgb, size_t pixels, const epd_adjust_t *cfg)
{
    epd_adjust_tone_region(rgb, pixels, 1, pixels * 3, cfg);
}

void epd_adjust_tone_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                            const epd_adjust_t *cfg)
{
    if (rgb == NULL || cfg == NULL || !cfg->tone_enabled || width == 0 || height == 0) {
        return;
    }

    const float exposure = exposure_to_multiplier(cfg->exposure);
    const float saturation = linear_to_multiplier(cfg->saturation);
    const float contrast = contrast_to_multiplier(cfg->contrast);
    const epd_tone_mode_t mode = cfg->tone_mode;

    // processing.ts:1116-1125: the S-curve runs for mode `scurve` and for an unset mode, and
    // only when its strength is non-zero.
    const bool has_scurve =
        (mode == EPD_TONE_MODE_UNSET || mode == EPD_TONE_MODE_SCURVE) && cfg->strength != 0.0f;

    uint8_t exposure_lut[256];
    uint8_t tone_lut[256];
    uint8_t scurve_lut[256];

    if (has_scurve) {
        build_scurve_lookup(cfg->strength, cfg->shadow_boost, cfg->highlight_compress,
                            cfg->midpoint, scurve_lut);
    }

    for (int value = 0; value < 256; value++) {
        exposure_lut[value] = clamp_byte_f((float)value * exposure);

        float tone_value = (float)value;
        if (mode != EPD_TONE_MODE_OFF) {
            if (mode == EPD_TONE_MODE_UNSET || mode == EPD_TONE_MODE_CONTRAST) {
                tone_value = (float)clamp_byte_f((tone_value - 128.0f) * contrast + 128.0f);
            }
            if (has_scurve) {
                tone_value = (float)scurve_lut[(int)tone_value];
            }
        }
        tone_lut[value] = clamp_byte_f(tone_value);
    }

    // The neutral-saturation branch is the whole reason the tone stage was never the
    // expensive one: it is three table lookups per pixel and no arithmetic at all. It is not
    // an optimisation of the path below -- that one round-trips through HSL, which is lossy,
    // so taking it with a neutral saturation would change pixels (processing.ts:1160-1218).
    if (saturation == 1.0f) {
        for (size_t row = 0; row < height; row++) {
            uint8_t *line = rgb + row * stride;
            for (size_t x = 0; x < width; x++) {
                uint8_t *p = &line[x * 3];
                p[0] = tone_lut[exposure_lut[p[0]]];
                p[1] = tone_lut[exposure_lut[p[1]]];
                p[2] = tone_lut[exposure_lut[p[2]]];
            }
        }
        return;
    }

    // **The expensive branch, and the one the auto flow usually takes: 708.7 ms against the LUT
    // path's 38.1 ms**, measured on hardware 2026-09-05 (`env:photo`, ten runs, capture
    // `photo-auto-20260905-232550.log`). Stage 0's figure was EPD_ADJUST_BALANCED's, whose
    // saturation is neutral, so it took the path above; epd_auto.c asks for a non-neutral
    // saturation for six of the seven kinds -- including `photo` when lumaStdDev <= 42, which is
    // an ordinary photograph -- and every one of those lands here. It is 37 % of the whole auto
    // flow. The arm's own control is that `range_ms` moved 0.1 ms while this moved 18.6x, the two
    // configs differing in exactly one field.
    //
    // Three divisions, an fmodf, a floorf and two fabsf per pixel. Two of those are removable
    // exactly -- `fmodf(hue * 6, 2)` on a value known to be in 0-6 is a subtraction of 2 or 4, and
    // `floorf` of a non-negative float is a cast -- and **neither is done**, because speed is not
    // being asked for at this stage and this is a working colour path. epd_adjust.h says the rest.
    for (size_t row = 0; row < height; row++) {
        uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            uint8_t *p = &line[col * 3];

            const float r0 = (float)exposure_lut[p[0]] / 255.0f;
            const float g0 = (float)exposure_lut[p[1]] / 255.0f;
            const float b0 = (float)exposure_lut[p[2]] / 255.0f;

            const float max = max_f(max_f(r0, g0), b0);
            const float min = min_f(min_f(r0, g0), b0);
            const float lightness = (max + min) / 2.0f;
            float r = r0;
            float g = g0;
            float b = b0;

            if (max != min) {
                const float delta = max - min;
                const float sat = lightness > 0.5f ? delta / (2.0f - max - min)
                                                   : delta / max_f(max + min, 0.000001f);
                float hue;
                if (max == r0) {
                    hue = ((g0 - b0) / delta + (g0 < b0 ? 6.0f : 0.0f)) / 6.0f;
                } else if (max == g0) {
                    hue = ((b0 - r0) / delta + 2.0f) / 6.0f;
                } else {
                    hue = ((r0 - g0) / delta + 4.0f) / 6.0f;
                }

                const float new_sat = clamp_f(sat * saturation, 0.0f, 1.0f);
                const float c = (1.0f - fabsf(2.0f * lightness - 1.0f)) * new_sat;
                const float xc = c * (1.0f - fabsf(fmodf(hue * 6.0f, 2.0f) - 1.0f));
                const float m = lightness - c / 2.0f;
                const int sector = (int)floorf(hue * 6.0f);

                switch (sector) {
                case 0:
                    r = c + m; g = xc + m; b = m;
                    break;
                case 1:
                    r = xc + m; g = c + m; b = m;
                    break;
                case 2:
                    r = m; g = c + m; b = xc + m;
                    break;
                case 3:
                    r = m; g = xc + m; b = c + m;
                    break;
                case 4:
                    r = xc + m; g = m; b = c + m;
                    break;
                default:
                    r = c + m; g = m; b = xc + m;
                    break;
                }
            }

            p[0] = tone_lut[clamp_byte_f(r * 255.0f)];
            p[1] = tone_lut[clamp_byte_f(g * 255.0f)];
            p[2] = tone_lut[clamp_byte_f(b * 255.0f)];
        }
    }
}

// ----------------------------------------------------------------- range compression

// processing.ts:786-816: the darkest and lightest palette entries by Rec. 709 luma,
// first-wins on a tie.
static void palette_endpoints(const epd_palette_entry_t *palette,
                              const epd_palette_entry_t **black,
                              const epd_palette_entry_t **white)
{
    const epd_palette_entry_t *darkest = &palette[0];
    const epd_palette_entry_t *lightest = &palette[0];

    for (size_t i = 1; i < EPD_PALETTE_COUNT; i++) {
        const float luma = luma709_f(palette[i].r, palette[i].g, palette[i].b);
        if (luma < luma709_f(darkest->r, darkest->g, darkest->b)) {
            darkest = &palette[i];
        }
        if (luma > luma709_f(lightest->r, lightest->g, lightest->b)) {
            lightest = &palette[i];
        }
    }

    *black = darkest;
    *white = lightest;
}

// processing.ts:692-710, over the 256-bin byte histogram the fast path already uses.
static float percentile_from_byte_histogram(const uint32_t *histogram, uint32_t count, float p)
{
    if (count == 0) {
        return 0.0f;
    }

    // The reference rounds here (floor(v + 0.5), JavaScript's Math.round) and this does not,
    // and that is exact rather than approximate: `seen` below only ever holds a sum of bin
    // counts, so it is integral, and for an integral s the tests `s > floor(t)` and `s > t`
    // select the same first bin for every t. Dropping the floorf() removes a call from a
    // function that runs twice per image, which is not the point -- the point is that it does
    // not change the answer, so it is not a deviation to test for.
    const float target = clamp_f((float)(count - 1) * p + 0.5f, 0.0f, (float)(count - 1));
    float seen = 0.0f;

    for (size_t index = 0; index < 256; index++) {
        seen += (float)histogram[index];
        if (seen > target) {
            return (float)index;
        }
    }

    return 255.0f;
}

void epd_adjust_range(uint8_t *rgb, size_t pixels, const epd_adjust_t *cfg)
{
    epd_adjust_range_region(rgb, pixels, 1, pixels * 3, cfg);
}

void epd_adjust_range_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                             const epd_adjust_t *cfg)
{
    if (rgb == NULL || cfg == NULL || cfg->palette == NULL || width == 0 || height == 0 ||
        cfg->range_mode == EPD_RANGE_MODE_OFF) {
        return;
    }

    const float strength = clamp_f(cfg->range_strength, 0.0f, 1.0f);
    if (strength == 0.0f) {
        return;
    }

    const epd_palette_entry_t *black;
    const epd_palette_entry_t *white;
    palette_endpoints(cfg->palette, &black, &white);

    const float black_y = luma709_f(black->r, black->g, black->b);
    const float white_y = luma709_f(white->r, white->g, white->b);
    const float target_range = white_y - black_y;
    if (target_range <= 0.0f) {
        return;
    }

    float source_black_y = 0.0f;
    float source_white_y = 255.0f;

    const size_t pixels = width * height;

    if (cfg->range_mode == EPD_RANGE_MODE_AUTO) {
        uint32_t histogram[256] = {0};
        for (size_t row = 0; row < height; row++) {
            const uint8_t *line = rgb + row * stride;
            for (size_t col = 0; col < width; col++) {
                const uint8_t *p = &line[col * 3];
                histogram[clamp_byte_f(luma709_f(p[0], p[1], p[2]))] += 1;
            }
        }
        source_black_y =
            percentile_from_byte_histogram(histogram, (uint32_t)pixels, cfg->low_percentile);
        source_white_y =
            percentile_from_byte_histogram(histogram, (uint32_t)pixels, cfg->high_percentile);
    }

    const float source_range = source_white_y - source_black_y;
    if (source_range <= 0.0001f) {
        return;
    }

    // Hoisted out of the loop: the reference divides by source_range per pixel, and a
    // reciprocal multiply is the same value to well within the tolerance test_adjust states.
    const float inv_source_range = 1.0f / source_range;

    for (size_t row = 0; row < height; row++) {
        uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            uint8_t *p = &line[col * 3];
            const float r = p[0];
            const float g = p[1];
            const float b = p[2];
            const float y = luma709_f(r, g, b);
            const float normalized_y =
                clamp_f((y - source_black_y) * inv_source_range, 0.0f, 1.0f);
            const float target_y = black_y + normalized_y * target_range;
            const float effective_strength = strength * (1.0f - chroma_protection_f(r, g, b));
            const float next_y = y + (target_y - y) * effective_strength;

            float ratio = y > 0.0f ? next_y / y : 0.0f;
            const float max_channel = max_f(max_f(r, g), b);
            if (max_channel > 0.0f) {
                ratio = min_f(ratio, 255.0f / max_channel);
            }

            p[0] = clamp_byte_f(r * ratio);
            p[1] = clamp_byte_f(g * ratio);
            p[2] = clamp_byte_f(b * ratio);
        }
    }
}

// ---------------------------------------------------------------- paper normalisation
//
// processing.ts:456-521. Three exclusive branches per pixel, tried in this order: red ink is
// pushed a little further towards red, dark neutral ink is anchored towards black, and warm
// paper is pulled towards a neutral white. Reached only by epd_auto.c's posterScan arm.

// processing.ts:441-442. **This one has no degenerate-edge guard**, unlike the namesake in
// image-style.ts that epd_classify.c ports -- upstream really does divide by (max - min) here.
// Safe because every call below passes a literal range with max > min.
static inline float normalize_f(float value, float min, float max)
{
    return clamp_f((value - min) / (max - min), 0.0f, 1.0f);
}

// processing.ts:453-454.
static inline bool is_red_ink(float r, float g, float b, float saturation)
{
    return saturation >= 0.34f && r >= g + 24.0f && r >= b + 28.0f;
}

void epd_adjust_paper_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                             const epd_paper_t *cfg)
{
    if (rgb == NULL || cfg == NULL || !cfg->enabled || width == 0 || height == 0) {
        return;
    }

    // Upstream fills each of these from `options.x ?? default` (:466-471). The port takes them
    // from the struct instead, because src/core/epd_auto.c is the only producer and sets all eight
    // explicitly -- there is no path here that can present a missing field.
    const float strength = clamp_f(cfg->strength, 0.0f, 1.0f);
    if (strength == 0.0f) {
        return;
    }
    const float min_luma = cfg->min_luma;
    const float saturation_threshold = cfg->saturation_threshold;
    const float warm_bias_threshold = cfg->warm_bias_threshold;
    const float black_anchor = clamp_f(cfg->black_anchor, 0.0f, 1.0f);
    const float preserve_red = clamp_f(cfg->preserve_red, 0.0f, 1.0f);
    const float paper_r = (float)cfg->paper_white[0];
    const float paper_g = (float)cfg->paper_white[1];
    const float paper_b = (float)cfg->paper_white[2];

    const float red_boost = strength * preserve_red;
    const float target_mix = 0.72f + 0.2f * strength;

    for (size_t row = 0; row < height; row++) {
        uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            uint8_t *p = &line[col * 3];
            const float r = p[0];
            const float g = p[1];
            const float b = p[2];
            const float luma = luma709_f(r, g, b);
            const float saturation = saturation_f(r, g, b);

            if (is_red_ink(r, g, b, saturation)) {
                p[0] = clamp_byte_f(r + (255.0f - r) * 0.08f * red_boost);
                p[1] = clamp_byte_f(g * (1.0f - 0.08f * red_boost));
                p[2] = clamp_byte_f(b * (1.0f - 0.12f * red_boost));
                continue;
            }

            const float dark_neutral_mask = normalize_f(112.0f - luma, 0.0f, 72.0f) *
                                            normalize_f(0.42f - saturation, 0.0f, 0.32f);
            if (dark_neutral_mask > 0.0f) {
                const float amount = dark_neutral_mask * black_anchor * strength;
                const float scale = 1.0f - 0.72f * amount;
                p[0] = clamp_byte_f(r * scale);
                p[1] = clamp_byte_f(g * scale);
                p[2] = clamp_byte_f(b * scale);
                continue;
            }

            const float warm_bias = min_f(r - b, (r + g) / 2.0f - b);
            const float warm_paper_mask =
                normalize_f(luma, min_luma, 210.0f) *
                normalize_f(245.0f - luma, 0.0f, 80.0f) *
                normalize_f(saturation_threshold - saturation, 0.0f, saturation_threshold) *
                normalize_f(warm_bias, warm_bias_threshold, 34.0f);
            if (warm_paper_mask <= 0.0f) {
                continue;
            }

            const float amount = warm_paper_mask * strength;
            // **The target lightness comes from the paper white's RED channel alone**
            // (:509-512), and the other two only bias it by 0.4 of their distance from 248.
            // That reads like a bug and is upstream's behaviour; a "fix" here would be a
            // divergence with no way to tell it from one.
            const float target_luma = min_f(252.0f, luma + (paper_r - luma) * target_mix);
            const float neutral_r = target_luma + (paper_r - 248.0f) * 0.4f;
            const float neutral_g = target_luma + (paper_g - 248.0f) * 0.4f;
            const float neutral_b = target_luma + (paper_b - 248.0f) * 0.4f;

            p[0] = clamp_byte_f(r + (neutral_r - r) * amount);
            p[1] = clamp_byte_f(g + (neutral_g - g) * amount);
            p[2] = clamp_byte_f(b + (neutral_b - b) * amount);
        }
    }
}

// --------------------------------------------------------------- level compression, luma
//
// processing.ts:1074-1092. Maps luma 0-255 onto black..white and scales all three channels by
// the ratio, so hue survives. It is containment rather than expansion: at black 8 and white 245
// the picture ends up inside a slightly narrower band than it started in.

void epd_adjust_level_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                             const epd_level_t *cfg)
{
    if (rgb == NULL || cfg == NULL || !cfg->enabled || width == 0 || height == 0) {
        return;
    }

    const float black = cfg->black;
    const float span = cfg->white - black;
    if (span <= 0.0f) {
        return;
    }

    // **Written the way upstream writes it, division by division.** Two of those divisions are
    // removable: `span / 255` is a loop constant, and `255 / maxChannel` has an integer divisor
    // so a 256-entry reciprocal table would return the identical float. Both were in an earlier
    // version of this function and both came out again, because the owner's standing position
    // is that speed is not being asked for at this stage -- and each of them cost something
    // real. The hoist moves where the rounding happens, so this stage would no longer be
    // byte-identical to the library for a value on a boundary; the table put 1 KB on the display
    // task's stack, which is a budget that has already caused a reboot loop once in this project
    // (app_display.c's own comment on why that stack is 8192). If a measurement ever asks for
    // them back, they are two lines each and `test_adjust` will say whether the bytes moved.
    for (size_t row = 0; row < height; row++) {
        uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            uint8_t *p = &line[col * 3];
            const float r = p[0];
            const float g = p[1];
            const float b = p[2];
            const float y = luma709_f(r, g, b);
            const float next_y = black + (y * span) / 255.0f;

            float ratio = y > 0.0f ? next_y / y : 0.0f;
            const float max_channel = max_f(max_f(r, g), b);
            if (max_channel > 0.0f) {
                ratio = min_f(ratio, 255.0f / max_channel);
            }

            p[0] = clamp_byte_f(r * ratio);
            p[1] = clamp_byte_f(g * ratio);
            p[2] = clamp_byte_f(b * ratio);
        }
    }
}

// ------------------------------------------------------------------ white preservation
//
// dither.ts:328-419. See epd_adjust.h for why this is two calls and one bit per pixel rather
// than upstream's per-pixel array of doubles.

size_t epd_adjust_white_bits_bytes(size_t width, size_t height)
{
    return (width * height + 7u) / 8u;
}

// dither.ts:369, which is `getSaturationFromChannels(r, g, b) <= maxWhiteSaturation`.
//
// **A black pixel is a candidate, and that is not a nicety.** getSaturationFromChannels returns 0
// when the maximum channel is 0 (dither.ts:1590-1595), and saturation_f() does the same, so a
// black pixel lands in histogram bin 0 and shifts the percentile. The apply pass then never
// rewrites it, because its luma is far below the threshold.
//
// This was written division-free at first -- for max > 0, `(max - min) / max <= t` is
// `(max - min) <= t * max`, which is two passes over the region without a libgcc division call in
// either. It came out again: the rearrangement moves the rounding, so a pixel sitting exactly on
// the threshold could fall the other way, and speed is not being asked for at this stage.
static inline bool low_saturation(float r, float g, float b, float threshold)
{
    return saturation_f(r, g, b) <= threshold;
}

bool epd_adjust_white_plan(const uint8_t *rgb, size_t width, size_t height, size_t stride,
                           const epd_palette_entry_t *palette, const epd_white_t *cfg,
                           uint8_t *bits, size_t bits_bytes, epd_white_plan_t *out)
{
    if (out == NULL) {
        return false;
    }
    out->active = false;
    out->target[0] = 0;
    out->target[1] = 0;
    out->target[2] = 0;
    out->target_luma = 0.0f;
    out->source_white_luma = 0.0f;

    if (rgb == NULL || palette == NULL || cfg == NULL || !cfg->enabled || bits == NULL ||
        width == 0 || height == 0 || bits_bytes < epd_adjust_white_bits_bytes(width, height)) {
        return false;
    }

    const float max_saturation = clamp_f(cfg->max_saturation, 0.0f, 1.0f);

    // Pass one: the low-saturation pixels' luma histogram. Nothing per-pixel is stored, because
    // the threshold the bits depend on is not known until this pass has finished.
    uint32_t histogram[256] = {0};
    uint32_t candidates = 0;
    for (size_t row = 0; row < height; row++) {
        const uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            const uint8_t *p = &line[col * 3];
            if (!low_saturation(p[0], p[1], p[2], max_saturation)) {
                continue;
            }
            histogram[clamp_byte_f(luma709_f(p[0], p[1], p[2]))] += 1;
            candidates++;
        }
    }
    if (candidates == 0) {
        return false;
    }

    const float source_white_luma =
        percentile_from_byte_histogram(histogram, candidates, cfg->percentile);
    // dither.ts:383. Upstream abandons the plan when the brightest neutral in the picture is
    // still dark: there is no paper white to preserve, and forcing one would invent highlights.
    // The figure is kept for the log line even on the refusal -- it is the number that says how
    // close the picture came.
    if (source_white_luma < cfg->min_luma) {
        out->source_white_luma = source_white_luma;
        return false;
    }

    // Pass two: one bit per pixel, both conditions folded in. `luma + 0.0001 >= threshold` is
    // upstream's test at dither.ts:409 with the sense flipped, and it compares the *unrounded*
    // luma against the integer bin index -- which is why the bit is computed here from the same
    // float rather than from the byte the histogram saw.
    memset(bits, 0, epd_adjust_white_bits_bytes(width, height));
    size_t index = 0;
    for (size_t row = 0; row < height; row++) {
        const uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            const uint8_t *p = &line[col * 3];
            if (low_saturation(p[0], p[1], p[2], max_saturation) &&
                luma709_f(p[0], p[1], p[2]) + 0.0001f >= source_white_luma) {
                bits[index >> 3] |= (uint8_t)(1u << (index & 7u));
            }
            index++;
        }
    }

    // getPaletteWhite, dither.ts:320-326: the lightest entry by Rec. 709 luma, first wins.
    const epd_palette_entry_t *lightest = &palette[0];
    for (size_t i = 1; i < EPD_PALETTE_COUNT; i++) {
        if (luma709_f(palette[i].r, palette[i].g, palette[i].b) >
            luma709_f(lightest->r, lightest->g, lightest->b)) {
            lightest = &palette[i];
        }
    }

    out->target[0] = lightest->r;
    out->target[1] = lightest->g;
    out->target[2] = lightest->b;
    out->target_luma = luma709_f(lightest->r, lightest->g, lightest->b);
    out->source_white_luma = source_white_luma;
    out->active = true;
    return true;
}

void epd_adjust_white_apply(uint8_t *rgb, size_t width, size_t height, size_t stride,
                            const uint8_t *bits, const epd_white_plan_t *plan)
{
    if (rgb == NULL || bits == NULL || plan == NULL || !plan->active) {
        return;
    }

    size_t index = 0;
    for (size_t row = 0; row < height; row++) {
        uint8_t *line = rgb + row * stride;
        for (size_t col = 0; col < width; col++) {
            // The counter advances for every pixel, marked or not: it is the bit index, not a
            // count of marked pixels.
            const bool marked = (bits[index >> 3] & (uint8_t)(1u << (index & 7u))) != 0;
            index++;
            if (!marked) {
                continue;
            }
            uint8_t *p = &line[col * 3];
            // dither.ts:412 -- a pixel the adjustments already left at or above the palette's
            // white is left alone rather than pulled back down to it.
            if (luma709_f(p[0], p[1], p[2]) >= plan->target_luma) {
                continue;
            }
            p[0] = plan->target[0];
            p[1] = plan->target[1];
            p[2] = plan->target[2];
        }
    }
}
