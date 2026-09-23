// A port of paperlesspaper/epdoptimize's src/auto-processing.ts. Copyright the epdoptimize
// authors, licensed Apache-2.0; see LICENSES/epdoptimize-Apache-2.0.txt.
//
// Only buildLayeredSuggestion() at intent "natural", which is what the demo site runs: its
// processing select defaults to full auto (examples/index.ts:893) and its auto-flow control to
// Layered auto. The older buildSuggestion() ("legacy" in upstream's own terms) is not ported.
//
// **The order below is upstream's call order and it is load-bearing**, because these functions
// overwrite each other rather than composing: applyPosterScanTuning replaces everything
// applyLowContrastRestoreTuning just set, and applyPaletteTuning then overrides the range mode
// of either. Reordering them changes the answer for real pictures, not just in principle.
//
//   buildLayeredSuggestion (:312-405)
//     getLayeredBaseRecommendation   (:567)  kind -> preset values
//     applyLayeredAutoAdjustments    (:633)  per-kind overrides; early-returns for a restorable source
//     applyLowContrastRestoreTuning  (:814)  the faded-scan arm
//     applyPosterScanTuning          (:877)  the warm-paper arm, which wins over the one above
//     applyPaletteTuning            (:1145)  the palette's own luma range overrides the range mode
//     applyIntent                   (:1066)  a no-op at "natural"
//     enforceQuantizationGuard      (:1176)  can flip quantizationOnly back to diffusion
//     enforceMinimumAutoContrast     (:407)  floors a contrast-mode curve at 0
//     enforceAutoWhitePreservation   (:428)  turns white preservation on for any non-off range
//
// Three of the nine are not ported at all and the header says why. Also skipped, deliberately:
// addClassificationReasons/addPaletteReasons and the pipelineSteps array, which build the
// English text the demo site shows beside the picture. The device logs raw values instead.
//
// **Every literal below is the value upstream computes, not the expression it computes it
// with.** exposureAdjustmentFromMultiplier() is `Number(Math.log2(m).toFixed(3))` and
// linearAdjustmentFromMultiplier() is `Number((m - 1).toFixed(3))` -- both round to three
// decimals, so log2(1.1) reaches the pipeline as exactly 0.138 and 1.35 - 1 reaches it as
// exactly 0.35 rather than as 0.35000000000000009. Writing the rounded literal is what makes
// test/test_auto's assertions equalities instead of tolerance bands; each one carries the
// multiplier it came from so the arithmetic stays checkable.

#include "epd_auto.h"

#include <stddef.h>

// ------------------------------------------------------------------------ tiny helpers

static float max_f(float a, float b)
{
    return a > b ? a : b;
}

static float min_f(float a, float b)
{
    return a < b ? a : b;
}

// auto-processing.ts:1284-1286, and the same Rec. 709 weights as everywhere else here.
static float luma709(float r, float g, float b)
{
    return r * 0.2126f + g * 0.7152f + b * 0.0722f;
}

// auto-processing.ts:1288-1293.
static float saturation_of(float r, float g, float b)
{
    const float max = max_f(max_f(r, g), b) / 255.0f;
    const float min = min_f(min_f(r, g), b) / 255.0f;
    return max == 0.0f ? 0.0f : (max - min) / max;
}

// ------------------------------------------------------------------- the palette profile
//
// getPaletteProfile, auto-processing.ts:1240-1263. `saturationRange` is computed there and read
// by nothing, so it is not here.

typedef struct {
    bool valid;
    int colour_count;
    float luma_range;
    float average_saturation;
} palette_profile_t;

