// Ported from paperlesspaper/epdoptimize, src/image-style.ts.
// Copyright the epdoptimize authors, licensed Apache-2.0; see LICENSES/epdoptimize-Apache-2.0.txt.
//
// Kept in upstream's order -- sampling, neighbour metrics, colour distribution, edges, tiles,
// photo score, kind scores -- so the two files can be read side by side. The thresholds are all
// upstream's and none of them was retuned: this project has no rating data of its own, and a
// threshold moved by eye here would be a second unmeasured thing on top of a first.

#include "epd_classify.h"

#include <math.h>
#include <string.h>

#define MAX_DIM EPD_CLASSIFY_MAX_SAMPLE_DIMENSION
#define COLOUR_KEYS EPD_CLASSIFY_COLOUR_KEYS

// image-style.ts:82.
#define PHOTO_THRESHOLD 0.5f

// Quarter-luma bins over 0-255. See the header for what this replaces and what it costs.
#define LUMA_BINS 1024
#define LUMA_BIN_SCALE 4.0f

size_t epd_classify_samples_bytes(void)
{
    return (size_t)MAX_DIM * (size_t)MAX_DIM * sizeof(epd_classify_sample_t);
}

size_t epd_classify_colour_counts_bytes(void)
{
    return (size_t)COLOUR_KEYS * sizeof(uint16_t);
}

size_t epd_classify_tile_stamps_bytes(void)
{
    return (size_t)COLOUR_KEYS * sizeof(uint16_t);
}

size_t epd_classify_luma_bins_bytes(void)
{
    return (size_t)LUMA_BINS * sizeof(uint32_t);
}

const char *epd_image_kind_name(epd_image_kind_t kind)
{
    switch (kind) {
    case EPD_KIND_PHOTO:
        return "photo";
    case EPD_KIND_LOW_CONTRAST_PHOTO:
        return "lowContrastPhoto";
    case EPD_KIND_HIGH_CONTRAST_PHOTO:
        return "highContrastPhoto";
    case EPD_KIND_FLAT_ILLUSTRATION:
        return "flatIllustration";
    case EPD_KIND_LINE_ART:
        return "lineArt";
    case EPD_KIND_TEXT_OR_UI:
        return "textOrUi";
    case EPD_KIND_PIXEL_ART:
        return "pixelArt";
    case EPD_KIND_UNKNOWN:
        return "unknown";
    default:
        return "invalid";
    }
}

// -------------------------------------------------------------------------------- helpers

static inline float clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// image-style.ts:816-822.
static inline float normalise(float value, float min, float max)
{
    if (max <= min) {
        return value >= max ? 1.0f : 0.0f;
    }
    return clamp01((value - min) / (max - min));
}

// image-style.ts:772-784. Upstream divides all three channels by 255 first; (max - min) / max
// is scale-invariant, so this is the same number with two fewer divisions.
static inline float saturation_of(uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const uint8_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return max == 0 ? 0.0f : (float)(max - min) / (float)max;
}

static inline float luma_of(uint8_t r, uint8_t g, uint8_t b)
{
    return 0.2126f * (float)r + 0.7152f * (float)g + 0.0722f * (float)b;
}

