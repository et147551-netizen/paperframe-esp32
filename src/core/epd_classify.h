// What kind of picture is this? A port of epdoptimize's src/image-style.ts.
//
// This is the first half of what <https://paperlesspaper.github.io/epdoptimize> actually runs.
// Its processing select defaults to full auto (examples/index.ts:893), so every picture on that
// page was classified first and then given settings chosen from the class -- and the settings
// are the second half, which lives in epd_auto.h.
//
// **It does not use ML and it is cheap.** The image is sampled onto a grid whose longest side
// is 160 (upstream's DEFAULT_MAX_SAMPLE_DIMENSION), so for this panel's 400 x 600 canvas the
// whole of the work below runs over 107 x 160 = 17,120 samples rather than 240,000 pixels. That
// is the reason this half can run per render while the pixel half had to be rewritten to
// afford it.
//
// Three deliberate deviations from upstream, none of them a shortcut worth hiding:
//
//  1. **There is no alpha.** epd_canvas_t is RGB888 and has no alpha channel at all, so every
//     sample is visible by construction, `transparentRatio` is always 0, and upstream's
//     per-sample visibility checks collapse. Nothing is lost: an image with transparency has
//     already been composited onto white by the decoder before it reaches a canvas.
//  2. **Percentiles come from a 1024-bin histogram** (quarter-luma bins) instead of sorting
//     every sample's luma. It removes a 68 KB array and a 17,000-element sort, and puts at most
//     0.25 of a luma level between this and upstream's value. That matters only for an image
//     whose true `lumaRange` sits within a quarter-level of one of the thresholds in
//     epd_auto.c, and for such an image either answer is defensible.
//  3. **The caller says which region to classify.** Upstream classifies a whole image; here the
//     canvas is matted white around the picture (epd_image_draw()), and including the matte
//     inflates `lightRatio`, `flatRatio` and `topColorCoverage` enough to move a photograph
//     into `flatIllustration`. epd_fit_centre() gives the region, and it is the same rectangle
//     the decoder drew into.
//
// Free of ESP-IDF: it builds and is tested under env:native with the rest of the pure logic.

#ifndef EPD_CLASSIFY_H
#define EPD_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_canvas.h"

// image-style.ts:4-12. The order is upstream's declaration order; nothing depends on it, but
// keeping it makes the two files diffable.
typedef enum {
    EPD_KIND_PHOTO = 0,
    EPD_KIND_LOW_CONTRAST_PHOTO,
    EPD_KIND_HIGH_CONTRAST_PHOTO,
    EPD_KIND_FLAT_ILLUSTRATION,
    EPD_KIND_LINE_ART,
    EPD_KIND_TEXT_OR_UI,
    EPD_KIND_PIXEL_ART,
    EPD_KIND_UNKNOWN,
    EPD_KIND_COUNT,
} epd_image_kind_t;

// image-style.ts:3.
typedef enum {
    EPD_STYLE_PHOTO = 0,
    EPD_STYLE_ILLUSTRATION,
    EPD_STYLE_UNKNOWN,
} epd_image_style_t;

// The wire/log name of a kind, or "invalid" out of range. Used by the render log line, so a
// capture says what the classifier decided rather than leaving it to be inferred.
const char *epd_image_kind_name(epd_image_kind_t kind);

// image-style.ts:14-43, minus `transparentRatio` -- see deviation 1. Every field is a ratio in
// 0-1 except the four luma figures, which are on the 0-255 scale.
typedef struct {
    uint32_t sample_count;
    float unique_colour_ratio;
    float top_colour_coverage;
    float palette_entropy;
    float flat_ratio;
    float soft_change_ratio;
    float strong_edge_ratio;
    float edge_density;
    float horizontal_edge_ratio;
    float vertical_edge_ratio;
    float luma_std_dev;
    float luma_p05;
    float luma_p95;
    float luma_range;
    float saturation_mean;
    float saturation_std_dev;
    float dark_ratio;
    float light_ratio;
    float gray_ratio;
    float high_saturation_ratio;
    float warm_paper_ratio;
    float red_ratio;
    float dark_neutral_ratio;
    float photo_tile_ratio;
    float flat_tile_ratio;
    float text_tile_ratio;
    float gradient_tile_ratio;
} epd_classify_metrics_t;

// image-style.ts:45-52.
typedef struct {
    epd_image_style_t style;
    epd_image_kind_t kind;
    float kind_scores[EPD_KIND_COUNT];
    float confidence;
    float photo_score;
    epd_classify_metrics_t metrics;
} epd_classification_t;

// ------------------------------------------------------------------------- scratch memory
//
// Caller-owned, like epd_canvas_t's buffer and for the same reason: on the device this has to
// come out of PSRAM, and internal RAM is what this project runs out of first. On the host the
// tests want it static.
//
// Sizes for the default 160-sample grid on a 400 x 600 canvas: samples 205 KB, and 64 KB each
// for the two 32,768-entry colour tables. `epd_classify_scratch_bytes()` is the arithmetic so
// a call site does not repeat it.

#define EPD_CLASSIFY_MAX_SAMPLE_DIMENSION 160
// 15-bit RGB555 keys, which is upstream's quantisation for the colour histogram
// (image-style.ts:812-814).
#define EPD_CLASSIFY_COLOUR_KEYS 32768

typedef struct {
    uint8_t r, g, b;
    float luma;
    float saturation;
} epd_classify_sample_t;

typedef struct {
    epd_classify_sample_t *samples;   // at least EPD_CLASSIFY_MAX_SAMPLE_DIMENSION^2 entries
    size_t sample_capacity;           // in entries
    uint16_t *colour_counts;          // EPD_CLASSIFY_COLOUR_KEYS entries
    // Per-tile "have I seen this colour in this tile" marks, stamped with the tile's index so
    // that no tile has to clear them. EPD_CLASSIFY_COLOUR_KEYS entries.
    uint16_t *tile_stamps;
    // The luma histogram the two percentiles come from. Here rather than a file static because
    // 4 KB of internal RAM is 5 % of this firmware's idle headroom and this belongs in PSRAM
    // with the rest of the scratch -- and because a static would make the classifier
    // non-reentrant for no gain.
    uint32_t *luma_bins;
} epd_classify_scratch_t;

// Bytes needed for each buffer, for a caller sizing one allocation.
size_t epd_classify_samples_bytes(void);
size_t epd_classify_colour_counts_bytes(void);
size_t epd_classify_tile_stamps_bytes(void);
size_t epd_classify_luma_bins_bytes(void);

// ------------------------------------------------------------------------------ the API

// Classify a rectangle of a canvas, in **logical** coordinates -- the same coordinates
// epd_fit_centre() returns and epd_canvas_blit() draws in.
//
// Returns false and leaves `out` untouched when the region is empty, falls outside the canvas,
// or the scratch is missing or too small. A refusal is not a classification of `unknown`: the
// caller has to be able to tell "no answer" from "no idea", because the first means render the
// way the settings say and the second is a real class with real parameters attached.
bool epd_classify_canvas(const epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                         const epd_classify_scratch_t *scratch, epd_classification_t *out);

// The same over a flat RGB888 run, for tests and for anything holding pixels without a canvas.
bool epd_classify_rgb(const uint8_t *rgb, int32_t width, int32_t height,
                      const epd_classify_scratch_t *scratch, epd_classification_t *out);

#endif // EPD_CLASSIFY_H