static palette_profile_t palette_profile(const epd_palette_entry_t *palette)
{
    palette_profile_t p = {0};
    if (palette == NULL) {
        return p;   // upstream's `null` profile: applyPaletteTuning does nothing at all
    }

    float lowest = luma709(palette[0].r, palette[0].g, palette[0].b);
    float highest = lowest;
    float saturation_sum = 0.0f;

    for (size_t i = 0; i < EPD_PALETTE_COUNT; i++) {
        const float luma = luma709(palette[i].r, palette[i].g, palette[i].b);
        if (luma < lowest) {
            lowest = luma;
        }
        if (luma > highest) {
            highest = luma;
        }
        saturation_sum += saturation_of(palette[i].r, palette[i].g, palette[i].b);
    }

    p.valid = true;
    p.colour_count = EPD_PALETTE_COUNT;
    p.luma_range = highest - lowest;
    p.average_saturation = saturation_sum / (float)EPD_PALETTE_COUNT;
    return p;
}

// -------------------------------------------------------------------- the recommendation
//
// RecommendationBase, auto-processing.ts:97-106, minus processingPreset (read for its values
// and then overwritten; the name only reaches upstream's reasons strings).
//
// `range_mode == EPD_RANGE_MODE_OFF` covers upstream's `{mode: "off"}`. It does **not** have to
// also model `dynamicRangeCompression: undefined`, which is a distinct state there -- the tail
// of applyLayeredAutoAdjustments tests `?.mode === "off"` and so treats undefined differently.
// In the layered path it cannot arise: getLayeredBaseRecommendation copies the preset's, and all
// seven PROCESSING_PRESETS define one.

typedef struct {
    epd_tone_mode_t tone_mode;
    float exposure;
    float saturation;
    float contrast;
    float strength;
    float shadow_boost;
    float highlight_compress;
    float midpoint;

    epd_range_mode_t range_mode;
    float range_strength;
    float low_percentile;
    float high_percentile;

    epd_level_t level;
    epd_paper_t paper;
    epd_white_t white;

    bool quantization_only;
    bool lab;
    bool stucki;
} rec_t;

// PROCESSING_PRESETS, processing.ts:129-280. Only the four getImageKindPreset() can return are
// here: `balanced`, `soft`, `vivid`, `restore`. `dynamic` is unreachable from any kind;
// `grayscale` and `posterScan` are named by applyPaletteTuning and applyPosterScanTuning, both
// of which then set every field they need explicitly, so those two presets' values are never
// read either. Adding an intent other than "natural" would change that.

static rec_t preset_balanced(void)
{
    rec_t r = {0};
    r.tone_mode = EPD_TONE_MODE_CONTRAST;
    r.range_mode = EPD_RANGE_MODE_DISPLAY;
    r.range_strength = 1.0f;
    return r;
}

static rec_t preset_soft(void)
{
    rec_t r = {0};
    r.tone_mode = EPD_TONE_MODE_CONTRAST;
    r.saturation = 0.1f;   // linear(1.1)
    r.contrast = -0.1f;    // linear(0.9)
    r.range_mode = EPD_RANGE_MODE_DISPLAY;
    r.range_strength = 1.0f;
    r.stucki = true;
    return r;
}

static rec_t preset_vivid(void)
{
    rec_t r = {0};
    r.tone_mode = EPD_TONE_MODE_SCURVE;
    r.exposure = 0.138f;   // log2(1.1)
    r.saturation = 0.6f;   // linear(1.6)
    r.strength = 0.7f;
    r.shadow_boost = 0.1f;
    r.highlight_compress = -1.3f;
    r.midpoint = 0.5f;
    r.range_mode = EPD_RANGE_MODE_OFF;
    return r;
}

static rec_t preset_restore(void)
{
    rec_t r = {0};
    r.tone_mode = EPD_TONE_MODE_SCURVE;
    r.exposure = 0.111f;    // log2(1.08)
    r.saturation = -0.1f;   // linear(0.9)
    r.strength = 1.0f;
    r.shadow_boost = 0.25f;
    r.highlight_compress = -0.75f;
    r.midpoint = 0.46f;
    r.range_mode = EPD_RANGE_MODE_AUTO;
    r.range_strength = 0.9f;
    r.low_percentile = 0.02f;
    r.high_percentile = 0.98f;
    r.lab = true;
    return r;
}