// image-style.ts:812-814.
static inline uint16_t colour_key(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

// image-style.ts:764-770.
static inline float colour_difference(const epd_classify_sample_t *a,
                                      const epd_classify_sample_t *b)
{
    const float dr = (float)a->r - (float)b->r;
    const float dg = (float)a->g - (float)b->g;
    const float db = (float)a->b - (float)b->b;
    return sqrtf(dr * dr + dg * dg + db * db);
}

// image-style.ts:786-801.
static inline bool is_warm_paper(uint8_t r, uint8_t g, uint8_t b, float luma, float saturation)
{
    return luma >= 92.0f && luma <= 230.0f && saturation >= 0.06f && saturation <= 0.56f &&
           (int)r >= (int)b + 10 && (int)g >= (int)b - 6;
}

// image-style.ts:803-810.
static inline bool is_red(uint8_t r, uint8_t g, uint8_t b, float saturation)
{
    return saturation >= 0.34f && (int)r >= (int)g + 24 && (int)r >= (int)b + 28;
}

// image-style.ts:754-762, over the histogram rather than a sorted array. Upstream returns the
// element at index round((n-1)*p) of the ascending sort, which is the (index+1)-th smallest --
// so the bin wanted here is the first whose cumulative count exceeds `index`.
static float percentile_from_bins(const uint32_t *bins, uint32_t count, float p)
{
    if (count == 0) {
        return 0.0f;
    }
    const float index_f = floorf((float)(count - 1) * p + 0.5f);
    const uint32_t index = (uint32_t)(index_f < 0.0f ? 0.0f : index_f);
    uint32_t seen = 0;

    for (size_t bin = 0; bin < LUMA_BINS; bin++) {
        seen += bins[bin];
        if (seen > index) {
            return (float)bin / LUMA_BIN_SCALE;
        }
    }
    return 255.0f;
}

// ------------------------------------------------------------------------ the metric passes

// image-style.ts:352-415. Right and down neighbours of every sample, bucketed by how far apart
// they are: flat, a soft change, or a strong edge.
static void neighbour_metrics(const epd_classify_sample_t *s, int32_t w, int32_t h,
                              epd_classify_metrics_t *out)
{
    uint32_t neighbours = 0;
    uint32_t flat = 0;
    uint32_t soft = 0;
    uint32_t strong = 0;

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            const epd_classify_sample_t *sample = &s[y * w + x];

            if (x + 1 < w) {
                const float difference = colour_difference(sample, &s[y * w + x + 1]);
                neighbours++;
                if (difference <= 4.0f) {
                    flat++;
                } else if (difference <= 28.0f) {
                    soft++;
                } else {
                    strong++;
                }
            }

            if (y + 1 < h) {
                const float difference = colour_difference(sample, &s[(y + 1) * w + x]);
                neighbours++;
                if (difference <= 4.0f) {
                    flat++;
                } else if (difference <= 28.0f) {
                    soft++;
                } else {
                    strong++;
                }
            }
        }
    }

    if (neighbours == 0) {
        out->flat_ratio = 1.0f;
        out->soft_change_ratio = 0.0f;
        out->strong_edge_ratio = 0.0f;
        return;
    }

    out->flat_ratio = (float)flat / (float)neighbours;
    out->soft_change_ratio = (float)soft / (float)neighbours;
    out->strong_edge_ratio = (float)strong / (float)neighbours;
}

// image-style.ts:417-442.
static void colour_distribution_metrics(const uint16_t *counts, uint32_t sample_count,
                                        epd_classify_metrics_t *out)
{
    if (sample_count == 0) {
        out->unique_colour_ratio = 0.0f;
        out->top_colour_coverage = 0.0f;
        out->palette_entropy = 0.0f;
        return;
    }

    // The eight largest counts, kept as a sorted insertion rather than by sorting 32,768
    // entries: upstream sorts descending and takes the first eight, and only their sum is used.
    uint32_t top[8] = {0};
    uint32_t distinct = 0;
    double entropy = 0.0;

    for (size_t key = 0; key < COLOUR_KEYS; key++) {
        const uint32_t count = counts[key];
        if (count == 0) {
            continue;
        }
        distinct++;

        const double probability = (double)count / (double)sample_count;
        entropy -= probability * log2(probability);

        if (count > top[7]) {
            size_t slot = 7;
            while (slot > 0 && top[slot - 1] < count) {
                top[slot] = top[slot - 1];
                slot--;
            }
            top[slot] = count;
        }
    }

    uint32_t top_sum = 0;
    for (size_t i = 0; i < 8; i++) {
        top_sum += top[i];
    }

    const double max_entropy = log2((double)(distinct < 2 ? 2 : distinct));

    out->unique_colour_ratio = (float)distinct / (float)sample_count;
    out->top_colour_coverage = (float)top_sum / (float)sample_count;
    out->palette_entropy = max_entropy == 0.0 ? 0.0f : (float)(entropy / max_entropy);
}

