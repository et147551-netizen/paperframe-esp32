// Host-side driver so the real quantiser can be looked at before there is a canvas, an
// image decoder, or a way to get a photograph onto the panel.
//
// It links src/epd_dither.c and src/epd_epdopt.c unchanged. That is the point: a
// reimplementation in Python would be quicker to write and would drift from the firmware
// within a week, at which stage the preview would be reassuring rather than informative.
//
// Deliberately NOT in src/ -- under framework = espidf, src/CMakeLists.txt globs
// src/*.* and would try to build this into the firmware.
//
//   clang -std=c11 -O2 -Isrc tools/render_preview.c src/epd_dither.c src/epd_canvas.c \
//       src/epd_colour.c src/epd_epdopt.c -o render_preview
//   render_preview <none|quality|auto|epdopt|epdopt-nearest> <w> <h> [palette] [serpentine]
//                  [accurate]
//
// `quality` and `none` are M5GFX's row-wise paths and are what the frame renders with
// (src/app_display.c) -- the two colour-reduction modes of FR-3.3.
//
// `auto` is those plus epdoptimize's own auto flow in front of them: classify the picture, choose
// its settings from the class, run the five per-pixel stages. That is what the demo site does by
// default and what the frame does when the `auto_adjust` setting is on, and unlike the `epdopt`
// modes it is code the device really runs. It prints the plan to stderr.
//
// The two `epdopt` modes are the ported epdoptimize pipeline, which the device does **not**
// run: 24 s a photograph against 1.6 s, ticket 32. This tool is the only place it can be
// looked at, which is why it is here. They are also whole-image -- error diffusion cannot be
// streamed a row at a time -- so they buffer the picture where `none` and `quality` do not.
//
// tools/render_preview.py wraps it with PNG decode and encode.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_adjust.h"
#include "epd_auto.h"
#include "epd_canvas.h"
#include "epd_classify.h"
#include "epd_diffuse.h"
#include "epd_dither.h"
#include "epd_epdopt.h"
#include "epd_flow.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
// Without this the CRT translates 0x0A to 0x0D 0x0A on the way out and treats 0x1A as
// end-of-file on the way in, which corrupts pixel data a couple of rows in. It presents
// as "short read at row 2", not as wrong colours, so it is easy to misdiagnose.
static void set_binary_stdio(void)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}
#else
static void set_binary_stdio(void) {}
#endif

static const char *PALETTE_NAMES[] = {"stock", "measured-none", "measured-half",
                                     "measured-full", "manual"};
static const epd_render_t *const PALETTE_CFGS[] = {
    &EPD_RENDER_STOCK, &EPD_RENDER_MEASURED_NONE, &EPD_RENDER_MEASURED_HALF,
    &EPD_RENDER_MEASURED, &EPD_RENDER_MANUAL};

static const char *EPDOPT_NAMES[] = {"epdopt-aitjcize", "epdopt-spectra6", "epdopt-legacy",
                                     "epdopt-boeber", "epdopt-original"};
static const epd_palette_entry_t *const EPDOPT_TABLES[] = {
    EPD_PALETTE_EPDOPT_AITJCIZE, EPD_PALETTE_EPDOPT_SPECTRA6, EPD_PALETTE_EPDOPT_LEGACY,
    EPD_PALETTE_EPDOPT_BOEBER, EPD_PALETTE_EPDOPT_ORIGINAL};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// The row-wise paths: one row in, one packed row out, nothing held.
