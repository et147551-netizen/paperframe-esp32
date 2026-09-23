// epdoptimize's image pipeline, ported to C.
//
// Three stages, run in this order on a whole canvas, from
// refs/epdoptimize/src/dither/processing.ts:1225-1249 and dither.ts:1009-1078:
//
//   1. tone mapping        -- exposure, saturation, contrast or an S-curve, all by LUT
//   2. range compression    -- squeeze the image's lightness into what the panel can reach
//   3. error diffusion      -- Floyd-Steinberg (or another kernel) against the palette
//
// The stages are separately callable because that is what the parity test needs: a
// mismatch against the reference implementation has to localise to a stage rather than to
// "the pipeline". tools/epdopt_reference.mjs emits the expected bytes after each one.
//
// **This is a port, not an implementation.** Where the reference does something a fresh
// implementation would do differently -- accumulating diffusion error through clamped
// bytes rather than a float plane, rounding halves towards +infinity, matching in plain
// RGB -- this file does it the reference's way, because the acceptance test is byte
// equality against it. epd_colour.h carries the same warning about the constants.
//
// What is NOT ported, and why, is in `.scratch/digital-frame/issues/32`: the per-image auto
// classifier (auto-processing.ts, which is what the demo site actually runs by default),
// edge preservation and antialiasing, paper normalisation, clarity, and every dithering
// mode except error diffusion.
//
// **Nothing here runs on the device.** Ticket 32 measured it at 24.0 s a photograph against
// the row-wise path's 1.62 s and adopted only epdoptimize's *palette*; the frame quantises
// with epd_dither.c. This file exists for tools/render_preview.py and test/test_epdopt, and
// because it is what makes the palettes' provenance checkable.
//
// **That is still true of this file and no longer true of its first two stages.** src/epd_adjust.c
// is tone mapping and range compression rewritten in single precision for the device: 668.9 ms
// against this file's 8 259 ms in the same build. **Both figures are for a neutral saturation**,
// which is what EPD_EPDOPT_BALANCED_FAST and EPD_ADJUST_BALANCED both ask for and which takes the
// float version's cheap LUT branch. With a non-neutral saturation the float pair is 1 337 ms
// (measured 2026-09-05), because the tone stage then does an HSL round trip per pixel; that is the
// figure to compare against if the settings came from src/epd_auto.c rather than from the preset. It is a separate module rather than an edit here
// precisely because this one's acceptance test is byte equality against the JavaScript, and a
// float rewrite cannot pass that. **Do not "optimise" anything below.** Every figure in this
// header predates 2026-09-05 and was taken at 160 MHz with a 32-byte data cache line; they are
// about a third high for the current build, and the ratios are what they are being used for.
//
// Free of ESP-IDF, so it builds and is tested under env:native.

#ifndef EPD_EPDOPT_H
#define EPD_EPDOPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_canvas.h"
#include "epd_dither.h"

// processing.ts:4. EPD_TONE_MODE_UNSET is the reference's `mode: undefined`, and it is a
// distinct behaviour rather than a missing value: undefined applies the contrast
// adjustment *and* the S-curve, where "contrast" and "scurve" each apply only their own.
typedef enum {
    EPD_TONE_MODE_UNSET = 0,
    EPD_TONE_MODE_OFF,
    EPD_TONE_MODE_CONTRAST,
    EPD_TONE_MODE_SCURVE,
} epd_tone_mode_t;

// processing.ts:6. "display" compresses from a fixed source range of L* 0-100; "auto"
// measures the image's own percentiles first.
typedef enum {
    EPD_RANGE_MODE_OFF = 0,
    EPD_RANGE_MODE_DISPLAY,
    EPD_RANGE_MODE_AUTO,
} epd_range_mode_t;

// processing.ts:9, and the branch at processing.ts:838. ACCURATE works in L*a*b* and
// costs a cbrt plus up to eighteen pow() calls per pixel; FAST is Rec. 709 luma ratios
// and costs none. They are different algorithms with different output, not a quality
// knob on one algorithm -- each is matched against the reference separately.
//
// **EPD_RANGE_ACCURATE is host-only. Do not select it from device code.** It is upstream's
// default and it is unusable here: on 2026-09-05 it ran for over 90 s on one 400 x 600
// photograph without finishing and tripped the task watchdog eighteen times
// (.scratch/captures/photo-epdopt-20260905-142247.log; the M5GFX path it replaced took
// 1618 ms in the same run). The reason is arithmetic, not tuning -- those pow() calls are
// double precision and the ESP32-S3's FPU is single precision, so every one is software
// emulated. It stays in the tree because tools/render_preview.py and test/test_epdopt run it
// on the host, where it costs nothing and where being able to reproduce upstream's actual
// default is worth having.
typedef enum {
    EPD_RANGE_ACCURATE = 0,
    EPD_RANGE_FAST,
} epd_range_quality_t;

// One entry of an error-diffusion kernel: where the error goes and how much of it.
typedef struct {
    int8_t dx;
    int8_t dy;
    double factor;
} epd_diffusion_tap_t;

typedef struct {
    const epd_diffusion_tap_t *taps;
    size_t count;
} epd_diffusion_kernel_t;