// image-style.ts:444-504. A 4-neighbour luma gradient over the interior only.
static void edge_metrics(const epd_classify_sample_t *s, int32_t w, int32_t h,
                         epd_classify_metrics_t *out)
{
    uint32_t checked = 0;
    uint32_t edges = 0;
    uint32_t horizontal = 0;
    uint32_t vertical = 0;

    for (int32_t y = 1; y < h - 1; y++) {
        for (int32_t x = 1; x < w - 1; x++) {
            const float left = s[y * w + x - 1].luma;
            const float right = s[y * w + x + 1].luma;
            const float up = s[(y - 1) * w + x].luma;
            const float down = s[(y + 1) * w + x].luma;

            checked++;
            const float dx = fabsf(right - left);
            const float dy = fabsf(down - up);
            const float magnitude = sqrtf(dx * dx + dy * dy);

            if (magnitude >= 42.0f) {
                edges++;
                if (dy > dx * 1.2f) {
                    horizontal++;
                } else if (dx > dy * 1.2f) {
                    vertical++;
                }
            }
        }
    }

    if (checked == 0 || edges == 0) {
        out->edge_density = 0.0f;
        out->horizontal_edge_ratio = 0.0f;
        out->vertical_edge_ratio = 0.0f;
        return;
    }

    out->edge_density = (float)edges / (float)checked;
    out->horizontal_edge_ratio = (float)horizontal / (float)edges;
    out->vertical_edge_ratio = (float)vertical / (float)edges;
}

// image-style.ts:575-647.
typedef struct {
    float unique_colour_ratio;
    float gray_ratio;
    float flat_ratio;
    float soft_change_ratio;
    float strong_edge_ratio;
    float edge_density; // upstream sets this to strong_edge_ratio, not to the global measure
    float luma_std_dev;
} tile_stats_t;

static bool tile_stats(const epd_classify_sample_t *s, int32_t w, int32_t h, int32_t tile_x,
                       int32_t tile_y, int32_t tile_size, uint16_t *stamps, uint16_t stamp,
                       tile_stats_t *out)
{
    uint32_t visible = 0;
    uint32_t distinct = 0;
    uint32_t gray = 0;
    double luma_sum = 0.0;
    double luma_square_sum = 0.0;
    uint32_t neighbours = 0;
    uint32_t flat = 0;
    uint32_t soft = 0;
    uint32_t strong = 0;

    const int32_t max_y = (h < tile_y + tile_size) ? h : tile_y + tile_size;
    const int32_t max_x = (w < tile_x + tile_size) ? w : tile_x + tile_size;

    for (int32_t y = tile_y; y < max_y; y++) {
        for (int32_t x = tile_x; x < max_x; x++) {
            const epd_classify_sample_t *sample = &s[y * w + x];

            visible++;
            luma_sum += (double)sample->luma;
            luma_square_sum += (double)sample->luma * (double)sample->luma;
            if (sample->saturation <= 0.08f) {
                gray++;
            }

            const uint16_t key = colour_key(sample->r, sample->g, sample->b);
            if (stamps[key] != stamp) {
                stamps[key] = stamp;
                distinct++;
            }

            if (x + 1 < max_x) {
                const float difference = colour_difference(sample, &s[y * w + x + 1]);
                neighbours++;
                if (difference <= 4.0f) {
                    flat++;
                } else if (difference <= 28.0f) {
                    soft++;
                } else {
                    strong++;
                }
            }

            if (y + 1 < max_y) {
                const float difference = colour_difference(sample, &s[(y + 1) * w + x]);
                neighbours++;
                if (difference <= 4.0f) {
                    flat++;
                } else if (difference <= 28.0f) {
                    soft++;
                } else {
                    strong++;
                }
            }
        }
    }

    // image-style.ts:631. A partial tile at the right or bottom edge is dropped rather than
    // counted, which is why the tile ratios do not simply average the whole image.
    const uint32_t minimum = (uint32_t)(tile_size * tile_size) / 4u;
    if (visible < (minimum > 12u ? minimum : 12u)) {
        return false;
    }

    const double luma_mean = luma_sum / (double)visible;
    const double variance = luma_square_sum / (double)visible - luma_mean * luma_mean;
    const float strong_ratio = neighbours == 0 ? 0.0f : (float)strong / (float)neighbours;

    out->unique_colour_ratio = (float)distinct / (float)visible;
    out->gray_ratio = (float)gray / (float)visible;
    out->flat_ratio = neighbours == 0 ? 1.0f : (float)flat / (float)neighbours;
    out->soft_change_ratio = neighbours == 0 ? 0.0f : (float)soft / (float)neighbours;
    out->strong_edge_ratio = strong_ratio;
    out->edge_density = strong_ratio;
    out->luma_std_dev = (float)sqrt(variance > 0.0 ? variance : 0.0);
    return true;
}