// getLayeredBaseRecommendation (:567) with getImageKindPreset (:586),
// getImageKindColorMatching (:604), getImageKindDiffusionMatrix (:613) and
// getImageKindDitheringType (:624) folded in -- four switches over one enum, which read as one.
static rec_t layered_base(epd_image_kind_t kind)
{
    rec_t r;
    switch (kind) {
    case EPD_KIND_LOW_CONTRAST_PHOTO:
        r = preset_restore();
        r.lab = true;
        r.stucki = false;   // overrides restore's own floydSteinberg with floydSteinberg
        break;
    case EPD_KIND_HIGH_CONTRAST_PHOTO:
        r = preset_soft();
        r.stucki = true;
        break;
    case EPD_KIND_FLAT_ILLUSTRATION:
    case EPD_KIND_PIXEL_ART:
        r = preset_vivid();
        break;
    case EPD_KIND_TEXT_OR_UI:
    case EPD_KIND_LINE_ART:
        r = preset_balanced();
        r.lab = true;
        break;
    case EPD_KIND_PHOTO:
    case EPD_KIND_UNKNOWN:
    default:
        r = preset_balanced();
        break;
    }

    r.quantization_only = (kind == EPD_KIND_TEXT_OR_UI || kind == EPD_KIND_LINE_ART ||
                           kind == EPD_KIND_PIXEL_ART);
    return r;
}

// ------------------------------------------------------------------------ the predicates

// isRestorableLowContrastSource, auto-processing.ts:855-875.
static bool restorable_low_contrast(const epd_classification_t *c)
{
    const epd_classify_metrics_t *m = &c->metrics;

    if (c->kind == EPD_KIND_LOW_CONTRAST_PHOTO) {
        return true;
    }
    if (m->luma_range > 96.0f || m->luma_std_dev > 32.0f) {
        return false;
    }
    if (m->gray_ratio < 0.5f && m->saturation_mean > 0.18f) {
        return false;
    }
    if (c->kind == EPD_KIND_PIXEL_ART) {
        return false;
    }

    return m->edge_density >= 0.015f || m->soft_change_ratio >= 0.12f ||
           m->gradient_tile_ratio >= 0.04f || m->text_tile_ratio >= 0.04f ||
           (m->luma_range <= 70.0f && m->gray_ratio >= 0.7f && m->palette_entropy >= 0.35f &&
            m->top_colour_coverage <= 0.92f);
}

// isWarmPosterScanSource, auto-processing.ts:923-938.
static bool warm_poster_scan(const epd_classification_t *c)
{
    const epd_classify_metrics_t *m = &c->metrics;

    const bool has_warm_paper = m->warm_paper_ratio >= 0.18f;
    const bool has_ink = m->dark_neutral_ratio >= 0.025f || m->red_ratio >= 0.008f ||
                         m->strong_edge_ratio >= 0.05f;
    const bool graphic_or_poster_like =
        c->kind == EPD_KIND_FLAT_ILLUSTRATION || c->kind == EPD_KIND_TEXT_OR_UI ||
        c->kind == EPD_KIND_LINE_ART || m->flat_ratio >= 0.5f ||
        m->top_colour_coverage >= 0.36f;

    return has_warm_paper && has_ink && graphic_or_poster_like;
}

// isClearlyQuantizationFriendly, auto-processing.ts:1194-1231.
static bool clearly_quantization_friendly(const epd_classification_t *c)
{
    const epd_classify_metrics_t *m = &c->metrics;

    const bool photo_like_detail = c->style == EPD_STYLE_PHOTO || c->photo_score >= 0.34f ||
                                   m->photo_tile_ratio >= 0.1f ||
                                   m->gradient_tile_ratio >= 0.08f ||
                                   m->soft_change_ratio >= 0.28f;
    if (photo_like_detail) {
        return false;
    }

    const bool flat_repeated_colour = m->flat_ratio >= 0.7f &&
                                      m->top_colour_coverage >= 0.72f &&
                                      m->palette_entropy <= 0.72f;
    const bool clear_text_or_ui = m->text_tile_ratio >= 0.16f && m->edge_density >= 0.1f &&
                                  m->gray_ratio >= 0.5f && m->top_colour_coverage >= 0.62f;
    const bool clear_line_art = m->gray_ratio >= 0.76f && m->edge_density >= 0.12f &&
                                m->top_colour_coverage >= 0.68f &&
                                m->high_saturation_ratio <= 0.08f;
    const bool clear_pixel_art = m->flat_ratio >= 0.78f && m->flat_tile_ratio >= 0.44f &&
                                 m->top_colour_coverage >= 0.78f &&
                                 m->soft_change_ratio <= 0.16f;

    return flat_repeated_colour &&
           (clear_text_or_ui || clear_line_art || clear_pixel_art);
}