// diffusion-maps.ts:2-7 and :37-52. Only these two are ported: floydSteinberg is the
// default and what four of the six presets ask for, stucki is what `soft` asks for.
// Adding another is a table, and the factors are already written down upstream.
extern const epd_diffusion_kernel_t EPD_DIFFUSION_FLOYD_STEINBERG;
extern const epd_diffusion_kernel_t EPD_DIFFUSION_STUCKI;

typedef struct {
    // Six entries in epdoptimize's canonical role order. Use one of the
    // EPD_PALETTE_EPDOPT_* tables; a differently ordered palette breaks distance ties
    // differently and will not match the reference.
    const epd_palette_entry_t *palette;

    // --------------------------------------------------------------- tone mapping
    // False means the reference's `toneMapping: undefined` -- the stage does not run at
    // all, which is not the same as EPD_TONE_MODE_OFF (that still runs the exposure and
    // saturation work, just not the tone curve).
    bool tone_enabled;
    epd_tone_mode_t tone_mode;
    double exposure;            // stops; 0 is neutral
    double saturation;          // 0 is neutral, 0.5 means 1.5x, -1 removes saturation
    double contrast;            // 0 is neutral
    double strength;            // S-curve strength
    double shadow_boost;
    double highlight_compress;
    double midpoint;

    // ----------------------------------------------------------- range compression
    epd_range_mode_t range_mode;
    epd_range_quality_t range_quality;
    double range_strength;      // 0-1
    double low_percentile;      // EPD_RANGE_MODE_AUTO only
    double high_percentile;

    // ------------------------------------------------------------ error diffusion
    const epd_diffusion_kernel_t *kernel;
    bool serpentine;
} epd_epdopt_t;

// The library's "balanced" preset (processing.ts:130-147) against the palette
// epdoptimize's demo selects: a neutral tone pass, full-strength display-range
// compression, Floyd-Steinberg, no serpentine. Faithful to upstream, and **host-only** --
// it selects EPD_RANGE_ACCURATE. It is the primary parity target and the thing
// tools/render_preview.py draws when asked for upstream's own default.
extern const epd_epdopt_t EPD_EPDOPT_BALANCED;

// The same with the cheap range compressor. Not a lower-quality setting of the above but a
// second algorithm the library also has, with its own byte-for-byte parity coverage.
//
// **Also host-only, and it was measured rather than assumed.** It has no `pow()` and no
// `cbrt()` anywhere and it still cost 24.0 s a photograph against the row path's 1.62 s, because
// the inner loops are all `double` on a single-precision FPU -- at 160 MHz, which is what the
// chip ran at that day. The frame therefore quantises with epd_dither.c; only epdoptimize's
// *palette* was adopted. Ticket 32.
//
// This config is also the timing reference for src/epd_adjust.c, which is these two stages in
// single precision: `env:photo`'s `adj-double` arm runs `epd_epdopt_tone_map()` and
// `epd_epdopt_range_compress()` with exactly these settings, so the two implementations are
// compared on one photograph in one build rather than across two sessions. 8 259 ms against
// 668.9 ms at the time of writing.
extern const epd_epdopt_t EPD_EPDOPT_BALANCED_FAST;

// -------------------------------------------------------------------------- stages
//
// The first two are per-pixel and order-independent, so they take a flat RGB888 run and
// do not care about canvas rotation. Diffusion does care, and takes the canvas.

// processing.ts:1095-1223, minus the level-compression fusion (no ported preset sets it).
void epd_epdopt_tone_map(uint8_t *rgb, size_t pixels, const epd_epdopt_t *cfg);

// processing.ts:826-975. Returns false only when EPD_RANGE_MODE_AUTO cannot get its
// 40 KB lightness histogram; every other refusal (mode off, zero strength, a degenerate
// palette range) is the reference declining to do anything and reports true.
//
// That 40 KB is worth reading twice before using AUTO on the device: internal RAM is the
// scarce resource here and shipping idle leaves about 72 KB of it. EPD_RANGE_MODE_DISPLAY
// allocates nothing.
bool epd_epdopt_range_compress(uint8_t *rgb, size_t pixels, const epd_epdopt_t *cfg);

// dither.ts:1009-1078. Replaces every pixel of the canvas with its palette colour,
// diffusing the quantisation error forward in *logical* raster order. In place: the
// reference mutates its ImageData too, and the canvas is refilled from the decoder for
// every image.
//
// This deliberately stops at RGB rather than emitting nibbles. Once it has run, every
// pixel is exactly a palette entry, so epd_dither_row_none_cfg() packs the canvas
// losslessly and the odd-width padding rule stays in the one place that already had it.
void epd_epdopt_diffuse(epd_canvas_t *c, const epd_epdopt_t *cfg);

// The stages in order, which is what every caller wants; the three above are exposed for
// the parity test and for the one place that needs to stop halfway.
//
// `diffuse` false is epdoptimize's `quantizationOnly`: tone mapping and range compression
// happen either way, and skipping the diffusion is the whole of FR-3.3's "Nearest" mode. It
// leaves the canvas in full colour, so the caller's nearest-match pack does the quantising --
// which is also true after diffusion, where that pack is exact.
bool epd_epdopt_render(epd_canvas_t *c, const epd_epdopt_t *cfg, bool diffuse);

#endif // EPD_EPDOPT_H