// image-style.ts:506-573.
static void tile_metrics(const epd_classify_sample_t *s, int32_t w, int32_t h,
                         uint16_t *stamps, epd_classify_metrics_t *out)
{
    const int32_t smaller = w < h ? w : h;
    const int32_t tile_size = (smaller / 10) > 8 ? (smaller / 10) : 8;
    uint32_t tiles = 0;
    uint32_t photo_tiles = 0;
    uint32_t flat_tiles = 0;
    uint32_t text_tiles = 0;
    uint32_t gradient_tiles = 0;
    uint16_t stamp = 0;

    for (int32_t tile_y = 0; tile_y < h; tile_y += tile_size) {
        for (int32_t tile_x = 0; tile_x < w; tile_x += tile_size) {
            tile_stats_t tile;
            stamp++;
            if (!tile_stats(s, w, h, tile_x, tile_y, tile_size, stamps, stamp, &tile)) {
                continue;
            }

            tiles++;

            if (tile.edge_density >= 0.16f && tile.gray_ratio >= 0.55f &&
                tile.luma_std_dev >= 38.0f) {
                text_tiles++;
            }
            if (tile.unique_colour_ratio <= 0.12f && tile.flat_ratio >= 0.62f) {
                flat_tiles++;
            }
            if (tile.unique_colour_ratio >= 0.18f && tile.luma_std_dev >= 18.0f &&
                tile.flat_ratio <= 0.68f) {
                photo_tiles++;
            }
            if (tile.soft_change_ratio >= 0.38f && tile.strong_edge_ratio <= 0.16f &&
                tile.luma_std_dev >= 12.0f) {
                gradient_tiles++;
            }
        }
    }

    if (tiles == 0) {
        out->photo_tile_ratio = 0.0f;
        out->flat_tile_ratio = 0.0f;
        out->text_tile_ratio = 0.0f;
        out->gradient_tile_ratio = 0.0f;
        return;
    }

    out->photo_tile_ratio = (float)photo_tiles / (float)tiles;
    out->flat_tile_ratio = (float)flat_tiles / (float)tiles;
    out->text_tile_ratio = (float)text_tiles / (float)tiles;
    out->gradient_tile_ratio = (float)gradient_tiles / (float)tiles;
}

// ------------------------------------------------------------------------------ the scores

// image-style.ts:649-684.
static float photo_score_of(const epd_classify_metrics_t *m)
{
    const float unique = normalise(m->unique_colour_ratio, 0.08f, 0.35f);
    const float soft_change = normalise(m->soft_change_ratio, 0.18f, 0.48f);
    const float texture = normalise(1.0f - m->flat_ratio, 0.2f, 0.65f);
    const float luma = normalise(m->luma_std_dev, 24.0f, 72.0f);
    const float saturation_spread = normalise(m->saturation_std_dev, 0.08f, 0.26f);
    const float entropy = normalise(m->palette_entropy, 0.55f, 0.9f);
    const float photo_tile = normalise(m->photo_tile_ratio, 0.18f, 0.62f);

    float desaturated = normalise(m->gray_ratio, 0.45f, 0.75f);
    const float candidates[4] = {
        normalise(m->photo_tile_ratio, 0.34f, 0.58f),
        normalise(m->luma_std_dev, 48.0f, 76.0f),
        normalise(1.0f - m->flat_ratio, 0.34f, 0.58f),
        normalise(m->palette_entropy, 0.62f, 0.88f),
    };
    for (size_t i = 0; i < 4; i++) {
        if (candidates[i] < desaturated) {
            desaturated = candidates[i];
        }
    }

    const float flat_penalty = normalise(m->flat_ratio, 0.55f, 0.88f);
    const float top_colour_penalty = normalise(m->top_colour_coverage, 0.45f, 0.86f);
    const float hard_edge_penalty =
        m->flat_ratio > 0.35f ? normalise(m->strong_edge_ratio, 0.28f, 0.58f) : 0.0f;

    return clamp01(unique * 0.34f + soft_change * 0.22f + texture * 0.16f + luma * 0.1f +
                   saturation_spread * 0.06f + entropy * 0.07f + photo_tile * 0.13f +
                   desaturated * 0.24f - flat_penalty * 0.12f - top_colour_penalty * 0.1f -
                   hard_edge_penalty * 0.08f);
}