// ---------------------------------------------------------------------------- the stages
//
// **These two exist because upstream replaces whole objects, not fields.**
// `recommendation.toneMapping = {...}` discards every key the new literal does not name, so a
// `{mode: "off", exposure: 0, saturation: 0}` leaves `strength` and `midpoint` *undefined*
// rather than at the preset's values. Writing eight fields individually would leave the
// preset's behind -- unread by epd_adjust_tone() in that mode, and so functionally identical,
// but not equal, and test/test_auto compares structs. Replacing wholesale is both what upstream
// does and what makes the comparison an equality.
//
// Where upstream reads the old value on the way past (`Math.max(rec.toneMapping?.saturation ??
// 0, ...)`), the caller captures it before calling these.

static void set_tone(rec_t *r, epd_tone_mode_t mode, float exposure, float saturation,
                     float contrast, float strength, float shadow_boost,
                     float highlight_compress, float midpoint)
{
    r->tone_mode = mode;
    r->exposure = exposure;
    r->saturation = saturation;
    r->contrast = contrast;
    r->strength = strength;
    r->shadow_boost = shadow_boost;
    r->highlight_compress = highlight_compress;
    r->midpoint = midpoint;
}

static void set_range(rec_t *r, epd_range_mode_t mode, float strength, float low, float high)
{
    r->range_mode = mode;
    r->range_strength = strength;
    r->low_percentile = low;
    r->high_percentile = high;
}

static const epd_level_t LEVEL_LUMA_8_245 = {true, 8.0f, 245.0f};
static const epd_level_t LEVEL_LUMA_6_248 = {true, 6.0f, 248.0f};
static const epd_level_t LEVEL_LUMA_3_252 = {true, 3.0f, 252.0f};

