// Ticket 07: the first photograph this project's firmware puts on the panel.
//
// Everything before this displayed solid colours. This runs the whole pipeline --
// decode, fit and centre, dither, pack, refresh -- on three real files in three formats,
// and prints a capture marker after each so tools/bringup_capture.py photographs the
// result.
//
// The images are embedded in flash rather than read from storage. Ticket 07 is blocked
// on the display pipeline, not on the filesystem, and mixing the two would mean a blank
// screen could be either. Files arrive with ticket 08.
//
// Decode and dither are timed separately from the refresh. That is not tidiness: Phase 0
// needs the per-phase split, and the shipping library makes it impossible by dithering
// inside the SPI transaction.

#ifdef BUILD_PHOTO

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "epd_adjust.h"
#include "epd_auto.h"
#include "epd_canvas.h"
#include "epd_classify.h"
#include "epd_diffuse.h"
#include "epd_dither.h"
#include "epd_epdopt.h"
#include "epd_flow.h"
#include "epd_panel.h"
#include "epd_format.h"
#include "epd_image.h"
#include "assets_data.h"
#include "trace.h"

static const char *TAG = "photo";

#define SETTLE_MS 3000
// Long enough that the host's capture finishes before the next refresh starts. A flatbed
// scan of the panel region takes 11-12 s, and at the old 6000 the refresh began 8 s after
// the marker -- so every scan but the last of a run carried the next refresh in its bottom
// 40% (mean 86-91 LSB against 123 for the one clean scan). See issues/08's comments.
#define DWELL_MS 20000


typedef enum { DITHER_QUALITY, DITHER_NEAREST } dither_mode_t;

// How many times to run the adjustment stages before the shot is drawn. Each run re-decodes
// first, because the stages work in place. Ten samples of a 0.3 s stage cost 3 s; ten samples
// taken by refreshing ten times would cost 400 s, and the refresh is not what is being
// measured -- so the repetition lives here rather than in the number of shots.
#define ADJUST_RUNS 10
// Three for the double reference: enough to see whether the recorded 12 826 ms reproduces and
// with what spread, without spending 128 s and forty watchdog reports to learn it.
#define ADJUST_REF_RUNS 3

// Which order the diffusion walks the picture in. **At rotation 0 these two are the same walk**
// (epd_canvas.c's offset_of()), so an arm that compares them has to be at rotation 1 or it cannot
// come out either way -- which is the whole reason the rot1 pair below exists.
typedef enum {
    DIFFUSE_OFF = 0,
    DIFFUSE_LOGICAL,  // the order the picture was drawn, which is what epdoptimize diffuses in
    DIFFUSE_PHYSICAL, // the order the panel is scanned in: sequential through PSRAM
} diffuse_order_t;

typedef struct {
    const char *label;
    const uint8_t *data;
    const size_t *len;
    dither_mode_t mode;
    uint8_t rotation;
    // Which colours the quantiser matches against. NULL keeps the stock device palette,
    // so every shot written before ticket 19 renders exactly as it did.
    const epd_render_t *render;
    // The waveform rate. EPD_FRS_STOCK for everything except the frs-ab group, which is
    // ticket 08's next step: the same photograph at two rates, on the glass, minutes apart
    // in one lighting condition.
    uint8_t frs;
    // The single-precision adjustment stages, run on the whole canvas before the quantiser.
    // NULL for every shot written before this, so none of them changes.
    const epd_adjust_t *adjust;
    // The same two stages from the double port, for the same-run A/B. NULL unless this shot
    // is the reference arm. **Slow on purpose**: 12.8 s of CPU 0 with the watchdog barking
    // four times, which is the figure the float arm exists to be compared against. Having
    // both in one run is what makes the comparison a measurement rather than two sessions.
    const epd_epdopt_t *adjust_ref;
    // The whole auto flow: classify the picture, choose its settings, run all five stages. Unlike
    // the two above, `render` is ignored for such a shot -- the plan decides the tone compression
    // through epd_auto_row_tone(), because exactly one range compression has to happen and which
    // one it is depends on what the plan asked for.
    bool auto_flow;
    // The integer error-diffusion quantiser (epd_diffuse.h), over the picture's own rectangle.
    // Not OFF also forces the pack to nearest at EPD_TONE_NONE, because after diffusion every
    // pixel already is a palette entry -- see the note on the arms below.
    diffuse_order_t diffuse;
} shot_t;

// The palette comparison, on the algorithm that ships. epdoptimize's calibrations against
// the manual's, all through M5GFX's pair search at full tone compression, so the palette is
// the only thing that varies.
//
// `epdopt-original` is the control and it varies **two** things, which is worth stating
// rather than discovering: its white is (255,255,255) and its black (0,0,0), so
// tone_compress() becomes an identity and this arm is "no calibration *and* no compression".
// It cannot separate the two on its own.
static const epd_render_t RENDER_EPDOPT_SPECTRA6 = {EPD_PALETTE_EPDOPT_SPECTRA6, EPD_TONE_FULL};
static const epd_render_t RENDER_EPDOPT_ORIGINAL = {EPD_PALETTE_EPDOPT_ORIGINAL, EPD_TONE_FULL};

