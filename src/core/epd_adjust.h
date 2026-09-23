// epdoptimize's per-pixel adjustment stages, in single precision, for the device.
//
// **This is not the port. `src/core/epd_epdopt.c` is the port** -- byte-for-byte verified against
// the library and host-only. This file is the same stages written to run on this SoC, and it
// exists because of one measurement: the ported tone mapping plus range compression cost
// **12 826 ms** on one 400 x 600 photograph (2026-09-05,
// `.scratch/captures/photo-epdopt-fast-20260905-145215.log`, `env:photo`, one run), against the
// shipping row path's 1618 ms. That is ~53 us per pixel for arithmetic with no transcendentals
// in it, and the reason is that every intermediate there is a `double` on an FPU that only has
// single precision, plus `epd_clamp_byte()`'s `isfinite` and `floor` three times per pixel.
//
// **Every millisecond in this file and in epd_adjust.c was measured at 160 MHz with a 32-byte
// data cache line, and the project moved to 240 MHz and 64 bytes later the same day.** The
// cost of tone plus range together is **668.9 ms**, against 8 259 ms for the ported pair in the
// same build. The figures are left as they were taken because each one belongs to an arm that
// isolated one variable, and re-scaling them by a clock ratio afterwards would turn four
// measurements into four estimates. `docs/measurements.md` carries both sets.
//
// The hypothesis this file exists to test was: **the cost was the type, not the algorithm.**
// It was, by a factor of 12.4, which is why epdoptimize's auto flow runs inline on every render
// and needs no pre-rendered cache. src/core/epd_auto.c is the other half -- what the settings should
// be for a given picture.
//
// **Therefore this is deliberately NOT a parity port and must never be given one's acceptance
// test.** Single-precision `powf` in the LUT build and single-precision division per pixel both
// move bytes by ones. `test/test_adjust/` compares the two original stages against
// `epd_epdopt.c`'s double ones within a stated tolerance, and `test/test_epdopt/` keeps that
// double path pinned to the library itself; the chain is what makes "close to upstream"
// checkable without claiming "identical to upstream", which would be false. The three stages
// added for the auto flow have no double counterpart here, so they are pinned straight to the
// library with a stated tolerance instead -- see epd_adjust.c.
//
// **What is already established, before any hardware ran.** `xtensa-esp32s3-elf-nm -u` on the
// two objects in the same `env:photo` build: `epd_epdopt.c.o` references fourteen soft-double
// helpers (`__adddf3`, `__muldf3`, `__divdf3`, `__fixdfsi`, ...) and this file references
// exactly one, `__divsf3`. So the rewrite did change the arithmetic rather than only the
// spelling of the types, which is the thing a fast timing figure would otherwise be assumed to
// mean.
//
// That one remaining helper is worth knowing about: **the S3's FPU has no divide instruction**,
// so every `/` below is a libgcc call, and the pixel loops make two or three per pixel. Several
// of them are removable -- a divisor that is an integer 0-255 can be indexed out of a 256-entry
// reciprocal table exactly, and a divisor that is a loop constant can be hoisted -- and **none of
// it is done**, because the owner's standing position is that speed is not being asked for at
// this stage. Each removal costs something: a hoist moves where the rounding happens and so gives
// up byte-identity with the library, and a table costs 1 KB of a task stack that has already
// caused a reboot loop in this project once. `epd_adjust.c` names the two that were tried and
// taken out again, at the point where they would go.
//
// Free of ESP-IDF, so it builds and is tested under env:native with the rest of the pure logic.

#ifndef EPD_ADJUST_H
#define EPD_ADJUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_dither.h"
#include "epd_epdopt.h"

// The mode enums are epd_epdopt.h's rather than a second copy under new names: these stages
// are the same stages and a divergence between the two spellings would be a silent bug the day
// a suggestion is mapped from one to the other. EPD_RANGE_ACCURATE has no meaning here -- there
// is no L*a*b* path in this file at all -- and selecting it falls back to the fast one rather
// than pretending.
//
// **Note what that means for the auto flow: upstream's auto never sets `quality`, so its own
// default is the accurate range compressor.** This runs the fast one. They are two algorithms
// with different output (epd_epdopt.h), not two settings of one, and the accurate one ran for
// over 90 s here without finishing.
typedef struct {
    const epd_palette_entry_t *palette;

    bool tone_enabled;
    epd_tone_mode_t tone_mode;
    float exposure;            // stops; 0 is neutral
    float saturation;          // 0 is neutral, 0.5 means 1.5x, -1 removes saturation
    float contrast;            // 0 is neutral
    float strength;            // S-curve strength
    float shadow_boost;
    float highlight_compress;
    float midpoint;

    epd_range_mode_t range_mode;
    float range_strength;      // 0-1
    float low_percentile;      // EPD_RANGE_MODE_AUTO only
    float high_percentile;
} epd_adjust_t;

// The same settings as EPD_EPDOPT_BALANCED_FAST, so the timing arm in photo_main.c measures
// this file against that one figure and not against a different workload.
extern const epd_adjust_t EPD_ADJUST_BALANCED;