// applyLayeredAutoAdjustments, auto-processing.ts:633-803.
static void layered_auto_adjustments(rec_t *r, const epd_classification_t *c,
                                     const palette_profile_t *profile)
{
    const epd_classify_metrics_t *m = &c->metrics;

    // :640. The early return is why this whole function -- including its palette tail below --
    // is skipped for a faded scan, which applyLowContrastRestoreTuning then handles instead.
    if (restorable_low_contrast(c)) {
        return;
    }

    switch (c->kind) {
    case EPD_KIND_LOW_CONTRAST_PHOTO:
        // Unreachable in practice: restorable_low_contrast() returns true for this kind, so the
        // early return above has already fired. Ported anyway, because it is upstream's code
        // and a future change to that predicate would make it live.
        set_tone(r, EPD_TONE_MODE_SCURVE,
                 max_f(r->exposure, 0.084f),   // log2(1.06)
                 m->gray_ratio >= 0.72f ? -1.0f                         // linear(0)
                                        : min_f(r->saturation, 0.05f),  // linear(1.05)
                 0.0f,
                 m->luma_range <= 70.0f ? 1.0f : 0.9f,
                 m->luma_p05 >= 55.0f ? 0.2f : 0.28f, -0.75f,
                 m->luma_p95 <= 190.0f ? 0.44f : 0.46f);
        set_range(r, EPD_RANGE_MODE_AUTO, m->luma_range <= 70.0f ? 0.96f : 0.88f, 0.02f, 0.98f);
        r->level = LEVEL_LUMA_8_245;
        r->lab = m->gray_ratio >= 0.55f;
        r->stucki = false;
        break;

    case EPD_KIND_HIGH_CONTRAST_PHOTO:
        set_tone(r, EPD_TONE_MODE_CONTRAST, 0.0f, 0.05f /* linear(1.05) */, 0.0f, 0.0f, 0.0f,
                 0.0f, 0.0f);
        set_range(r, EPD_RANGE_MODE_DISPLAY, 0.85f, 0.0f, 0.0f);
        break;

    case EPD_KIND_PHOTO:
        if (m->luma_std_dev <= 42.0f) {
            set_tone(r, EPD_TONE_MODE_SCURVE, 0.057f /* log2(1.04) */,
                     max_f(r->saturation, 0.12f) /* linear(1.12) */, 0.0f, 0.66f, 0.05f, -1.2f,
                     0.49f);
            set_range(r, EPD_RANGE_MODE_AUTO, 0.68f, 0.01f, 0.99f);
        } else if (m->luma_std_dev >= 70.0f) {
            set_range(r, EPD_RANGE_MODE_DISPLAY, 0.78f, 0.0f, 0.0f);
        } else {
            set_range(r, EPD_RANGE_MODE_DISPLAY, 0.7f, 0.0f, 0.0f);
        }
        break;

    case EPD_KIND_FLAT_ILLUSTRATION:
        set_tone(r, EPD_TONE_MODE_SCURVE, 0.084f /* log2(1.06) */,
                 // linear(1.35) or linear(1.45)
                 m->high_saturation_ratio >= 0.28f ? 0.35f : 0.45f, 0.0f, 0.68f, 0.06f, -1.2f,
                 0.5f);
        set_range(r, EPD_RANGE_MODE_OFF, 0.0f, 0.0f, 0.0f);
        break;

    case EPD_KIND_TEXT_OR_UI:
        set_tone(r, EPD_TONE_MODE_CONTRAST, 0.057f /* log2(1.04) */,
                 // linear(0.85) or linear(1)
                 m->gray_ratio >= 0.7f ? -0.15f : 0.0f, 0.2f /* linear(1.2) */, 0.0f, 0.0f,
                 0.0f, 0.0f);
        set_range(r, EPD_RANGE_MODE_DISPLAY, 0.72f, 0.0f, 0.0f);
        break;

    case EPD_KIND_LINE_ART:
        set_tone(r, EPD_TONE_MODE_CONTRAST, 0.0f, -0.25f /* linear(0.75) */,
                 // linear(1.42) or linear(1.25)
                 m->luma_range <= 96.0f ? 0.42f : 0.25f, 0.0f, 0.0f, 0.0f, 0.0f);
        // Both percentiles are set here whatever the mode -- upstream's object literal carries
        // them unconditionally (:767-772), and display mode simply does not read them.
        set_range(r, m->luma_range <= 96.0f ? EPD_RANGE_MODE_AUTO : EPD_RANGE_MODE_DISPLAY,
                  m->luma_range <= 96.0f ? 0.9f : 0.65f, 0.02f, 0.98f);
        // :773-776 -- the wide-range branch leaves whatever was there, which is nothing.
        if (m->luma_range <= 96.0f) {
            r->level = LEVEL_LUMA_6_248;
        }
        break;

    case EPD_KIND_PIXEL_ART:
        set_tone(r, EPD_TONE_MODE_OFF, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        set_range(r, EPD_RANGE_MODE_OFF, 0.0f, 0.0f, 0.0f);
        break;

    case EPD_KIND_UNKNOWN:
    default:
        set_range(r, EPD_RANGE_MODE_DISPLAY, 0.72f, 0.0f, 0.0f);
        break;
    }

    // :793-802. A dim palette cannot afford to have the range left alone.
    if (profile->valid && profile->luma_range <= 150.0f && r->range_mode == EPD_RANGE_MODE_OFF) {
        set_range(r, EPD_RANGE_MODE_DISPLAY, 0.7f, 0.0f, 0.0f);
    }
}

// applyLowContrastRestoreTuning, auto-processing.ts:814-853.
static void low_contrast_restore_tuning(rec_t *r, const epd_classification_t *c)
{
    if (!restorable_low_contrast(c)) {
        return;
    }

    const epd_classify_metrics_t *m = &c->metrics;

    r->lab = m->gray_ratio >= 0.55f;
    r->stucki = false;
    r->quantization_only = false;

    set_tone(r, EPD_TONE_MODE_SCURVE,
             // log2(1.1) or log2(1.06)
             m->luma_p95 <= 190.0f ? 0.138f : 0.084f,
             // linear(0) or linear(0.9)
             m->gray_ratio >= 0.72f ? -1.0f : -0.1f, 0.0f,
             m->luma_range <= 70.0f ? 1.0f : 0.92f,
             m->luma_p05 >= 55.0f ? 0.2f : 0.28f, -0.75f,
             m->luma_p95 <= 190.0f ? 0.44f : 0.46f);
    set_range(r, EPD_RANGE_MODE_AUTO, m->luma_range <= 70.0f ? 0.96f : 0.9f, 0.02f, 0.98f);

    r->level = LEVEL_LUMA_8_245;
}

// applyPosterScanTuning, auto-processing.ts:877-921. Runs after the one above and overwrites
// it, so a warm poster that is also a faded scan is treated as a poster.
static void poster_scan_tuning(rec_t *r, const epd_classification_t *c)
{
    if (!warm_poster_scan(c)) {
        return;
    }

    r->lab = false;
    r->stucki = false;
    r->quantization_only = false;

    r->paper.enabled = true;
    r->paper.strength = 0.95f;
    r->paper.min_luma = 82.0f;
    r->paper.saturation_threshold = 0.56f;
    r->paper.warm_bias_threshold = 8.0f;
    r->paper.black_anchor = 0.95f;
    r->paper.preserve_red = 0.85f;
    r->paper.paper_white[0] = 248;
    r->paper.paper_white[1] = 248;
    r->paper.paper_white[2] = 246;

    set_tone(r, EPD_TONE_MODE_SCURVE, 0.057f /* log2(1.04) */, 0.05f /* linear(1.05) */, 0.0f,
             0.92f, 0.08f, -0.55f, 0.44f);
    set_range(r, EPD_RANGE_MODE_AUTO, 1.0f, 0.015f, 0.985f);

    r->level = LEVEL_LUMA_3_252;
}

// applyPaletteTuning, auto-processing.ts:1145-1174.
static void palette_tuning(rec_t *r, const palette_profile_t *profile)
{
    if (!profile->valid) {
        return;
    }

    if (profile->colour_count <= 2) {
        // The grayscale preset's tone curve, written out at :1155-1163. Unreachable from this
        // frame -- every palette here has six entries -- and ported because the predicate is
        // about the palette rather than about the panel.
        r->lab = true;
        set_tone(r, EPD_TONE_MODE_SCURVE, 0.0f, -1.0f /* linear(0) */, 0.0f, 0.8f, 0.1f, -1.4f,
                 0.5f);
        return;
    }

    if (profile->luma_range <= 150.0f) {
        // **This overrides the kind's choice, including "off" and "auto".** The new object
        // upstream builds carries only mode and strength, so the percentiles are dropped --
        // which does not matter, because display mode does not read them. `?? 0` on the old
        // strength is what makes an "off" range come out at 0.8: an off range still carries
        // whatever strength its object had, and every one that reaches here has none.
        set_range(r, EPD_RANGE_MODE_DISPLAY, max_f(r->range_strength, 0.8f), 0.0f, 0.0f);
    }
}

// enforceQuantizationGuard, auto-processing.ts:1176-1192. Upstream's own re-check of the
// kind's decision against the metrics, which is what bounds the damage when a knife-edge
// classification picks quantizationOnly for a picture that needs dithering.
static void quantization_guard(rec_t *r, const epd_classification_t *c)
{
    if (!r->quantization_only) {
        return;
    }
    if (clearly_quantization_friendly(c)) {
        return;
    }
    r->quantization_only = false;
}

// enforceMinimumAutoContrast, auto-processing.ts:407-426.
//
// Only the first branch is ported. The second reads the *preset* when the recommendation has no
// toneMapping at all, and in the layered path that cannot happen: getLayeredBaseRecommendation
// copies the preset's curve and all seven presets define one.
static void minimum_auto_contrast(rec_t *r)
{
    if (r->tone_mode == EPD_TONE_MODE_CONTRAST) {
        r->contrast = max_f(r->contrast, 0.0f);
    }
}

// enforceAutoWhitePreservation, auto-processing.ts:428-448. The defaults it fills in are read
// in dither.ts:378-383; whitePreserveMaxSaturation is never set by auto and defaults to 0.18
// at dither.ts:351-354.
static void auto_white_preservation(rec_t *r)
{
    if (r->range_mode == EPD_RANGE_MODE_OFF) {
        return;
    }
    r->white.enabled = true;
    r->white.percentile = 0.99f;
    r->white.min_luma = 150.0f;
    r->white.max_saturation = 0.18f;
}

// -------------------------------------------------------------------------------- the API

void epd_auto_suggest(const epd_classification_t *c, const epd_palette_entry_t *palette,
                      epd_auto_plan_t *out)
{
    if (c == NULL || out == NULL) {
        return;
    }

    const palette_profile_t profile = palette_profile(palette);

    rec_t r = layered_base(c->kind);
    layered_auto_adjustments(&r, c, &profile);
    low_contrast_restore_tuning(&r, c);
    poster_scan_tuning(&r, c);
    palette_tuning(&r, &profile);
    // applyIntent(base, "natural", reasons) is a no-op: :1066-1099 has a branch for each of the
    // other four intents and none for this one.
    quantization_guard(&r, c);
    minimum_auto_contrast(&r);
    auto_white_preservation(&r);

    epd_auto_plan_t plan = {0};
    plan.kind = c->kind;

    plan.adjust.palette = palette;
    // Always true: upstream's `toneMapping: undefined` -- the stage not running at all -- cannot
    // arise in the layered path, per minimum_auto_contrast()'s comment. EPD_TONE_MODE_OFF is a
    // different thing and does occur (pixelArt), and still runs exposure and saturation.
    plan.adjust.tone_enabled = true;
    plan.adjust.tone_mode = r.tone_mode;
    plan.adjust.exposure = r.exposure;
    plan.adjust.saturation = r.saturation;
    plan.adjust.contrast = r.contrast;
    plan.adjust.strength = r.strength;
    plan.adjust.shadow_boost = r.shadow_boost;
    plan.adjust.highlight_compress = r.highlight_compress;
    plan.adjust.midpoint = r.midpoint;
    plan.adjust.range_mode = r.range_mode;
    plan.adjust.range_strength = r.range_strength;
    plan.adjust.low_percentile = r.low_percentile;
    plan.adjust.high_percentile = r.high_percentile;

    plan.paper = r.paper;
    plan.level = r.level;
    plan.white = r.white;

    plan.nearest = r.quantization_only;
    // :388-391 -- the layered suggestion spreads `serpentine: true` only into the errorDiffusion
    // branch, and omits the key entirely for quantizationOnly.
    plan.serpentine = !r.quantization_only;
    plan.wanted_lab = r.lab;
    plan.wanted_stucki = r.stucki;

    *out = plan;
}

uint8_t epd_auto_row_tone(const epd_auto_plan_t *plan, uint8_t palette_tone)
{
    if (plan == NULL) {
        return palette_tone;
    }
    // Exactly one range compression on every path: epd_adjust_range() when the plan asks for
    // one, epd_dither.c's tone_compress() when it does not. See epd_auto.h.
    return plan->adjust.range_mode == EPD_RANGE_MODE_OFF ? palette_tone : EPD_TONE_NONE;
}