// image-style.ts:686-745.
static void kind_scores_of(const epd_classify_metrics_t *m, float photo_score, float *out)
{
    const float contrast_endpoints = m->dark_ratio + m->light_ratio;
    const float photo_tile_line_art_penalty = normalise(m->photo_tile_ratio, 0.32f, 0.58f);
    const float low_usable_range = normalise(92.0f - m->luma_range, 0.0f, 72.0f);

    out[EPD_KIND_PHOTO] =
        clamp01(photo_score * 0.55f + normalise(m->photo_tile_ratio, 0.12f, 0.62f) * 0.25f +
                normalise(m->palette_entropy, 0.55f, 0.9f) * 0.12f +
                normalise(m->soft_change_ratio, 0.22f, 0.48f) * 0.08f);

    out[EPD_KIND_LOW_CONTRAST_PHOTO] =
        clamp01(photo_score * 0.38f + normalise(34.0f - m->luma_std_dev, 0.0f, 22.0f) * 0.24f +
                low_usable_range * 0.22f +
                normalise(m->gradient_tile_ratio, 0.16f, 0.55f) * 0.18f +
                normalise(m->soft_change_ratio, 0.24f, 0.5f) * 0.1f);

    out[EPD_KIND_HIGH_CONTRAST_PHOTO] =
        clamp01(photo_score * 0.42f + normalise(m->luma_std_dev, 58.0f, 92.0f) * 0.3f +
                normalise(contrast_endpoints, 0.18f, 0.42f) * 0.18f +
                normalise(m->photo_tile_ratio, 0.18f, 0.58f) * 0.1f);

    out[EPD_KIND_FLAT_ILLUSTRATION] =
        clamp01((1.0f - photo_score) * 0.32f + normalise(m->flat_ratio, 0.52f, 0.9f) * 0.2f +
                normalise(m->top_colour_coverage, 0.38f, 0.85f) * 0.22f +
                normalise(m->flat_tile_ratio, 0.18f, 0.72f) * 0.18f +
                normalise(m->high_saturation_ratio, 0.08f, 0.38f) * 0.08f);

    out[EPD_KIND_LINE_ART] =
        clamp01(normalise(m->gray_ratio, 0.48f, 0.9f) * 0.28f +
                normalise(m->edge_density, 0.05f, 0.22f) * 0.24f +
                normalise(m->flat_ratio, 0.5f, 0.86f) * 0.18f +
                normalise(m->top_colour_coverage, 0.45f, 0.9f) * 0.18f +
                normalise(0.16f - m->high_saturation_ratio, 0.0f, 0.16f) * 0.12f -
                photo_tile_line_art_penalty * 0.14f);

    out[EPD_KIND_TEXT_OR_UI] =
        clamp01(normalise(m->text_tile_ratio, 0.05f, 0.35f) * 0.32f +
                normalise(m->edge_density, 0.06f, 0.24f) * 0.22f +
                normalise(m->gray_ratio, 0.42f, 0.86f) * 0.16f +
                normalise(m->top_colour_coverage, 0.42f, 0.86f) * 0.16f +
                normalise(m->flat_tile_ratio, 0.12f, 0.58f) * 0.14f);

    out[EPD_KIND_PIXEL_ART] =
        clamp01(normalise(m->flat_ratio, 0.62f, 0.94f) * 0.25f +
                normalise(m->top_colour_coverage, 0.5f, 0.92f) * 0.24f +
                normalise(m->flat_tile_ratio, 0.25f, 0.82f) * 0.2f +
                normalise(m->high_saturation_ratio, 0.08f, 0.45f) * 0.16f +
                normalise(0.22f - m->soft_change_ratio, 0.0f, 0.22f) * 0.15f);

    out[EPD_KIND_UNKNOWN] = 0.0f;
}

// image-style.ts:747-752. Upstream reduces with a strict `>`, so the earliest kind wins a tie
// and the enum's order is therefore load-bearing in exactly the way the palette tables' is.
static epd_image_kind_t best_kind(const float *scores)
{
    epd_image_kind_t best = EPD_KIND_UNKNOWN;
    float best_score = -1.0f;

    for (size_t i = 0; i < EPD_KIND_COUNT; i++) {
        if (scores[i] > best_score) {
            best_score = scores[i];
            best = (epd_image_kind_t)i;
        }
    }
    return best;
}

// -------------------------------------------------------------------------- the whole thing