// ------------------------------------------------- the three stages the auto flow adds
//
// Parameters only; src/core/epd_auto.c decides them. They live here rather than there so that this
// file stays the one that owns "what a pixel stage takes" and epd_auto.h stays the one that
// owns "which values a picture wants" -- and because the other way round is a circular include.

// applyLevelCompression, mode "luma" only (processing.ts:1074-1092). Squeezes the picture into a
// black..white band, so it is containment rather than expansion. `perChannel` is not ported: no
// arm of the auto flow asks for it.
typedef struct {
    bool enabled;
    float black;
    float white;
} epd_level_t;

// applyPaperNormalization, mode "warmPaper" (processing.ts:456-521).
typedef struct {
    bool enabled;
    float strength;
    float min_luma;
    float saturation_threshold;
    float warm_bias_threshold;
    float black_anchor;
    float preserve_red;
    uint8_t paper_white[3];
} epd_paper_t;

// getWhitePreservationPlan / applyWhitePreservation (dither.ts:328-419).
typedef struct {
    bool enabled;
    float percentile;       // of the low-saturation pixels' luma
    float min_luma;         // below this the plan is abandoned entirely
    float max_saturation;   // what counts as a white candidate
} epd_white_t;

// ------------------------------------------------------------------------- the stages
//
// **Every stage has a region form and a flat form, and the region form is the one the device
// uses.** The canvas is matted white around the picture (epd_image_draw()), and two of these
// measure statistics over the pixels they are given: EPD_RANGE_MODE_AUTO's percentiles and
// white preservation's p99. Hand either a 20 % white matte and `auto` degenerates to `display`
// while the white plan latches onto the matte instead of onto the paper in the photograph. The
// flat forms are the region forms at `height = 1`, and test_adjust asserts they agree.
//
// `rgb` points at the region's top-left pixel; `stride` is the bytes per row of the buffer that
// contains it, which is the whole canvas width times three, not the region's. All of these are
// per-pixel and order-independent, so they can walk physical rows and ignore canvas rotation --
// epd_canvas_physical_rect() is how a caller turns a logical rectangle into one.

// processing.ts:1095-1223, minus the level-compression fusion. Callers run tone before range:
// reversing them would compress a range the tone curve then moves back out of.
void epd_adjust_tone(uint8_t *rgb, size_t pixels, const epd_adjust_t *cfg);
void epd_adjust_tone_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                            const epd_adjust_t *cfg);

// EPD_RANGE_MODE_AUTO measures the image's own luma percentiles from a 256-bin integer
// histogram on the stack -- 1 KB, not the accurate path's 40 KB heap allocation, and the same
// binning the fast reference already uses.
void epd_adjust_range(uint8_t *rgb, size_t pixels, const epd_adjust_t *cfg);
void epd_adjust_range_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                             const epd_adjust_t *cfg);

// Runs before tone mapping, and only for the posterScan arm.
void epd_adjust_paper_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                             const epd_paper_t *cfg);

// Runs **after** range compression, not fused into the tone LUT. That is not a choice: upstream
// fuses level compression into the tone pass only when it is `perChannel` and the range stage is
// off (processing.ts:1103-1106, :1233-1248), and every arm of the auto flow that sets level
// compression sets mode "luma" with a live range stage, so it always lands here.
void epd_adjust_level_region(uint8_t *rgb, size_t width, size_t height, size_t stride,
                             const epd_level_t *cfg);

// ------------------------------------------------------------------ white preservation
//
// Two calls because it is two things: a plan measured on the **untouched source**, and an
// application after every other stage has run (dither.ts:509-523). Between them the source is
// gone, so something has to be remembered.
//
// **Upstream remembers a `Float64Array` of every pixel's luma -- 1.9 MB for this canvas.** This
// keeps one bit per pixel instead, by making the threshold known before the bits are written:
// pass one histograms the low-saturation lumas, pass two takes the percentile and then marks
// each pixel that satisfies *both* conditions. 30 KB rather than 270 KB, and the comparison
// against the threshold stays exact because the bit is computed from the same unrounded luma
// upstream compares.

typedef struct {
    bool active;
    uint8_t target[3];      // the palette's lightest entry, by Rec. 709 luma
    float target_luma;
    float source_white_luma; // raw, for the log line -- the threshold the percentile landed on
} epd_white_plan_t;

// Bytes of `bits` a region of this size needs.
size_t epd_adjust_white_bits_bytes(size_t width, size_t height);

// False means there is nothing to preserve, and `out->active` is left false: no low-saturation
// pixels at all, or a p99 white darker than `min_luma`, which is upstream declining because the
// picture has no paper white in it. A missing or short `bits` is also a false, so a caller that
// could not allocate gets a refusal rather than a wild write.
bool epd_adjust_white_plan(const uint8_t *rgb, size_t width, size_t height, size_t stride,
                           const epd_palette_entry_t *palette, const epd_white_t *cfg,
                           uint8_t *bits, size_t bits_bytes, epd_white_plan_t *out);

// Replaces every marked pixel that the adjustments left darker than the palette's white with
// that white. A plan whose `active` is false is a no-op, so the caller does not branch.
void epd_adjust_white_apply(uint8_t *rgb, size_t width, size_t height, size_t stride,
                            const uint8_t *bits, const epd_white_plan_t *plan);

#endif // EPD_ADJUST_H