static int render_rows(int quality, long width, long height, const epd_render_t *cfg)
{
    const size_t row_bytes = (size_t)width * 3;
    const size_t packed_bytes = (size_t)((width + 1) / 2);

    unsigned char *row = malloc(row_bytes);
    unsigned char *packed = malloc(packed_bytes);
    if (row == NULL || packed == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    for (long y = 0; y < height; y++) {
        if (fread(row, 1, row_bytes, stdin) != row_bytes) {
            fprintf(stderr, "short read at row %ld\n", y);
            return 1;
        }

        if (quality) {
            epd_dither_row_quality_cfg(row, packed, (size_t)width, (size_t)y,
                                       EPD_DITHER_STRENGTH_QUALITY, cfg);
        } else {
            epd_dither_row_none_cfg(row, packed, (size_t)width, cfg);
        }

        if (fwrite(packed, 1, packed_bytes, stdout) != packed_bytes) {
            fprintf(stderr, "short write at row %ld\n", y);
            return 1;
        }
    }

    free(row);
    free(packed);
    return 0;
}

// Floyd-Steinberg error diffusion, which is the quantiser the frame uses when `dither_diffuse` is
// on. **The same three steps in the same order as src/app_display.c's diffuse_and_pack()**: the
// integer tone compression the plan still owes, then the diffusion, then a nearest pack at
// EPD_TONE_NONE -- which is exact, because every pixel is a palette entry by then, and which must
// not carry a tone or it would compress the palette's own white and re-match it.
//
// `cfg->tone` is the pre-pass amount, not the pack amount. The device gets it from
// epd_auto_row_tone() when the auto flow ran and from the palette otherwise; so does this.
//
// The region here is the whole image and the walk is contiguous. On the device the picture sits in
// a white matte and the region is its rectangle, which at rotation 1 strides backwards -- measured
// as costing nothing, but it is the one thing this tool does not reproduce.
static int diffuse_and_pack(unsigned char *buffer, long width, long height,
                            const epd_render_t *cfg, int serpentine)
{
    const size_t packed_bytes = (size_t)((width + 1) / 2);
    unsigned char *packed = malloc(packed_bytes);
    if (packed == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    epd_dither_tone_rect(buffer, (int32_t)width, (int32_t)height, 3, (ptrdiff_t)width * 3, cfg);

    const epd_diffuse_t dcfg = {.palette = cfg->palette, .serpentine = serpentine ? true : false};
    epd_diffuse_rect(buffer, (int32_t)width, (int32_t)height, 3, (ptrdiff_t)width * 3, &dcfg, 0,
                     (int32_t)height);

    const epd_render_t pack = {cfg->palette, EPD_TONE_NONE};
    for (long y = 0; y < height; y++) {
        epd_dither_row_none_cfg(&buffer[(size_t)y * (size_t)width * 3], packed, (size_t)width,
                                &pack);
        if (fwrite(packed, 1, packed_bytes, stdout) != packed_bytes) {
            fprintf(stderr, "short write at row %ld\n", y);
            free(packed);
            return 1;
        }
    }

    free(packed);
    return 0;
}

// The diffusion quantiser with no auto flow in front of it: the `dither_diffuse` setting on its
// own, which is one of the four combinations the two settings make.
static int render_diffuse(long width, long height, const epd_render_t *cfg, int serpentine)
{
    const size_t pixels = (size_t)width * (size_t)height;
    unsigned char *buffer = malloc(pixels * 3);
    if (buffer == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (fread(buffer, 1, pixels * 3, stdin) != pixels * 3) {
        fprintf(stderr, "short read: expected %zu bytes\n", pixels * 3);
        free(buffer);
        return 1;
    }

    const int rc = diffuse_and_pack(buffer, width, height, cfg, serpentine);
    free(buffer);
    return rc;
}

// The ported pipeline. Whole image, because the error diffusion needs its neighbours.
static int render_epdopt(int diffuse, int serpentine, int accurate, long width, long height,
                         const epd_palette_entry_t *palette)
{
    const size_t pixels = (size_t)width * (size_t)height;
    const size_t packed_bytes = (size_t)((width + 1) / 2);

    unsigned char *buffer = malloc(pixels * 3);
    unsigned char *packed = malloc(packed_bytes);
    if (buffer == NULL || packed == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (fread(buffer, 1, pixels * 3, stdin) != pixels * 3) {
        fprintf(stderr, "short read: expected %zu bytes\n", pixels * 3);
        return 1;
    }

    epd_canvas_t canvas;
    if (!epd_canvas_init(&canvas, buffer, pixels * 3, (int32_t)width, (int32_t)height)) {
        fprintf(stderr, "canvas init refused the buffer\n");
        return 1;
    }

    // A preset with the palette swapped. Copying one rather than rebuilding it keeps the
    // numbers in epd_epdopt.c.
    //
    // BALANCED_FAST by default and BALANCED under `accurate`. Neither runs on the device;
    // the difference is that BALANCED is what upstream's own `balanced` preset actually
    // means, and the two differ in 45 % of pixels on a photograph, so being able to see
    // both is the point.
    epd_epdopt_t cfg = accurate ? EPD_EPDOPT_BALANCED : EPD_EPDOPT_BALANCED_FAST;
    cfg.palette = palette;
    cfg.serpentine = serpentine ? true : false;

    if (!epd_epdopt_render(&canvas, &cfg, diffuse)) {
        fprintf(stderr, "range compression could not allocate\n");
        return 1;
    }

    const epd_render_t pack = {palette, EPD_TONE_NONE};
    for (long y = 0; y < height; y++) {
        const unsigned char *row = epd_canvas_row(&canvas, (int32_t)y);
        epd_dither_row_none_cfg(row, packed, (size_t)width, &pack);
        if (fwrite(packed, 1, packed_bytes, stdout) != packed_bytes) {
            fprintf(stderr, "short write at row %ld\n", y);
            return 1;
        }
    }

    free(buffer);
    free(packed);
    return 0;
}

// epdoptimize's auto flow, which is what its demo site runs by default and what the frame runs
// when the setting is on: classify the picture, choose its settings from the class, apply the five
// per-pixel stages, then quantise with M5GFX's row path.
//
// **This is the whole thing the device does, not an approximation of it.** src/epd_classify.c,
// src/epd_auto.c and src/epd_adjust.c are the same objects, and the plan is printed so a colour
// question can be answered against what it decided rather than against a guess. The one
// difference is the region: on the device the picture sits inside a white matte and the stages are
// given only its rectangle, whereas here the input *is* the picture.
static int render_auto(long width, long height, const epd_render_t *base, int diffuse)
{
    const size_t pixels = (size_t)width * (size_t)height;
    const size_t packed_bytes = (size_t)((width + 1) / 2);

    unsigned char *buffer = malloc(pixels * 3);
    unsigned char *packed = malloc(packed_bytes);
    epd_classify_scratch_t scratch = {0};
    scratch.samples = malloc(epd_classify_samples_bytes());
    scratch.sample_capacity = epd_classify_samples_bytes() / sizeof(epd_classify_sample_t);
    scratch.colour_counts = malloc(epd_classify_colour_counts_bytes());
    scratch.tile_stamps = malloc(epd_classify_tile_stamps_bytes());
    scratch.luma_bins = malloc(epd_classify_luma_bins_bytes());
    unsigned char *bits = malloc(epd_adjust_white_bits_bytes((size_t)width, (size_t)height));

    if (buffer == NULL || packed == NULL || scratch.samples == NULL ||
        scratch.colour_counts == NULL || scratch.tile_stamps == NULL ||
        scratch.luma_bins == NULL || bits == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (fread(buffer, 1, pixels * 3, stdin) != pixels * 3) {
        fprintf(stderr, "short read: expected %zu bytes\n", pixels * 3);
        return 1;
    }

    epd_classification_t c;
    if (!epd_classify_rgb(buffer, (int32_t)width, (int32_t)height, &scratch, &c)) {
        fprintf(stderr, "the classifier refused this image\n");
        return 1;
    }

    epd_auto_plan_t plan;
    epd_auto_suggest(&c, base->palette, &plan);

    // The six stages, in epd_flow.h's order. This tool and the two device sites are three callers
    // of one sequence now, which is what makes compare_plans.py's fourteen agreements evidence that
    // the port is right rather than evidence that three hand-copies still match. No clock: the
    // host's figures are not comparable with the device's and were never printed here.
    const size_t stride = (size_t)width * 3u;
    const epd_flow_region_t region = {
        .rgb = buffer,
        .width = (size_t)width,
        .height = (size_t)height,
        .stride = stride,
        .palette = base->palette,
        .white_bits = bits,
        .white_bits_bytes = epd_adjust_white_bits_bytes((size_t)width, (size_t)height),
    };
    epd_white_plan_t white;
    epd_flow_apply(&region, &plan, &white, NULL, NULL);

    // To stderr, because stdout carries the packed frame. Raw values in the same shape
    // src/app_display.c logs them, so a preview and a capture can be read side by side.
    epd_render_t cfg = *base;
    cfg.tone = epd_auto_row_tone(&plan, base->tone);
    fprintf(stderr,
            "auto kind=%s nearest=%d row_tone=%u tone=%d exp=%.3f sat=%.3f con=%.3f str=%.3f "
            "sb=%.3f hc=%.3f mid=%.3f range=%d rs=%.3f lo=%.3f hi=%.3f\n",
            epd_image_kind_name(plan.kind), plan.nearest ? 1 : 0, (unsigned)cfg.tone,
            (int)plan.adjust.tone_mode, (double)plan.adjust.exposure,
            (double)plan.adjust.saturation, (double)plan.adjust.contrast,
            (double)plan.adjust.strength, (double)plan.adjust.shadow_boost,
            (double)plan.adjust.highlight_compress, (double)plan.adjust.midpoint,
            (int)plan.adjust.range_mode, (double)plan.adjust.range_strength,
            (double)plan.adjust.low_percentile, (double)plan.adjust.high_percentile);
    fprintf(stderr,
            "auto lab=%d stucki=%d paper=%d level=%d(%.0f,%.0f) white=%d(%.1f) "
            "photo_score=%.4f style=%d\n",
            plan.wanted_lab ? 1 : 0, plan.wanted_stucki ? 1 : 0, plan.paper.enabled ? 1 : 0,
            plan.level.enabled ? 1 : 0, (double)plan.level.black, (double)plan.level.white,
            white.active ? 1 : 0, (double)white.source_white_luma, (double)c.photo_score,
            (int)c.style);

    // Nearest is nearest on either quantiser, so `diffuse` does not override the plan's choice --
    // the same rule as src/app_display.c.
    if (diffuse && !plan.nearest) {
        const int rc = diffuse_and_pack(buffer, width, height, &cfg, plan.serpentine ? 1 : 0);
        if (rc != 0) {
            return rc;
        }
    } else {
        for (long y = 0; y < height; y++) {
            const unsigned char *row = &buffer[(size_t)y * stride];
            if (plan.nearest) {
                epd_dither_row_none_cfg(row, packed, (size_t)width, &cfg);
            } else {
                epd_dither_row_quality_cfg(row, packed, (size_t)width, (size_t)y,
                                           EPD_DITHER_STRENGTH_QUALITY, &cfg);
            }
            if (fwrite(packed, 1, packed_bytes, stdout) != packed_bytes) {
                fprintf(stderr, "short write at row %ld\n", y);
                return 1;
            }
        }
    }

    free(buffer);
    free(packed);
    free(scratch.samples);
    free(scratch.colour_counts);
    free(scratch.tile_stamps);
    free(scratch.luma_bins);
    free(bits);
    return 0;
}

int main(int argc, char **argv)
{
    set_binary_stdio();

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <none|quality|auto|epdopt|epdopt-nearest> <width> <height> "
                "[palette] [serpentine] [accurate] [diffuse]\n",
                argv[0]);
        return 2;
    }

    // Everything after the palette is a bare keyword flag. Positional and unordered because
    // this is only ever driven by render_preview.py; an unknown word is refused rather than
    // ignored, so a typo cannot quietly render the default.
    int serpentine = 0;
    int accurate = 0;
    int diffuse = 0;
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "serpentine") == 0) {
            serpentine = 1;
        } else if (strcmp(argv[i], "accurate") == 0) {
            accurate = 1;
        } else if (strcmp(argv[i], "diffuse") == 0) {
            diffuse = 1;
        } else {
            fprintf(stderr, "unknown flag '%s'; expected serpentine, accurate or diffuse\n",
                    argv[i]);
            return 2;
        }
    }

    const long width = strtol(argv[2], NULL, 10);
    const long height = strtol(argv[3], NULL, 10);
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "bad dimensions\n");
        return 2;
    }

    const int is_epdopt = strncmp(argv[1], "epdopt", 6) == 0;
    // Both families default to the frame's own palette, so a bare invocation previews what
    // ships rather than a historical arm.
    const char *palette_name = (argc >= 5) ? argv[4] : "epdopt-aitjcize";

    if (is_epdopt) {
        const epd_palette_entry_t *table = NULL;
        for (size_t i = 0; i < COUNT_OF(EPDOPT_NAMES); i++) {
            if (strcmp(palette_name, EPDOPT_NAMES[i]) == 0) {
                table = EPDOPT_TABLES[i];
                break;
            }
        }
        if (table == NULL) {
            fprintf(stderr, "the epdopt modes take an epdopt-* palette, not '%s'\n",
                    palette_name);
            return 2;
        }
        if (strcmp(argv[1], "epdopt") != 0 && strcmp(argv[1], "epdopt-nearest") != 0) {
            fprintf(stderr, "mode must be none, quality, epdopt or epdopt-nearest\n");
            return 2;
        }
        return render_epdopt(strcmp(argv[1], "epdopt") == 0, serpentine, accurate, width,
                             height, table);
    }

    if (accurate) {
        fprintf(stderr, "accurate applies to the epdopt modes only\n");
        return 2;
    }
    // `serpentine` means something on the diffusion path too, and nothing without it.
    if (serpentine && !diffuse) {
        fprintf(stderr, "serpentine needs the epdopt modes or diffuse\n");
        return 2;
    }

    const epd_render_t *cfg = NULL;
    for (size_t i = 0; i < COUNT_OF(PALETTE_NAMES); i++) {
        if (strcmp(palette_name, PALETTE_NAMES[i]) == 0) {
            cfg = PALETTE_CFGS[i];
            break;
        }
    }

    // A row mode may also take an epdopt palette, at full tone compression. That crossing is
    // the one question this tool exists to answer cheaply: it separates what epdoptimize's
    // *calibration* buys from what its *algorithm* buys, because the M5GFX path costs 1.6 s
    // on the device where the ported pipeline costs 24 s.
    epd_render_t crossed;
    if (cfg == NULL) {
        for (size_t i = 0; i < COUNT_OF(EPDOPT_NAMES); i++) {
            if (strcmp(palette_name, EPDOPT_NAMES[i]) == 0) {
                crossed.palette = EPDOPT_TABLES[i];
                crossed.tone = EPD_TONE_FULL;
                cfg = &crossed;
                break;
            }
        }
    }

    if (cfg == NULL) {
        fprintf(stderr, "unknown palette '%s'\n", palette_name);
        return 2;
    }

    // `auto` takes a palette from either family, like the row modes -- it *is* a row mode, with
    // the classifier and the five adjustment stages in front of it.
    if (strcmp(argv[1], "auto") == 0) {
        return render_auto(width, height, cfg, diffuse);
    }

    const int quality = (strcmp(argv[1], "quality") == 0);
    if (!quality && strcmp(argv[1], "none") != 0) {
        fprintf(stderr, "mode must be none, quality, auto, epdopt or epdopt-nearest\n");
        return 2;
    }

    // `diffuse` replaces the row quantiser, so it has no meaning on `none`: nearest is nearest on
    // either path, which is the same rule the device follows.
    if (diffuse) {
        if (!quality) {
            fprintf(stderr, "diffuse applies to quality and auto, not none\n");
            return 2;
        }
        return render_diffuse(width, height, cfg, serpentine);
    }

    return render_rows(quality, width, height, cfg);
}