// image-style.ts:236-309, given a grid that is already sampled.
static void classify_samples(epd_classify_sample_t *s, int32_t w, int32_t h,
                             const epd_classify_scratch_t *scratch, epd_classification_t *out)
{
    const uint32_t count = (uint32_t)w * (uint32_t)h;

    // **These four accumulators are `double` on purpose and it is not laziness.** A float sum
    // of 17,120 squared lumas reaches 1e9, where a float's ulp is 64 -- every later term would
    // round away and the variance would come out visibly wrong. lumaStdDev feeds the photo
    // score and three of the seven kind scores, so that error would change classifications.
    // The cost is four accumulations over the *sample* grid, not over 240,000 pixels, which is
    // the whole reason the sample grid exists.
    double luma_sum = 0.0;
    double luma_square_sum = 0.0;
    double saturation_sum = 0.0;
    double saturation_square_sum = 0.0;

    uint32_t dark = 0, light = 0, gray = 0, high_saturation = 0;
    uint32_t warm_paper = 0, red = 0, dark_neutral = 0;

    uint32_t *luma_bins = scratch->luma_bins;
    memset(luma_bins, 0, epd_classify_luma_bins_bytes());
    memset(scratch->colour_counts, 0, epd_classify_colour_counts_bytes());
    memset(scratch->tile_stamps, 0, epd_classify_tile_stamps_bytes());

    for (uint32_t i = 0; i < count; i++) {
        epd_classify_sample_t *sample = &s[i];
        const float luma = luma_of(sample->r, sample->g, sample->b);
        const float saturation = saturation_of(sample->r, sample->g, sample->b);
        sample->luma = luma;
        sample->saturation = saturation;

        luma_sum += (double)luma;
        luma_square_sum += (double)luma * (double)luma;
        saturation_sum += (double)saturation;
        saturation_square_sum += (double)saturation * (double)saturation;

        if (luma <= 36.0f) dark++;
        if (luma >= 220.0f) light++;
        if (saturation <= 0.08f) gray++;
        if (saturation >= 0.72f) high_saturation++;
        if (is_warm_paper(sample->r, sample->g, sample->b, luma, saturation)) warm_paper++;
        if (is_red(sample->r, sample->g, sample->b, saturation)) red++;
        if (luma <= 88.0f && saturation <= 0.36f) dark_neutral++;

        int32_t bin = (int32_t)floorf(luma * LUMA_BIN_SCALE + 0.5f);
        if (bin < 0) bin = 0;
        if (bin >= LUMA_BINS) bin = LUMA_BINS - 1;
        luma_bins[bin]++;

        const uint16_t key = colour_key(sample->r, sample->g, sample->b);
        if (scratch->colour_counts[key] < UINT16_MAX) {
            scratch->colour_counts[key]++;
        }
    }

    epd_classify_metrics_t *m = &out->metrics;
    memset(m, 0, sizeof(*m));
    m->sample_count = count;

    neighbour_metrics(s, w, h, m);
    colour_distribution_metrics(scratch->colour_counts, count, m);
    edge_metrics(s, w, h, m);
    tile_metrics(s, w, h, scratch->tile_stamps, m);

    const double luma_mean = luma_sum / (double)count;
    const double luma_variance = luma_square_sum / (double)count - luma_mean * luma_mean;
    const double saturation_mean = saturation_sum / (double)count;
    const double saturation_variance =
        saturation_square_sum / (double)count - saturation_mean * saturation_mean;

    m->luma_std_dev = (float)sqrt(luma_variance > 0.0 ? luma_variance : 0.0);
    m->luma_p05 = percentile_from_bins(luma_bins, count, 0.05f);
    m->luma_p95 = percentile_from_bins(luma_bins, count, 0.95f);
    m->luma_range = m->luma_p95 - m->luma_p05;
    m->saturation_mean = (float)saturation_mean;
    m->saturation_std_dev =
        (float)sqrt(saturation_variance > 0.0 ? saturation_variance : 0.0);
    m->dark_ratio = (float)dark / (float)count;
    m->light_ratio = (float)light / (float)count;
    m->gray_ratio = (float)gray / (float)count;
    m->high_saturation_ratio = (float)high_saturation / (float)count;
    m->warm_paper_ratio = (float)warm_paper / (float)count;
    m->red_ratio = (float)red / (float)count;
    m->dark_neutral_ratio = (float)dark_neutral / (float)count;

    out->photo_score = photo_score_of(m);
    out->confidence = clamp01(fabsf(out->photo_score - PHOTO_THRESHOLD) * 2.0f);
    out->style = out->photo_score >= PHOTO_THRESHOLD ? EPD_STYLE_PHOTO : EPD_STYLE_ILLUSTRATION;
    kind_scores_of(m, out->photo_score, out->kind_scores);
    out->kind = best_kind(out->kind_scores);
}