// **Tone compression off, and that is not a detail: it would otherwise happen twice.**
// `epd_render_t.tone` IS display-mode range compression, per channel, in integers
// (epd_dither.c's tone_compress()), and the adjustment stages below do the same job in luma
// with a chroma guard. Leaving the row path's copy on would squeeze the picture into the
// panel's range twice and flatten both ends. Every arm that runs an adjustment stage
// therefore quantises with this cfg, and every arm that does not keeps EPD_TONE_FULL.
static const epd_render_t RENDER_EPDOPT_NO_TONE = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_NONE};

// EPD_ADJUST_BALANCED with **one field moved** -- the saturation. Written out rather than copied
// because C cannot initialise one const object from another at file scope, and the fields are
// worth being able to diff by eye: every one below is EPD_ADJUST_BALANCED's except `saturation`,
// which is 0.12 (upstream's `linearAdjustmentFromMultiplier(1.12)`, what src/core/epd_auto.c gives a
// photograph whose lumaStdDev is 42 or less).
static const epd_adjust_t ADJUST_SATURATED = {
    .palette = EPD_PALETTE_EPDOPT_AITJCIZE,
    .tone_enabled = true,
    .tone_mode = EPD_TONE_MODE_CONTRAST,
    .exposure = 0.0f,
    .saturation = 0.12f,
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

static const shot_t SHOTS[] = {
    // ------------------------------------------- the cost of the adjustment stages, Stage 0
    //                                             of the epdoptimize-demo plan
    //
    // The question: was the ported pipeline's 12 826 ms for tone mapping plus range
    // compression (2026-09-05, one run) the *algorithm* or the `double`? If it was the type,
    // epdoptimize's per-image auto flow can run inline on every render; if it was not, the
    // frame has to pre-render at import and cache, which is a much larger change.
    //
    // Both arms are in one run deliberately: the two figures then share a photograph, a build,
    // a clock rate and a panel temperature, which two sessions cannot. `adj-float` reports ten
    // samples and `adj-double` three, each printed as its own `# adjust` line -- the median is
    // computed at analysis time, where being wrong is free.
    //
    // **This arm can come out slow, and that is the point.** Above about 2 s the inline plan is
    // refuted and the plan's fallback is what gets built. `dither_ms` is not comparable between
    // these two arms and `pal-aitjcize` below: these carry EPD_TONE_NONE, so the row path does
    // marginally less work. Read `adjust_*_ms` for the stage cost and take the row path's own
    // figure from `pal-aitjcize`.
    {"adj-float", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &RENDER_EPDOPT_NO_TONE, EPD_FRS_STOCK, &EPD_ADJUST_BALANCED, NULL, false},
    {"adj-double", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &RENDER_EPDOPT_NO_TONE, EPD_FRS_STOCK, NULL, &EPD_EPDOPT_BALANCED_FAST, false},

    // ------------------------------------------- the branch Stage 0 did not measure, and the
    //                                             whole auto flow. Stage 2b of the same plan.
    //
    // **`adj-float` above took the cheap path and nobody noticed for a day.**
    // EPD_ADJUST_BALANCED has a neutral saturation, so epd_adjust_tone() takes its
    // three-LUT-lookups branch and reported 66.7 ms. src/core/epd_auto.c asks for a *non-neutral*
    // saturation for six of the seven image kinds -- including `photo` when lumaStdDev <= 42,
    // which is an ordinary photograph -- and every one of those takes the HSL round trip
    // instead: three divisions, an fmodf, a floorf and two fabsf per pixel. This arm is that
    // path and nothing else; `adj-float` is its control and must not move, because the two
    // configs differ in exactly one field.
    //
    // **Ran 2026-09-05: tone 708.7 ms against adj-float's 38.1, and range moved 0.1 ms.** So the
    // control held and the branch is 18.6x. Ten runs agreed to 0.1 ms.
    {"adj-float-sat", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &RENDER_EPDOPT_NO_TONE, EPD_FRS_STOCK, &ADJUST_SATURATED, NULL, false},

    // classify -> suggest -> all five stages, each timed separately, on the photograph the two
    // arms above use. This is what src/app/app_display.c runs per render when the setting is on, so
    // it is the figure the plan's 3 000 ms pre-registration is about -- and it is the only arm
    // here whose settings come from the picture rather than from a table.
    //
    // It renders with the plan's own choices, so this shot is also the visual arm: the tone
    // decision comes from epd_auto_row_tone() rather than from `render` below, which is why that
    // column is NULL.
    //
    // **Ran 2026-09-05: 1 916.8 ms total** -- classify 193.3, suggest 0.09, white plan 372.0,
    // tone 708.9, range 628.8, white apply 13.8, with paper and level not selected for this
    // picture. No watchdog. **And it did not exercise the region path**: this photograph fills the
    // panel, so the fit rectangle is the whole canvas and there is no white matte. A shot whose
    // source has a different aspect ratio is what would close that, and it is the largest gap
    // left in this arm.
    {"auto-full", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0, NULL,
     EPD_FRS_STOCK, NULL, NULL, true},

    // The same flow on a MATTED canvas, which the arm above cannot reach: this photograph is
    // 400 x 600 and so is the panel, so `auto-full`'s fit rectangle is the whole canvas and
    // there is no surround at all. Rotation 1 presents the canvas as 600 x 400, so a 2:3 source
    // becomes height-limited and lands as 266 x 400 logical with white either side --
    // region=400x266 physically -- and epd_image_draw() fills that surround with 255,255,255
    // before it draws, so the matte is real white and the per-run re-decode restores it.
    //
    // **Why the matte is the point.** epd_adjust_range_region()'s EPD_RANGE_MODE_AUTO
    // percentiles and epd_adjust_white_plan()'s p99 are each measured over whatever pixels they
    // are handed, so a matte makes the first degenerate into `display` and the second latch onto
    // the surround instead of onto the paper in the photograph. test_adjust covers that on the
    // host against a sentinel-filled surround; nothing has covered it on the glass.
    //
    // **What this arm does NOT reach**, so that nobody reads it as more than it is. At rotation 1
    // epd_canvas_physical_rect() returns px=0 pw=400, the full canvas width, so `stride ==
    // width * 3` and the region is contiguous with an offset. The strided form, where the stride
    // exceeds the region, needs a source TALLER than 2:3 at rotation 0 -- a landscape source is
    // width-limited and would look like it tested striding without doing so. That arm goes
    // through env:frame and an uploaded photograph, which costs no flash.
    //
    // It is also the first rotated run of epd_classify_canvas(), which walks the region in
    // LOGICAL coordinates through epd_canvas_pixel() (epd_classify.c:693) -- so every logical
    // row step is a physical column step across PSRAM. Read classify_ms against auto-full's
    // 193.3 scaled by the pixel count (106 400 against 240 000, so 0.44x); far above that is the
    // transposed access, and the `orientation` setting reaches the same path.
    {"auto-matte", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 1, NULL,
     EPD_FRS_STOCK, NULL, NULL, true, DIFFUSE_OFF},

    // ------------------------------------------- the integer error diffusion, Stage 2
    //                                             of the error-diffusion plan
    //
    // Three arms, and the third exists because of an apparatus problem rather than a second
    // question. `diffuse-fs` is the cost against the recorded double figure. The rot1 pair is the
    // scan-order comparison: diffusing in the order the picture was drawn is what epdoptimize does
    // and what keeps the dither from turning with the frame's rotation setting, but at rotation 1 a
    // logical row strides -1200 bytes, so it touches about one useful pixel per 64-byte cache line
    // where the physical walk touches 21. **There is no rotation-0 counterpart of that pair**,
    // because at rotation 0 the two walks are literally the same walk and the arm would report
    // "identical" by construction -- the ticket-18 failure shape.
    //
    // These pack with nearest at EPD_TONE_NONE rather than with the pair search, and that is not a
    // choice: after diffusion every pixel already is a palette entry, so nearest is lossless, and
    // tone compression at the pack would move the palette's own white off itself and re-match it.
    // The main loop forces both, so the `mode` and `render` columns here are ignored.
    //
    // Both rot1 arms give region=400x266 -- the source is wider than 2:3, so it is width-limited
    // and matted top and bottom. That is fine for a timing arm and does not reach the strided
    // region form; see the auto-matte note above for why that one needs env:frame.
    {"diffuse-fs", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0, NULL,
     EPD_FRS_STOCK, NULL, NULL, false, DIFFUSE_LOGICAL},
    {"diffuse-fs-rot1", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 1, NULL,
     EPD_FRS_STOCK, NULL, NULL, false, DIFFUSE_LOGICAL},
    {"diffuse-fs-rot1-phys", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 1,
     NULL, EPD_FRS_STOCK, NULL, NULL, false, DIFFUSE_PHYSICAL},

    // ------------------------------------------- the palette swap, 2026-09-05, ticket 19's
    //                                             decision re-opened with a sixth candidate
    //
    // First in the table so a capture can cover this group alone; everything below it was
    // shot and scanned on 2026-09-05 and does not need another refresh
    // (.scratch/captures/photo-epdopt-20260905-142247.log and -fast-20260905-145215.log).
    // A capture sized to this group exits non-zero because @@DONE never arrives, which is
    // the form docs/hardware-runs.md documents and is not a failure.
    //
    // Same photograph, same algorithm, same run: only the palette moves. `pal-manual` is
    // what the frame drew until 2026-09-05 and `pal-aitjcize` is what it draws now, so this
    // group is the old-versus-new comparison *in one session*, which the previous two runs
    // could not be. `pal-spectra6` is a second epdoptimize calibration and `pal-original` is
    // the uncalibrated control.
    //
    // What this group is for, specifically: a flatbed scan showed `pal-manual` rendering dark
    // brown hair green, and the same photograph through the aitjcize palette rendering it
    // brown. That was seen across two different algorithms, so the palette is the suspect --
    // this group is the test that varies nothing else.
    //
    // The ported epdoptimize pipeline is deliberately **not** here as a *shot*. It costs 24.0 s a
    // photograph against this path's 1.62 s and starves CPU 0 while it runs; its scans from
    // the 14:52 run are the record, and src/core/epd_epdopt.c is host-only now. Both of those figures
    // are 160 MHz / 32-byte-cache-line numbers -- see the `adj-*` group above, which is where its
    // first two stages are timed against the single-precision rewrite in the current build.
    {"pal-aitjcize", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_EPDOPT, EPD_FRS_STOCK, NULL, NULL, false},
    {"pal-manual", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_MANUAL, EPD_FRS_STOCK, NULL, NULL, false},
    {"pal-spectra6", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &RENDER_EPDOPT_SPECTRA6, EPD_FRS_STOCK, NULL, NULL, false},
    {"pal-original", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &RENDER_EPDOPT_ORIGINAL, EPD_FRS_STOCK, NULL, NULL, false},
    // Which colours moved, rather than whether it looks right.
    {"pal-chart", asset_test_chart_png, &asset_test_chart_png_len, DITHER_QUALITY, 0,
     &EPD_RENDER_EPDOPT, EPD_FRS_STOCK, NULL, NULL, false},

    // ------------------------------------------------------------- shot before 2026-09-05
    //
    // Panel-sized PNG, both reduction paths, so the difference FR-3.3 describes can be
    // seen on the glass rather than argued about from source.
    {"chart-png-dither", asset_test_chart_png, &asset_test_chart_png_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
    {"chart-png-nearest", asset_test_chart_png, &asset_test_chart_png_len, DITHER_NEAREST, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
    // A real photograph, and the JPEG decoder.
    {"photo-jpg-dither", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
    // Half size and bottom-up rows: upscaling plus the BMP path.
    {"small-bmp-dither", asset_test_small_bmp, &asset_test_small_bmp_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
    // Same photo turned: does rotation 1 come out upright, or mirrored?
    {"photo-jpg-rot1", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 1,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},

    // Ticket 19's comparison, and the two shots its acceptance rests on: the same
    // photograph, quantised against the colours the panel is SENT and then against the
    // colours it SHOWS, with the input range compressed into what it can reach. They sit
    // next to each other in the log so the two photographs are taken minutes apart under
    // one lighting condition.
    {"ab-stock", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_STOCK, EPD_FRS_STOCK, NULL, NULL, false},
    {"ab-measured-none", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY,
     0, &EPD_RENDER_MEASURED_NONE, EPD_FRS_STOCK, NULL, NULL, false},
    {"ab-measured-half", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY,
     0, &EPD_RENDER_MEASURED_HALF, EPD_FRS_STOCK, NULL, NULL, false},
    {"ab-measured", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_MEASURED, EPD_FRS_STOCK, NULL, NULL, false},
    {"ab-manual", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_MANUAL, EPD_FRS_STOCK, NULL, NULL, false},

    // Ticket 08's next step, in the shape tickets 19 and 20 settled: the SAME photograph
    // and the SAME quantiser at two waveform rates, so the only variable is the rate. The
    // 2026-09-04 sweep found 0x01 (23.1 s, +54 % over stock) moving green and blue AWAY
    // from white by 10-14 LSB on a six-colour chart -- the opposite direction to the fast
    // settings -- and the bar on this project is a visible improvement on the glass, not a
    // scanner delta. Two draws per setting because the panel's first draw of new content
    // differs from its second by more than a real effect; the trailing stock draw is the
    // reversibility check and leaves the panel where it started.
    {"frs-ab-stock-1", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
    {"frs-ab-stock-2", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
    {"frs-ab-slow-1", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, 0x01, NULL, NULL, false},
    {"frs-ab-slow-2", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, 0x01, NULL, NULL, false},
    {"frs-ab-stock-3", asset_test_photo_jpg, &asset_test_photo_jpg_len, DITHER_QUALITY, 0,
     &EPD_RENDER_DEFAULT, EPD_FRS_STOCK, NULL, NULL, false},
};
#define SHOT_COUNT (sizeof(SHOTS) / sizeof(SHOTS[0]))

static uint8_t *s_canvas_buf;
static uint8_t *s_packed;
static epd_canvas_t s_canvas;

// Decode one shot onto the canvas. Separate from the loop because the adjustment stages work
// in place, so timing them more than once means putting the pixels back first, and re-decoding
// is the only way to do that which cannot itself be wrong.
typedef struct {
    int32_t width;
    int32_t height;
    epd_image_format_t format;
    float decode_ms;
} decoded_t;

static esp_err_t decode_shot(const shot_t *s, decoded_t *out)
{
    epd_image_reader_storage_t storage;
    epd_image_reader_t *reader = NULL;
    esp_err_t err = epd_image_open_mem(&storage, s->data, *s->len, &reader);
    if (err != ESP_OK) {
        return err;
    }

    out->width = epd_image_width(reader);
    out->height = epd_image_height(reader);
    out->format = epd_image_format(reader);

    const int64_t t0 = esp_timer_get_time();
    err = epd_image_draw(reader, &s_canvas);
    const int64_t t1 = esp_timer_get_time();
    epd_image_close(reader);

    out->decode_ms = (float)(t1 - t0) / 1000.0f;
    return err;
}

// The two per-pixel adjustment stages, timed separately and printed per run. One line per run
// with the raw microsecond differences in it: a median or a verdict computed here would be a
// second thing that can be wrong, and on this board finding that out costs a rebuild, a flash
// and 15.6 s a shot.
static void time_adjust_stages(const shot_t *s, int runs)
{
    const size_t pixels = (size_t)s_canvas.width * (size_t)s_canvas.height;

    for (int run = 0; run < runs; run++) {
        if (run > 0) {
            decoded_t again = {0};
            if (decode_shot(s, &again) != ESP_OK) {
                printf("# adjust %s: re-decode failed, stopping at run %d\n", s->label, run);
                return;
            }
        }

        const int64_t a0 = esp_timer_get_time();
        if (s->adjust != NULL) {
            epd_adjust_tone(s_canvas.rgb, pixels, s->adjust);
        } else {
            epd_epdopt_tone_map(s_canvas.rgb, pixels, s->adjust_ref);
        }
        const int64_t a1 = esp_timer_get_time();
        bool ok = true;
        if (s->adjust != NULL) {
            epd_adjust_range(s_canvas.rgb, pixels, s->adjust);
        } else {
            ok = epd_epdopt_range_compress(s_canvas.rgb, pixels, s->adjust_ref);
        }
        const int64_t a2 = esp_timer_get_time();

        printf("# adjust %s impl=%s run=%d/%d ok=%d tone_ms=%.1f range_ms=%.1f total_ms=%.1f\n",
               s->label, s->adjust != NULL ? "float" : "double", run + 1, runs, (int)ok,
               (double)(a1 - a0) / 1000.0, (double)(a2 - a1) / 1000.0,
               (double)(a2 - a0) / 1000.0);
        fflush(stdout);

        // Let IDLE0 run between samples. The double arm starves CPU 0 for 12.8 s at a
        // stretch and the task watchdog fires four times per photograph; that is recorded
        // behaviour rather than a new failure, but there is no reason to make it worse.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ------------------------------------------------------------ the integer error diffusion
//
// What this arm settles, and what it cannot. The recorded cost of the double implementation is
// 11.2 s a photograph at 160 MHz with a 32-byte cache line (docs/measurements.md:181-190,
// from 23 966 ms with diffusion against 12 826 ms without in one capture), which scales to ~7.4 s
// on this build by the measured 0.666 clock-and-cache factor. src/core/epd_diffuse.c removes the
// soft-float multiply and the twelve isfinite+floor calls per pixel that measurements.md names as
// the cost, and test/test_diffuse pins it bit-identical to the library. **How much that is worth
// in milliseconds is not derivable from any of it**, which is what these arms are for.
//
// One call per run over the whole region: banding is bit-exact (test_diffuse covers it) but a band
// boundary is where the display task will yield, and mixing that into the arithmetic figure would
// make the number a mixture. The yield cost belongs to app_display.c, not here.
#define DIFFUSE_RUNS 10

// Fills `plan` is not this function's job -- diffusion takes no plan. It reports the region, the
// two byte steps and the raw milliseconds, and leaves the canvas quantised.
static bool time_diffuse(const shot_t *s, const decoded_t *dec, int runs)
{
    const epd_fit_t fit = epd_fit_centre(dec->width, dec->height,
                                         epd_canvas_logical_width(&s_canvas),
                                         epd_canvas_logical_height(&s_canvas));

    // Serpentine, because every errorDiffusion arm of epdoptimize's layered auto asks for it
    // (auto-processing.ts:388-391). The palette is the frame's default, which is also the one
    // test_diffuse's fixtures were generated against.
    const epd_diffuse_t cfg = {.palette = EPD_PALETTE_EPDOPT_AITJCIZE, .serpentine = true};

    for (int run = 0; run < runs; run++) {
        if (run > 0) {
            decoded_t again = {0};
            if (decode_shot(s, &again) != ESP_OK) {
                printf("# diffuse %s: re-decode failed, stopping at run %d\n", s->label, run);
                return run > 0;
            }
        }

        uint8_t *origin = NULL;
        ptrdiff_t step_x = 0, step_y = 0;
        int32_t w = 0, h = 0;

        if (s->diffuse == DIFFUSE_LOGICAL) {
            if (!epd_canvas_logical_walk(&s_canvas, fit.x, fit.y, fit.width, fit.height, &origin,
                                         &step_x, &step_y)) {
                printf("# diffuse %s: the logical walk refused the fit rectangle\n", s->label);
                return false;
            }
            w = fit.width;
            h = fit.height;
        } else {
            int32_t px = 0, py = 0, pw = 0, ph = 0;
            if (!epd_canvas_physical_rect(&s_canvas, fit.x, fit.y, fit.width, fit.height, &px, &py,
                                          &pw, &ph)) {
                printf("# diffuse %s: the physical rect refused the fit rectangle\n", s->label);
                return false;
            }
            origin = &s_canvas.rgb[((size_t)py * (size_t)s_canvas.width + (size_t)px) * 3u];
            step_x = 3;
            step_y = (ptrdiff_t)s_canvas.width * 3;
            w = pw;
            h = ph;
        }

        const int64_t d0 = esp_timer_get_time();
        epd_diffuse_rect(origin, w, h, step_x, step_y, &cfg, 0, h);
        const int64_t d1 = esp_timer_get_time();

        printf("# diffuse %s order=%s run=%d/%d region=%ldx%ld step_x=%ld step_y=%ld "
               "diffuse_ms=%.1f\n",
               s->label, s->diffuse == DIFFUSE_LOGICAL ? "logical" : "physical", run + 1, runs,
               (long)w, (long)h, (long)step_x, (long)step_y, (double)(d1 - d0) / 1000.0);
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

// --------------------------------------------------------------------- the whole auto flow
//
// classify -> suggest -> five per-pixel stages, timed one at a time. Same shape as
// time_adjust_stages(): raw microseconds per run, one line each, medians computed at analysis
// time where being wrong is free.
//
// **Pre-registered before the first run** (the epdoptimize-demo plan): the total added work must
// come in under 3 000 ms per 400 x 600 photograph. Non-refresh work is currently decode 264 +
// row dither 984 ms against a 15 017 ms refresh, so 3 000 keeps it under 4.3 s -- about 28 % of a
// refresh. Above that the plan's import-time cache branch is reopened rather than shipped slow.
#define AUTO_RUNS 10

static epd_classify_scratch_t s_auto_scratch;
static uint8_t *s_white_bits;

static bool auto_scratch_alloc(void)
{
    s_auto_scratch.samples = heap_caps_malloc(epd_classify_samples_bytes(), MALLOC_CAP_SPIRAM);
    s_auto_scratch.sample_capacity =
        epd_classify_samples_bytes() / sizeof(epd_classify_sample_t);
    s_auto_scratch.colour_counts =
        heap_caps_malloc(epd_classify_colour_counts_bytes(), MALLOC_CAP_SPIRAM);
    s_auto_scratch.tile_stamps =
        heap_caps_malloc(epd_classify_tile_stamps_bytes(), MALLOC_CAP_SPIRAM);
    s_auto_scratch.luma_bins =
        heap_caps_malloc(epd_classify_luma_bins_bytes(), MALLOC_CAP_SPIRAM);
    s_white_bits =
        heap_caps_malloc(epd_adjust_white_bits_bytes(EPD_WIDTH, EPD_HEIGHT), MALLOC_CAP_SPIRAM);

    return s_auto_scratch.samples && s_auto_scratch.colour_counts &&
           s_auto_scratch.tile_stamps && s_auto_scratch.luma_bins && s_white_bits;
}

// Fills `plan` and leaves the canvas adjusted. False means the arm could not run at all, which is
// not the same as a plan that chose to do nothing.
static bool time_auto_flow(const shot_t *s, const decoded_t *dec, epd_auto_plan_t *plan, int runs)
{
    const epd_palette_entry_t *palette = EPD_PALETTE_EPDOPT_AITJCIZE;
    const epd_fit_t fit = epd_fit_centre(dec->width, dec->height,
                                         epd_canvas_logical_width(&s_canvas),
                                         epd_canvas_logical_height(&s_canvas));

    int32_t px = 0, py = 0, pw = 0, ph = 0;
    if (!epd_canvas_physical_rect(&s_canvas, fit.x, fit.y, fit.width, fit.height, &px, &py, &pw,
                                  &ph)) {
        printf("# auto %s: fit %ld,%ld %ldx%ld is not inside the canvas\n", s->label,
               (long)fit.x, (long)fit.y, (long)fit.width, (long)fit.height);
        return false;
    }

    const size_t stride = (size_t)s_canvas.width * 3u;
    const size_t w = (size_t)pw;
    const size_t h = (size_t)ph;

    for (int run = 0; run < runs; run++) {
        if (run > 0) {
            decoded_t again = {0};
            if (decode_shot(s, &again) != ESP_OK) {
                printf("# auto %s: re-decode failed, stopping at run %d\n", s->label, run);
                return run > 0;
            }
        }

        uint8_t *origin = &s_canvas.rgb[((size_t)py * (size_t)s_canvas.width + (size_t)px) * 3u];

        const int64_t t0 = esp_timer_get_time();
        epd_classification_t c;
        const bool classified = epd_classify_canvas(&s_canvas, fit.x, fit.y, fit.width,
                                                    fit.height, &s_auto_scratch, &c);
        const int64_t t1 = esp_timer_get_time();
        if (!classified) {
            printf("# auto %s: the classifier refused the region\n", s->label);
            return false;
        }
        epd_auto_suggest(&c, palette, plan);
        const int64_t t2 = esp_timer_get_time();

        // The six stages, in epd_flow.h's order. This arm used to write the sequence out itself --
        // it was one of three hand-written copies, and it is the one whose per-stage figures
        // docs/measurements.md quotes, which is why epd_flow_apply() reports them rather
        // than only running the stages. Same six numbers, same order, pinned by test/test_flow.
        const epd_flow_region_t region = {
            .rgb = origin,
            .width = w,
            .height = h,
            .stride = stride,
            .palette = palette,
            .white_bits = s_white_bits,
            .white_bits_bytes = epd_adjust_white_bits_bytes(w, h),
        };
        epd_white_plan_t white;
        epd_flow_timing_t t;
        epd_flow_apply(&region, plan, &white, esp_timer_get_time, &t);
        const int64_t t8 = esp_timer_get_time();

        printf("# auto %s run=%d/%d kind=%s region=%ldx%ld nearest=%d white=%d(%.1f) level=%d "
               "paper=%d classify_ms=%.1f suggest_ms=%.2f wplan_ms=%.1f paper_ms=%.1f "
               "tone_ms=%.1f range_ms=%.1f level_ms=%.1f wapply_ms=%.1f total_ms=%.1f\n",
               s->label, run + 1, runs, epd_image_kind_name(plan->kind), (long)pw, (long)ph,
               plan->nearest ? 1 : 0, white.active ? 1 : 0, (double)white.source_white_luma,
               plan->level.enabled ? 1 : 0, plan->paper.enabled ? 1 : 0,
               (double)(t1 - t0) / 1000.0, (double)(t2 - t1) / 1000.0,
               (double)t.white_plan_us / 1000.0, (double)t.paper_us / 1000.0,
               (double)t.tone_us / 1000.0, (double)t.range_us / 1000.0,
               (double)t.level_us / 1000.0, (double)t.white_apply_us / 1000.0,
               (double)(t8 - t0) / 1000.0);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return true;
}

// ------------------------------------------------------------------ the memory floor
//
// **What does touching the whole canvas once cost, before any arithmetic?** Every stage that
// works on a photograph pays this, and without it a stage timing cannot be split into "the
// memory" and "the maths". The adjustment stages made the question urgent: `epd_adjust_tone`'s
// LUT path is three PSRAM reads, three PSRAM writes and six internal-RAM lookups per pixel and
// costs 66.7 ms, which is either almost all memory or almost all lookups, and which one decides
// whether raising the CPU clock would help it at all.
//
// Four passes, because fewer would not separate anything:
//
//   read      - 720 KB read, no writes. Read cost on its own.
//   memcpy    - 720 KB read + 720 KB write in the widest transfers newlib will use.
//   bytewise  - the same traffic one byte at a time, between two buffers.
//   inplace   - one byte at a time over a single buffer, which is the shape every pixel stage
//               has and the only one of the four that bounds them.
//
// **`inplace` was added after the first run and the first run is why.** A copy between two
// buffers moves 1.44 MB of cache lines; an in-place pass fetches 720 KB of lines, modifies them
// and writes them back, so it touches half as many distinct lines. `memcpy` is therefore not the
// ceiling for `epd_adjust_tone` -- it is the ceiling for a copy, and the adjust stages do not
// copy. The first run also showed the read-only pass (76.6 ms for 720 KB) coming out *slower*
// than `memcpy` (60.1 ms for 1.44 MB), which says these byte loops are bound by iteration count
// and not by PSRAM bandwidth at all: 17 to 22 cycles a byte at 160 MHz.
//
// **The clock arm then confirmed it from the other side, and this is the useful part.** At
// 240 MHz every pass fell to 0.72-0.83 of its old figure while `memcpy` did not move at all
// (60.11 -> 59.53 ms), so `memcpy` is the one pass here that really is bandwidth-bound and the
// byte loops are not. Current figures, 240 MHz with a 64-byte cache line: read 48.69, memcpy
// 43.17, bytewise 77.32, inplace 54.74 ms. Ten runs agree to 0.01 ms -- if a run of this ever
// disagrees by more than that, something is wrong with the board or the build, not with the
// measurement.
#define FLOOR_RUNS 10

static void measure_memory_floor(void)
{
    const size_t bytes = epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT);
    uint8_t *scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (scratch == NULL) {
        printf("# floor: no PSRAM for a %u byte scratch, skipped\n", (unsigned)bytes);
        return;
    }

    for (int run = 0; run < FLOOR_RUNS; run++) {
        // Refilled every run, not once, because the in-place pass below mutates the canvas and
        // the read pass's checksum is the apparatus check. 240000 * (128 + 64 + 32) = 53760000:
        // a read loop whose result nothing consumes is allowed to vanish, and a vanished loop
        // measures 0 ms and reads as a triumph. Untimed, so its own cost does not matter.
        epd_canvas_fill(&s_canvas, 128, 64, 32);

        const int64_t t0 = esp_timer_get_time();
        uint32_t sum = 0;
        for (size_t i = 0; i < bytes; i++) {
            sum += s_canvas_buf[i];
        }
        const int64_t t1 = esp_timer_get_time();

        memcpy(scratch, s_canvas_buf, bytes);
        const int64_t t2 = esp_timer_get_time();

        for (size_t i = 0; i < bytes; i++) {
            scratch[i] = s_canvas_buf[i];
        }
        const int64_t t3 = esp_timer_get_time();

        // In place, and the increment is what makes it unelidable: the canvas is observable
        // afterwards, so the compiler has to do the stores.
        for (size_t i = 0; i < bytes; i++) {
            s_canvas_buf[i] = (uint8_t)(s_canvas_buf[i] + 1);
        }
        const int64_t t4 = esp_timer_get_time();

        printf("# floor run=%d/%d bytes=%u read_ms=%.2f memcpy_ms=%.2f bytewise_ms=%.2f "
               "inplace_ms=%.2f sum=%u\n",
               run + 1, FLOOR_RUNS, (unsigned)bytes, (double)(t1 - t0) / 1000.0,
               (double)(t2 - t1) / 1000.0, (double)(t3 - t2) / 1000.0,
               (double)(t4 - t3) / 1000.0, (unsigned)sum);
        fflush(stdout);
    }

    heap_caps_free(scratch);
}

static void dither_canvas(dither_mode_t mode, const epd_render_t *render)
{
    const size_t row_bytes = EPD_WIDTH / 2;
    const epd_render_t *cfg = (render != NULL) ? render : &EPD_RENDER_STOCK;
    for (int32_t y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = epd_canvas_row(&s_canvas, y);
        uint8_t *dst = &s_packed[(size_t)y * row_bytes];
        if (mode == DITHER_NEAREST) {
            epd_dither_row_none_cfg(row, dst, EPD_WIDTH, cfg);
        } else {
            epd_dither_row_quality_cfg(row, dst, EPD_WIDTH, (size_t)y,
                                       EPD_DITHER_STRENGTH_QUALITY, cfg);
        }
    }
}

void app_main(void)
{
    printf("\n# M5Paper Color photo pipeline\n");
    printf("# ESP-IDF %s\n", esp_get_idf_version());
    printf("# PSRAM total %u free %u\n",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    trace_init();
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_epd_power(true));

    board_power_t pwr = {0};
    if (board_power_read(&pwr) == ESP_OK) {
        printf("# power: vin=%d (%u mV) bat=%d (%u mV)\n", (int)pwr.vin_present,
               (unsigned)pwr.vin_mv, (int)pwr.bat_present, (unsigned)pwr.vbat_mv);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(epd_init());

    s_canvas_buf = heap_caps_malloc(epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT),
                                    MALLOC_CAP_SPIRAM);
    s_packed = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == NULL || s_packed == NULL) {
        ESP_LOGE(TAG, "canvas/frame allocation failed -- is PSRAM enabled?");
        return;
    }
    if (!epd_canvas_init(&s_canvas, s_canvas_buf,
                         epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT), EPD_WIDTH, EPD_HEIGHT)) {
        ESP_LOGE(TAG, "canvas init refused the buffer");
        return;
    }

    printf("# PSRAM free after canvas: %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Before the panel is touched: what one pass over the canvas costs with no arithmetic in it.
    // Cheap (about a second) and it is the denominator for every stage timing below.
    measure_memory_floor();

    // ~472 KB of PSRAM for the classifier and the white-preservation bits. Taken once here rather
    // than per shot so the allocation is not inside anything being timed, and reported as a
    // number: this is the memory cost src/app/app_display.c pays the first time the setting is on.
    if (!auto_scratch_alloc()) {
        printf("# auto: PSRAM allocation failed; the auto-full shot will be skipped\n");
    }
    printf("# PSRAM free after auto scratch: %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    printf("shot,format,src_w,src_h,decode_ms,dither_ms,xfer_ms,drf_ms,total_ms\n");

    for (size_t i = 0; i < SHOT_COUNT; i++) {
        const shot_t *s = &SHOTS[i];

        epd_canvas_set_rotation(&s_canvas, s->rotation);

        decoded_t dec = {0};
        esp_err_t err = decode_shot(s, &dec);
        if (err != ESP_OK) {
            printf("# %s: decode failed: %s\n", s->label, esp_err_to_name(err));
            continue;
        }
        const int32_t sw = dec.width;
        const int32_t sh = dec.height;
        const epd_image_format_t fmt = dec.format;

        if (s->adjust != NULL || s->adjust_ref != NULL) {
            time_adjust_stages(s, s->adjust != NULL ? ADJUST_RUNS : ADJUST_REF_RUNS);
        }

        // The auto arm decides its own quantiser and its own tone compression, so the render cfg
        // is built here rather than taken from the table -- exactly as src/app/app_display.c does it.
        dither_mode_t mode = s->mode;
        epd_render_t auto_render = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_FULL};
        const epd_render_t *render = s->render;

        // A diffusion arm decides its own pack, for the reason spelled out at the arms: the canvas
        // already holds palette colours, so nearest is exact and tone compression would corrupt it.
        static const epd_render_t DIFFUSE_PACK = {EPD_PALETTE_EPDOPT_AITJCIZE, EPD_TONE_NONE};
        if (s->diffuse != DIFFUSE_OFF) {
            if (!time_diffuse(s, &dec, DIFFUSE_RUNS)) {
                printf("# %s: the diffusion did not run; drawing without it\n", s->label);
            }
            mode = DITHER_NEAREST;
            render = &DIFFUSE_PACK;
        }

        if (s->auto_flow) {
            epd_auto_plan_t plan;
            if (time_auto_flow(s, &dec, &plan, AUTO_RUNS)) {
                mode = plan.nearest ? DITHER_NEAREST : DITHER_QUALITY;
                auto_render.tone = epd_auto_row_tone(&plan, auto_render.tone);
                render = &auto_render;
            } else {
                printf("# %s: the auto flow did not run; drawing without it\n", s->label);
                render = &auto_render;
            }
        }

        const int64_t t1 = esp_timer_get_time();
        dither_canvas(mode, render);
        const int64_t t2 = esp_timer_get_time();

        epd_timings_t t = {0};
        t.run = i + 1;
        err = epd_refresh_frame(s_packed, s->frs, &t);
        if (err != ESP_OK) {
            printf("# %s: refresh failed: %s\n", s->label, esp_err_to_name(err));
            continue;
        }

        printf("%s,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f\n", s->label, (int)fmt, (int)sw,
               (int)sh, (double)dec.decode_ms, (double)(t2 - t1) / 1000.0,
               (double)t.xfer_us / 1000.0, (double)t.drf_us / 1000.0,
               (double)t.total_us / 1000.0);
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
        printf("@@CAPTURE %s\n", s->label);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(DWELL_MS));
    }

    printf("@@DONE\n");
    printf("# photo pipeline complete\n");
    fflush(stdout);
}

#endif // BUILD_PHOTO