// image-style.ts:141-151: the grid the whole file works over. JavaScript's Math.round is
// floor(v + 0.5) and the arguments are positive, so floorf() reproduces it.
static bool sample_grid_size(int32_t width, int32_t height, int32_t *out_w, int32_t *out_h)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    const int32_t longest = width > height ? width : height;
    const float scale = (float)MAX_DIM / (float)longest;

    if (scale >= 1.0f) {
        *out_w = width;
        *out_h = height;
    } else {
        *out_w = (int32_t)floorf((float)width * scale + 0.5f);
        *out_h = (int32_t)floorf((float)height * scale + 0.5f);
    }
    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;

    // An image larger than the grid cannot exceed it, but one smaller than it is sampled 1:1
    // and could in principle be wider than MAX_DIM in one axis if the caller passes something
    // strange. Refuse rather than overrun the scratch.
    return *out_w <= MAX_DIM && *out_h <= MAX_DIM;
}

static bool scratch_ok(const epd_classify_scratch_t *scratch, int32_t w, int32_t h)
{
    return scratch != NULL && scratch->samples != NULL && scratch->colour_counts != NULL &&
           scratch->tile_stamps != NULL && scratch->luma_bins != NULL &&
           scratch->sample_capacity >= (size_t)w * (size_t)h;
}

bool epd_classify_canvas(const epd_canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h,
                         const epd_classify_scratch_t *scratch, epd_classification_t *out)
{
    if (c == NULL || c->rgb == NULL || out == NULL || w <= 0 || h <= 0) {
        return false;
    }
    if (x < 0 || y < 0 || x + w > epd_canvas_logical_width(c) ||
        y + h > epd_canvas_logical_height(c)) {
        return false;
    }

    int32_t sw = 0, sh = 0;
    if (!sample_grid_size(w, h, &sw, &sh) || !scratch_ok(scratch, sw, sh)) {
        return false;
    }

    // Logical coordinates via epd_canvas_pixel(), for the same reason epd_epdopt_diffuse()
    // does: the rotation mapping lives in epd_canvas.c and nowhere else.
    // 32-bit, not 64: `sy * h` is at most 160 x 600 here and a 64-bit divide is `__divdi3`, a
    // libgcc call, once per sample. epd_image.c uses int64 in the same shape because its source
    // dimensions come off a phone camera; a sample grid is bounded by MAX_DIM.
    epd_canvas_t *mutable_canvas = (epd_canvas_t *)c;
    for (int32_t sy = 0; sy < sh; sy++) {
        const int32_t source_y = (sy * h) / sh;
        for (int32_t sx = 0; sx < sw; sx++) {
            const int32_t source_x = (sx * w) / sw;
            const uint8_t *p = epd_canvas_pixel(mutable_canvas, x + source_x, y + source_y);
            epd_classify_sample_t *sample = &scratch->samples[sy * sw + sx];
            if (p == NULL) {
                return false;
            }
            sample->r = p[0];
            sample->g = p[1];
            sample->b = p[2];
        }
    }

    classify_samples(scratch->samples, sw, sh, scratch, out);
    return true;
}

bool epd_classify_rgb(const uint8_t *rgb, int32_t width, int32_t height,
                      const epd_classify_scratch_t *scratch, epd_classification_t *out)
{
    if (rgb == NULL || out == NULL) {
        return false;
    }

    int32_t sw = 0, sh = 0;
    if (!sample_grid_size(width, height, &sw, &sh) || !scratch_ok(scratch, sw, sh)) {
        return false;
    }

    for (int32_t sy = 0; sy < sh; sy++) {
        const int32_t source_y = (sy * height) / sh;
        for (int32_t sx = 0; sx < sw; sx++) {
            const int32_t source_x = (sx * width) / sw;
            const uint8_t *p = &rgb[((size_t)source_y * (size_t)width + (size_t)source_x) * 3];
            epd_classify_sample_t *sample = &scratch->samples[sy * sw + sx];
            sample->r = p[0];
            sample->g = p[1];
            sample->b = p[2];
        }
    }

    classify_samples(scratch->samples, sw, sh, scratch, out);
    return true;
}
