#include "app_display.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_clock.h" // the wall clock for the matte band -- NOT the RTC, see app_clock.h
#include "battery.h"   // the web UI's own charge curve, ported (ticket 64)
#include "app_heapwatch.h"
#include "app_settings.h"
#include "app_smb_sync.h" // SMB_META_SUFFIX and its path: the import-time metadata sidecar (ticket 64)
#include "board.h"
#include "board_led.h"
#include "board_storage.h"
#include "qrcode.h"
#include "epd_adjust.h"
#include "epd_auto.h"
#include "epd_band.h"
#include "epd_canvas.h"
#include "epd_classify.h"
#include "epd_diffuse.h"
#include "epd_dither.h"
#include "epd_flow.h"
#include "epd_panel.h"
#include "epd_format.h"
#include "epd_image.h"
#include "epd_maint_course.h"
#include "epd_text.h"

static const char *TAG = "display";

#define REQUEST_PATH_MAX 128

typedef enum {
    REQ_NONE = 0,
    REQ_IMAGE,
    REQ_BLANK,
    REQ_CARD,
    // Ticket 68's maintenance course: one full-screen flat in a native panel colour.
    REQ_FLAT,
} request_kind_t;

static uint8_t *s_canvas_buf;
static uint8_t *s_packed;
static epd_canvas_t s_canvas;

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_wake;

static request_kind_t s_pending_kind;
static char s_pending_path[REQUEST_PATH_MAX];
// The connect card. Its own buffer rather than s_pending_path, because the request carries eight
// strings and none of them is a path.
//
// **s_card is the IN-FLIGHT copy, and it is a static rather than a stack variable on purpose.**
// display_task() takes a copy under the lock so the render does not hold it for 15 s, which is
// what the path buffer does too -- but this struct is ~480 bytes and the task's stack is 8192
// with 1.5 KB of measured spare over a PNG decode (see app_display_init()). Internal RAM is the
// scarce resource here, and .bss is cheaper to reason about than a deeper stack. Safe because the
// display task is the only reader and its requests are serialised by construction -- the same
// argument s_qr_box below already rests on.
static app_display_card_t s_pending_card;
static app_display_card_t s_card;
// The flat's colour index, validated at the request rather than here (ticket 68).
static uint8_t s_pending_flat;
static bool s_busy;
static char s_current[64];
static char s_current_path[REQUEST_PATH_MAX];
static char s_last_error[48];
static uint32_t s_renders;
static uint32_t s_failures;
static float s_last_total_ms;
static uint8_t s_rotation;
static uint8_t s_palette;
static bool s_auto_adjust;
static bool s_dither_diffuse;
static bool s_auto_rotate;

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static void copy_str(char *dst, const char *src, size_t size)
{
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// FR-3.3: the upload's algorithm choice travels in the filename. "imageN..." was
// uploaded as nearest; everything else, including the four photographs the factory
// firmware left behind, renders dithered.
static bool name_wants_nearest(const char *path)
{
    const char *name = basename_of(path);
    return strncmp(name, "imageN", 6) == 0;
}

// The palette, read once. It is a live setting, and it now reaches three stages of one render --
// the range compressor's endpoints, the tone decision below, and the quantiser -- so a change
// landing between them would compress into one palette's range and then match against another's.
// Reading it here and passing the config down is what makes that impossible.
static epd_render_t current_render(void)
{
    lock();
    const uint8_t palette = s_palette;
    unlock();
    return epd_render_for_palette((epd_palette_id_t)palette);
}

// Colour reduction: M5GFX's row-wise paths, against whichever palette the user has chosen
// (epd_dither.h, app_settings.h). The default is epdoptimize's aitjcize calibration.
//
// **The palette is worth changing; the algorithm is not, and that was measured.** The full
// epdoptimize pipeline is ported and verified in src/epd_epdopt.c, and on this board it costs
// 24.0 s per photograph against this path's 1.62 s -- longer than a refresh, and it starves
// CPU 0 for the duration, so the watchdog fires and httpd stops answering. It stays for
// tools/render_preview.py and the parity test. docs/agents/measurements.md has both sets of
// numbers and the account of a wrong turn taken on the way here.
//
// Those two figures are from 160 MHz with a 32-byte data cache line. Since 2026-09-05 this path
// renders in **1084.0 ms** on this env; the ratio the argument rests on is unchanged. What *did*
// change is that the pipeline's first two stages now have a device-side single-precision
// implementation in src/epd_adjust.c, and auto_adjust_canvas() below runs them and three more
// before this. So "the algorithm is not worth changing" is still true of the *port* and no longer
// true of the arithmetic it does. With the auto flow on, a photograph costs decode 264 + auto
// 1 917 + this path 984 = 3 165 ms against a 15 015 ms refresh (measured 2026-09-05), which is
// 21 % of a refresh where it was 8 %. The display task therefore holds CPU 0 for about 3.2 s at a
// stretch, inside the 5 s task watchdog but no longer far inside it.
//
// `cfg.tone` arrives already decided by epd_auto_row_tone() when the auto flow ran, because that
// decision belongs to the plan rather than to the palette -- see epd_auto.h.
static void dither_canvas(bool nearest, const epd_render_t *cfg)
{
    const int64_t started = esp_timer_get_time();
    const size_t row_bytes = EPD_WIDTH / 2;

    for (int32_t y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = epd_canvas_row(&s_canvas, y);
        uint8_t *dst = &s_packed[(size_t)y * row_bytes];
        if (nearest) {
            epd_dither_row_none_cfg(row, dst, EPD_WIDTH, cfg);
        } else {
            epd_dither_row_quality_cfg(row, dst, EPD_WIDTH, (size_t)y,
                                       EPD_DITHER_STRENGTH_QUALITY, cfg);
        }
    }

    printf("# render: %s tone=%u in %.1f ms\n", nearest ? "nearest" : "dither",
           (unsigned)cfg->tone, (double)(esp_timer_get_time() - started) / 1000.0);
}

// ------------------------------------------------------- epdoptimize's error diffusion
//
// The other half of what the demo site does, and independent of the auto flow above: this is the
// *quantiser*. M5GFX's row path searches thirty-six pairs per two pixels with an ordered chromatic
// bias and carries no error between rows; this is Floyd-Steinberg with a serpentine scan, which is
// what <https://paperlesspaper.github.io/epdoptimize> quantises with by default.
//
// **It costs less than the path it replaces**, which is not what anyone expected: 602.7 ms for the
// diffusion plus 217.6 ms for the pack, against the row path's 1076.3 ms at the same tone setting
// (ten runs each, docs/agents/measurements.md). The pack is cheap for a structural reason -- after
// diffusion every pixel already *is* a palette entry, so nearest searches six candidates instead of
// thirty-six, and it is exact rather than approximate.
//
// Two things this gets right that are easy to get wrong, both of them measured or tested rather
// than reasoned:
//
//   * **The region, not the canvas.** The matte is 255,255,255 and that is not a palette entry, so
//     diffusing the whole canvas would push the matte's own quantisation error into the picture's
//     edge. test/test_diffuse asserts the surround is untouched at both rotations.
//   * **Logical order, not physical.** The diffusion has to walk the picture as it was drawn or its
//     direction would turn with the `orientation` setting. At rotation 1 that strides -1200 bytes
//     per pixel and it costs *nothing* -- 265.8 ms against the sequential walk's 267.2, exactly
//     area-proportional, because a logical row's working set fits in cache.
//
// `epd_diffuse_rect()` can also run in bands, and that is bit-exact (test_diffuse covers it), so a
// yield point is available. It is not used: 602.7 ms is well inside the 5 s task watchdog and less
// than the row path already holds CPU 0 for, so banding here would buy nothing and cost tick
// latency.
static bool diffuse_and_pack(const epd_fit_t *fit, const epd_render_t *cfg, uint8_t pre_tone,
                             bool serpentine)
{
    uint8_t *origin = NULL;
    ptrdiff_t step_x = 0, step_y = 0;
    if (!epd_canvas_logical_walk(&s_canvas, fit->x, fit->y, fit->width, fit->height, &origin,
                                 &step_x, &step_y)) {
        ESP_LOGW(TAG, "diffuse: fit %ld,%ld %ldx%ld is not inside the canvas", (long)fit->x,
                 (long)fit->y, (long)fit->width, (long)fit->height);
        return false;
    }

    // Exactly one range compression, same rule as the row path and applied at the other end of it:
    // whatever epd_adjust_range() did not do, the integer compression owes here, because the pack
    // below cannot do it. See epd_auto.h.
    const int64_t t0 = esp_timer_get_time();
    const epd_render_t pre = {cfg->palette, pre_tone};
    epd_dither_tone_rect(origin, fit->width, fit->height, step_x, step_y, &pre);
    const int64_t t1 = esp_timer_get_time();

    const epd_diffuse_t dcfg = {.palette = cfg->palette, .serpentine = serpentine};
    epd_diffuse_rect(origin, fit->width, fit->height, step_x, step_y, &dcfg, 0, fit->height);
    const int64_t t2 = esp_timer_get_time();

    // EPD_TONE_NONE is not a choice. The canvas now holds exact palette colours, so nearest is
    // lossless -- and tone compression here would move the palette's own white off itself and
    // re-match it, which is silent and ruins the picture.
    const epd_render_t pack = {cfg->palette, EPD_TONE_NONE};
    const size_t row_bytes = EPD_WIDTH / 2;
    for (int32_t y = 0; y < EPD_HEIGHT; y++) {
        epd_dither_row_none_cfg(epd_canvas_row(&s_canvas, y), &s_packed[(size_t)y * row_bytes],
                                EPD_WIDTH, &pack);
    }
    const int64_t t3 = esp_timer_get_time();

    printf("# diffuse region=%ldx%ld step_x=%ld step_y=%ld serp=%d pre_tone=%u "
           "pre_ms=%.1f diffuse_ms=%.1f pack_ms=%.1f\n",
           (long)fit->width, (long)fit->height, (long)step_x, (long)step_y, serpentine ? 1 : 0,
           (unsigned)pre_tone, (double)(t1 - t0) / 1000.0, (double)(t2 - t1) / 1000.0,
           (double)(t3 - t2) / 1000.0);
    return true;
}

// ------------------------------------------------------------------- epdoptimize's auto flow
//
// Classify the photograph, choose its settings from the class, and apply the five per-pixel
// stages before the quantiser. This is what <https://paperlesspaper.github.io/epdoptimize> does
// by default; src/epd_classify.h is the first half and src/epd_auto.h the second.
//
// **Off unless the user turns it on** (app_settings.h): it overrides FR-3.3's filename rule for
// choosing between nearest and dithered.
//
// The scratch is PSRAM, allocated on first use and never freed. Lazily, because 472 KB should not
// be held while the setting is off; never freed, because the alternative is churning half a
// megabyte on a setting toggle. **An allocation failure logs once and falls back to the ordinary
// path** -- internal RAM is what this firmware runs out of, none of this comes out of it, and a
// render that cannot classify is still a render.

static epd_classify_scratch_t s_auto_scratch;
static uint8_t *s_white_bits;
static bool s_auto_scratch_failed;

static bool auto_scratch_ready(void)
{
    if (s_auto_scratch.samples != NULL) {
        return true;
    }
    if (s_auto_scratch_failed) {
        return false;
    }

    const size_t white_bytes = epd_adjust_white_bits_bytes(EPD_WIDTH, EPD_HEIGHT);
    epd_classify_scratch_t s = {0};
    s.samples = heap_caps_malloc(epd_classify_samples_bytes(), MALLOC_CAP_SPIRAM);
    s.sample_capacity = epd_classify_samples_bytes() / sizeof(epd_classify_sample_t);
    s.colour_counts = heap_caps_malloc(epd_classify_colour_counts_bytes(), MALLOC_CAP_SPIRAM);
    s.tile_stamps = heap_caps_malloc(epd_classify_tile_stamps_bytes(), MALLOC_CAP_SPIRAM);
    s.luma_bins = heap_caps_malloc(epd_classify_luma_bins_bytes(), MALLOC_CAP_SPIRAM);
    uint8_t *bits = heap_caps_malloc(white_bytes, MALLOC_CAP_SPIRAM);

    if (!s.samples || !s.colour_counts || !s.tile_stamps || !s.luma_bins || !bits) {
        free(s.samples);
        free(s.colour_counts);
        free(s.tile_stamps);
        free(s.luma_bins);
        free(bits);
        s_auto_scratch_failed = true;
        ESP_LOGE(TAG, "auto adjust needs %u B of PSRAM and could not get it; "
                      "rendering without it",
                 (unsigned)(epd_classify_samples_bytes() + epd_classify_colour_counts_bytes() +
                            epd_classify_tile_stamps_bytes() + epd_classify_luma_bins_bytes() +
                            white_bytes));
        return false;
    }

    s_auto_scratch = s;
    s_white_bits = bits;
    return true;
}

// Runs the five stages over the rectangle the decoder drew into, and reports what it decided.
// False means the ordinary path should be taken: the canvas is untouched in that case.
//
// **The region matters and is not tidiness.** The canvas is matted white around the picture, and
// two of these stages measure statistics over the pixels they are given -- EPD_RANGE_MODE_AUTO's
// percentiles and white preservation's p99. Given the matte, `auto` degenerates into `display`
// and the white plan latches onto the matte instead of onto the paper in the photograph.
static bool auto_adjust_canvas(const epd_fit_t *fit, const epd_palette_entry_t *palette,
                              epd_auto_plan_t *plan)
{
    if (!auto_scratch_ready()) {
        return false;
    }

    int32_t px = 0, py = 0, pw = 0, ph = 0;
    if (!epd_canvas_physical_rect(&s_canvas, fit->x, fit->y, fit->width, fit->height, &px, &py,
                                  &pw, &ph)) {
        ESP_LOGW(TAG, "auto: fit %ld,%ld %ldx%ld is not inside the canvas", (long)fit->x,
                 (long)fit->y, (long)fit->width, (long)fit->height);
        return false;
    }

    const int64_t t_start = esp_timer_get_time();
    epd_classification_t c;
    if (!epd_classify_canvas(&s_canvas, fit->x, fit->y, fit->width, fit->height, &s_auto_scratch,
                             &c)) {
        ESP_LOGW(TAG, "auto: the classifier refused the region");
        return false;
    }
    const int64_t t_classified = esp_timer_get_time();

    epd_auto_suggest(&c, palette, plan);

    // The order is upstream's and it is load-bearing: the white plan is measured on the untouched
    // source, level compression lands *after* range compression rather than fused into the tone LUT,
    // and the white apply is last of all. epd_adjust.h has the citations -- and since 2026-09-21
    // epd_flow.h owns the sequence, so this site, photo_main.c's timing arm and
    // tools/render_preview.c are three callers of one order rather than three copies of it.
    // test/test_flow pins it; the durations come back in `t` so this log line is unchanged.
    const epd_flow_region_t region = {
        .rgb = &s_canvas.rgb[((size_t)py * (size_t)s_canvas.width + (size_t)px) * 3u],
        .width = (size_t)pw,
        .height = (size_t)ph,
        .stride = (size_t)s_canvas.width * 3u,
        .palette = palette,
        .white_bits = s_white_bits,
        .white_bits_bytes = epd_adjust_white_bits_bytes((size_t)pw, (size_t)ph),
    };
    epd_white_plan_t white;
    epd_flow_timing_t t;
    epd_flow_apply(&region, plan, &white, esp_timer_get_time, &t);

    // Raw values and raw milliseconds, two lines, no verdict. A boolean computed here would be a
    // second thing that can be wrong, and on this board finding that out costs a rebuild, a flash
    // and a 15 s refresh.
    printf("# auto kind=%s region=%ldx%ld tone=%d exp=%.3f sat=%.3f con=%.3f str=%.3f "
           "sb=%.3f hc=%.3f mid=%.3f range=%d rs=%.3f lo=%.3f hi=%.3f\n",
           epd_image_kind_name(plan->kind), (long)pw, (long)ph, (int)plan->adjust.tone_mode,
           (double)plan->adjust.exposure, (double)plan->adjust.saturation,
           (double)plan->adjust.contrast, (double)plan->adjust.strength,
           (double)plan->adjust.shadow_boost, (double)plan->adjust.highlight_compress,
           (double)plan->adjust.midpoint, (int)plan->adjust.range_mode,
           (double)plan->adjust.range_strength, (double)plan->adjust.low_percentile,
           (double)plan->adjust.high_percentile);
    // **`plan_ms` NARROWED on 2026-09-21 and captures either side of that are not quite the same
    // field.** It used to be `t_plan - t_classified`, which also contained epd_auto_suggest() and the
    // region arithmetic because the bracket opened before them; it is now the white plan alone, which
    // is what the name says.
    //
    // **The narrowing is BELOW THE NOISE and was measured, not assumed.** Same board, same
    // photograph (`wpa-index-600.png`, the one fixture that turns on five of the six stages), either
    // side of the commit: `plan_ms` 147.6 -> 149.7, i.e. it moved UP by 2.1 ms where the bracket
    // predicted a small drop. The scale that settles it is `classify_ms`, which this change does not
    // touch at all and which moved 172.2 -> 175.1 in the same pair. So run-to-run variation here is
    // a few ms and epd_auto_suggest() is far under that. **Do not read a small move in this field as
    // evidence of anything, in either direction** -- and do not use this pair to claim the narrowing
    // happened either; the arithmetic says it did and the instrument cannot see it.
    printf("# auto nearest=%d lab=%d stucki=%d paper=%d level=%d(%.0f,%.0f) white=%d(%.1f) "
           "classify_ms=%.1f plan_ms=%.1f paper_ms=%.1f tone_ms=%.1f range_ms=%.1f "
           "level_ms=%.1f white_ms=%.1f\n",
           plan->nearest ? 1 : 0, plan->wanted_lab ? 1 : 0, plan->wanted_stucki ? 1 : 0,
           plan->paper.enabled ? 1 : 0, plan->level.enabled ? 1 : 0, (double)plan->level.black,
           (double)plan->level.white, white.active ? 1 : 0, (double)white.source_white_luma,
           (double)(t_classified - t_start) / 1000.0, (double)t.white_plan_us / 1000.0,
           (double)t.paper_us / 1000.0, (double)t.tone_us / 1000.0,
           (double)t.range_us / 1000.0, (double)t.level_us / 1000.0,
           (double)t.white_apply_us / 1000.0);
    return true;
}

// Decode happens with the storage lock held and the refresh happens without it. That
// order is not a detail: board_storage_lock() takes the SPI lock underneath, and
// epd_refresh_frame() takes the SPI lock itself, so refreshing while holding it asserts
// on a self-deadlock (board_spi.h, and the lock order fixed in ticket 03).
// Everything the band draws, in one struct of static buffers. Static and not locals because this
// task's stack has caused a reboot loop here once, and the display task is the single caller with
// its requests serialised by construction -- the same argument the connect card's QR box makes.
typedef struct {
    epd_band_content_t content;
    epd_band_layout_t layout;
    char city[40];
    char country[24];
    char taken[12];
    char ago[20];
    char now[8];
    char climate[24];
    int year, month, day;
    // True when the photograph's layer is FRAME_BAND_DUMMY_META's invention rather than the file's.
    bool dummy;
} band_state_t;

static band_state_t s_band;

// Reads `.thumbs/<name>.mta` for the photograph at `path`, the metadata sidecar the import wrote (app_smb_sync.c's write_meta). A
// missing one is the ORDINARY case, not a failure: everything imported before ticket 64 has none,
// an upload never had EXIF to lose (the browser's canvas re-encoded it away before the frame saw
// the file), and a source that carried none produces none.
//
// **Called with the storage lock already held**, inside render_image()'s locked section, because it
// is one more FATFS open on the same volume the panel shares an SPI bus with.
static void read_meta(const char *path)
{
    s_band.city[0] = '\0';
    s_band.country[0] = '\0';
    s_band.year = 0;
    s_band.month = 0;
    s_band.day = 0;
    s_band.dummy = false;

    char mpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(mpath, sizeof(mpath), path, SMB_META_SUFFIX);
    FILE *f = fopen(mpath, "rb");
    // **No early return here.** There used to be one, and it made the dummy-metadata block below
    // unreachable in the only case it exists for -- a photograph with NO sidecar, which on this bench
    // is every photograph. The whole eight-sample arm of 2026-09-19 ran with `dummy=0` because of it.
    if (f != NULL) {
        char body[96];
        const size_t n = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        body[n] = '\0';

        // Walked rather than sscanf'd: a city line contains spaces and `%s` would stop at the first
        // one, leaving `TOKYO` where `TOKYO JAPAN` was written.
        char *line = body;
        while (line != NULL && *line != '\0') {
            char *end = strchr(line, '\n');
            if (end != NULL) {
                *end = '\0';
            }
            if (line[0] != '\0' && line[1] == ' ') {
                const char *v = line + 2;
                if (line[0] == 'C') {
                    snprintf(s_band.city, sizeof(s_band.city), "%s", v);
                } else if (line[0] == 'N') {
                    // A separate line since 2026-09-19, because the band's one line drops the
                    // country first and keeps the city. app_smb_sync.c's write_meta() says why
                    // recovering them from a joined string is not an option.
                    snprintf(s_band.country, sizeof(s_band.country), "%s", v);
                } else if (line[0] == 'D') {
                    int y = 0, mo = 0, d = 0;
                    if (sscanf(v, "%d-%d-%d", &y, &mo, &d) == 3 && y > 1825 && mo >= 1 &&
                        mo <= 12) {
                        s_band.year = y;
                        s_band.month = mo;
                        s_band.day = d;
                    }
                }
            }
            line = (end != NULL) ? end + 1 : NULL;
        }
    }

#ifdef FRAME_BAND_DUMMY_META
    // **Bench only, and it must never be in `platformio.ini`** -- the same standing as
    // `-DFRAME_PAIR_CODE_LOG`, for the same reason: it puts something on the glass that is not true.
    //
    // It exists because the metadata sidecar has never been written on hardware. An upload has no
    // EXIF (the browser's canvas re-encoded it away before the frame saw the file) and the mirror
    // catalogues 0 of 1,036 files (ticket 27), so the band's PHOTOGRAPH layer -- the whole reason the
    // line composition has a ladder in it -- has only ever existed in host tests. This fills it so the
    // layout can be looked at on the panel.
    //
    // Fixed values, not derived from the file: a varying caption would make two renders differ for a
    // reason that is not the thing under test. `dummy=1` on the band's console line is what stops a
    // future reader seeing `TOKYO JAPAN` in a log and concluding the geocoder worked.
    if (s_band.city[0] == '\0' && s_band.year == 0) {
        // The city is overridable because the band's CUT cannot be seen with a five-letter name
        // (operator, 2026-09-20): `TOKYO 19-08-14` fits every band this project has, so the arm that
        // verifies truncation needs a long one and nothing on this bench produces one. Still inside
        // the bench block, still one flag away from not existing.
#ifndef FRAME_BAND_DUMMY_CITY
#define FRAME_BAND_DUMMY_CITY "TOKYO"
#endif
        snprintf(s_band.city, sizeof(s_band.city), "%s", FRAME_BAND_DUMMY_CITY);
        snprintf(s_band.country, sizeof(s_band.country), "JAPAN");
        s_band.year = 2019;
        s_band.month = 8;
        s_band.day = 14;
        s_band.dummy = true;
    }
#endif
}

// Composes the content from what the producers say now. read_meta() must have run for this
// photograph first; with nothing from it the layout promotes the room's line, which is its own
// documented path rather than a failure.
static void band_content(void)
{
    epd_band_content_t *c = &s_band.content;
    memset(c, 0, sizeof(*c));

    // **app_clock, not board_rtc_read().** The RX8130's calendar is not the wall clock -- nothing
    // sets it from SNTP -- and taking the date from it printed 02-04 on this band's first hardware
    // run while the frame's own clock read the 19th of September. app_clock.h has the account.
    int now_year = 0, now_month = 0, now_day = 0;
    const bool have_now = app_clock_local_date(&now_year, &now_month, &now_day);
    if (have_now) {
        // Day and month only. An hour would be a clock, and a clock on a panel that takes
        // 15,014.6 ms to redraw is the one thing ticket 64 rules out.
        snprintf(s_band.now, sizeof(s_band.now), "%02d-%02d", now_month, now_day);
        c->now = s_band.now;
    }

    float temp_c = 0.0f;
    float humidity = 0.0f;
    if (board_sht40_read(&temp_c, &humidity) == ESP_OK) {
        // `25`-degree-`C 51%` (operator, 2026-09-19). It used to read `25.0C RH51` because
        // NEITHER glyph existed in the 5x7 set and a byte with no glyph draws a hollow box; both
        // were added the same day. EPD_TEXT_DEGREE rather than a literal degree sign, because a
        // `\u00b0` typed into a UTF-8 source file is TWO bytes and the first of them has no glyph.
        //
        // And the temperature loses its decimal place. That buys three characters -- `25.0C RH51`
        // is ten and `25`+deg+`C 51%` is eight -- on a line where three characters decide whether
        // the country appears at all, in exchange for a tenth of a degree of a room reading that
        // nobody acts on.
        snprintf(s_band.climate, sizeof(s_band.climate), "%d" EPD_TEXT_DEGREE "C %d%%",
                 (int)(temp_c + 0.5f), (int)(humidity + 0.5f));
        c->climate = s_band.climate;
    }

    // **An icon, not a voltage** (operator, 2026-09-19): the band shows charge the way a phone does.
    // `vbat_mv == 0` is a board that cannot read its cell, which is not the same as an empty one, so
    // it stays negative and no icon is drawn. The curve is the web UI's own
    // (`battery_percent_from_mv()`, ported and parity-checked) so the page and the glass cannot
    // disagree about the same battery.
    c->battery_pct = -1;
    board_power_t power;
    if (board_power_read(&power) == ESP_OK && power.vbat_mv > 0) {
        c->battery_pct = battery_percent_from_mv(power.vbat_mv);
    }

    if (s_band.city[0] != '\0') {
        c->city = s_band.city;
    }
    if (s_band.country[0] != '\0') {
        c->country = s_band.country;
    }
    if (s_band.year > 0) {
        // Two digits here and four in the file: the band holds sixteen characters and a city plus
        // a four-digit date is over it for any name longer than five letters.
        snprintf(s_band.taken, sizeof(s_band.taken), "%02d-%02d-%02d", s_band.year % 100,
                 s_band.month, s_band.day);
        c->taken = s_band.taken;

        if (have_now) {
            const int years = now_year - s_band.year;
            if (years == 1) {
                snprintf(s_band.ago, sizeof(s_band.ago), "1 YEAR AGO");
                c->ago = s_band.ago;
            } else if (years > 1 && years < 200) {
                snprintf(s_band.ago, sizeof(s_band.ago), "%d YEARS AGO", years);
                c->ago = s_band.ago;
            }
        }
    }
}

static esp_err_t render_image(const char *path, float *total_ms)
{
    board_storage_lock();
    board_storage_prepare_access();

    epd_image_reader_storage_t rstore;
    epd_image_reader_t *reader = NULL;
    // The rectangle the decoder drew into. It used to be recomputed below from `src_w`/`src_h`
    // and hoped to agree with the one epd_image_draw() had used; since ticket 64 there is one
    // rectangle and it is PASSED to the draw, because the band's alignment means the two can no
    // longer be derived independently.
    int32_t src_w = 0;
    int32_t src_h = 0;
    epd_fit_t fit = {0.0f, 0, 0, 0, 0};
    epd_band_t band = {EPD_BAND_NONE, 0, 0, 0, 0, true};

    lock();
    const bool want_rotate = s_auto_rotate;
    const uint8_t base_rotation = s_rotation;
    unlock();

    // _display rather than epd_image_open_file(): the JPEG decode scale is chosen at open,
    // against the fit, and the fit is the SWAPPED one for a picture this is about to turn.
    // The reader applies the same epd_fit_wants_rotate() to the same dimensions.
    //
    // **`want_rotate` is passed rather than choosing between two entry points, and it is not
    // cosmetic.** With `auto_rotate` off the canvas is not turned, so a reader that still
    // chose its scale against the turned fit would decode one power of two LARGER than the
    // draw needs -- the same defect as the missing hint, in the direction that costs PSRAM
    // instead of sharpness. It used to be a ternary over two functions, and the second thing
    // this entry point carries is exactly why it no longer is: a JPEG too large to allocate at
    // the chosen scale is decoded one power of two SMALLER here rather than refused (ticket
    // 56), and a flag hung on the rotatable variant alone would have missed every draw with
    // `auto_rotate` off. epd_image.h has the trade; the observable is a `step down` line.
    esp_err_t err = epd_image_open_file_display(&rstore, path, want_rotate, &reader);
    bool rotated = false;
    if (err == ESP_OK) {
        src_w = epd_image_width(reader);
        src_h = epd_image_height(reader);

        // Turn the CANVAS, not the pixels. epd_image_draw() takes its fit from the
        // canvas's logical dimensions, so this is the whole of the feature on the display
        // side: the matte, the region the auto flow classifies, and the diffusion's walk
        // all follow it with no further change. And the canvas's own rotation is re-set
        // from s_rotation at the top of every request in display_task(), so a per-image
        // override cannot leak into the next photograph.
        rotated = want_rotate &&
                  epd_fit_wants_rotate(src_w, src_h, epd_canvas_logical_width(&s_canvas),
                                       epd_canvas_logical_height(&s_canvas));
        if (rotated) {
            // **`^ 1u` AND NOT `(base + 1) % 4`, and this was already correct for four
            // directions before four directions existed** (ticket 69 §3c). XOR with 1 maps
            // 0<->1 and 2<->3, and because rotation 2 has 0's logical shape and 3 has 1's, that
            // is exactly a quarter turn to the OTHER shape from whichever base the frame is set
            // to -- so a frame hung upside down turns its landscape photographs the way that
            // base implies, with no new code and no new decision.
            //
            // `(base + 1) % 4` would turn 1 into 2: the same logical shape, i.e. a 180-degree
            // flip of the picture and no turn at all. Do not "generalise" it.
            epd_canvas_set_rotation(&s_canvas, base_rotation ^ 1u);
        }

        // **The fit and the band together, and the rule is the bottom of the GLASS** (ticket 64,
        // operator 2026-09-19 after looking at five samples). The photograph is never made smaller
        // for the band's sake -- `epd_band_plan()` returns `epd_fit_centre()`'s own width and height
        // in every case -- and where the leftover cannot lie along the bottom of the glass there is
        // no band and the fit stays centred. That is the "photograph only" case.
        //
        // Computed in one call on purpose: doing the fit and the band separately is what put the
        // band down the panel's right edge for a 9:16 source and across its top for a turned 4:3
        // one, which is what the operator rejected.
        epd_band_plan(&s_canvas, src_w, src_h, &fit, &band);

        err = epd_image_draw_fit(reader, &s_canvas, &fit);
        epd_image_close(reader);

        // Inside the same locked section: one more open on the volume the panel shares a bus with.
        if (band.kind != EPD_BAND_NONE) {
            read_meta(path);
        }
    }
    board_storage_unlock();

    // Raw values, no verdict: the source's own size and the rotation actually used. A
    // boolean saying "rotated" would be a second thing that can be wrong, and finding that
    // out costs a rebuild, a flash and a 15 s refresh.
    printf("# draw src=%ldx%ld base_rot=%u rot=%u auto_rotate=%d\n", (long)src_w, (long)src_h,
           (unsigned)base_rotation, (unsigned)s_canvas.rotation, want_rotate ? 1 : 0);
    // Raw, and the verdict computed at analysis time: the band's LOGICAL rect and the fit it came
    // from. `kind=0` with a centred fit is the "photograph only" case and is the shape to look for
    // when a source's leftover cannot lie along the bottom of the glass. A derived "band at the
    // bottom" boolean would be a second thing that can be wrong, and finding that out costs a
    // rebuild, a flash and a 15 s refresh.
    printf("# band kind=%d x=%ld y=%ld w=%ld h=%ld low=%d scale=%ld fit=%ld,%ld %ldx%ld\n",
           (int)band.kind, (long)band.x, (long)band.y, (long)band.width, (long)band.height,
           band.photo_at_low ? 1 : 0, (long)epd_band_large_scale(&band), (long)fit.x, (long)fit.y,
           (long)fit.width, (long)fit.height);

    if (err != ESP_OK) {
        return err;
    }

    epd_render_t cfg = current_render();
    bool nearest = name_wants_nearest(path);

    lock();
    const bool want_auto = s_auto_adjust;
    const bool want_diffuse = s_dither_diffuse;
    unlock();

    // With no auto flow there is no plan, so the two things a plan would have decided come from
    // elsewhere: the tone compression is the palette's own, and serpentine is on -- the demo's
    // layered auto sets it for every errorDiffusion arm, and this setting exists to reach what the
    // demo does.
    uint8_t pre_tone = cfg.tone;
    bool serpentine = true;

    if (want_auto) {
        epd_auto_plan_t plan;
        if (auto_adjust_canvas(&fit, cfg.palette, &plan)) {
            // FR-3.3's filename rule is overridden here, which is why the setting defaults off.
            nearest = plan.nearest;
            cfg.tone = epd_auto_row_tone(&plan, cfg.tone);
            pre_tone = cfg.tone;
            serpentine = plan.serpentine;
        }
    }

    // **The band goes on AFTER the auto flow and BEFORE the quantise.** After, because the auto
    // flow's statistics are measured over the region and must not see this ink -- the white
    // preservation plan latching onto a band's black would move the whole tone curve. Before,
    // because both quantise paths walk the full canvas, so pure black on the surround comes back
    // as the palette's own black exactly.
    //
    // One trap, and it is the row path's: `diffuse_and_pack()` touches only the region, so the
    // band reaches the pack untouched and matches exactly. `dither_canvas()` quantises every row
    // including this one, so with `dither_diffuse` off the text can pick up the pair search's
    // dither. The default is on; this is a difference to look for on glass, not a defect.
    if (band.kind != EPD_BAND_NONE) {
        band_content();
        epd_band_layout(&band, &s_band.content, &s_band.layout);
        epd_band_draw(&s_canvas, &band, &s_band.layout);
        // `segs=`, not `lines=`: the band has been ONE line since 2026-09-19 and the count is how
        // many fields sit along it. Every segment's text, because which ones survived the length is
        // the whole question now -- `country=1` with `TOKYO` in segment 0 says the splice was
        // refused, and one line printing only the first segment could not tell you that.
        printf("# band segs=%ld scale=%ld bat=%d dummy=%d country=%d meta=%d,%d\n",
               (long)s_band.layout.count, (long)s_band.layout.scale, s_band.content.battery_pct,
               s_band.dummy ? 1 : 0, s_band.country[0] != '\0' ? 1 : 0,
               s_band.city[0] != '\0' ? 1 : 0, s_band.year);
        for (int32_t i = 0; i < s_band.layout.count; i++) {
            printf("# band seg%ld %s \"%s\"\n", (long)i,
                   s_band.layout.segments[i].kind == EPD_BAND_SEG_BATTERY ? "icon" : "text",
                   s_band.layout.segments[i].text);
        }
    }

    // Nearest is nearest on either quantiser: `quantizationOnly` is upstream's own name for it and
    // FR-3.3's "Nearest" is the same thing, so a diffusion setting does not override that choice.
    if (want_diffuse && !nearest && diffuse_and_pack(&fit, &cfg, pre_tone, serpentine)) {
        // Packed already, by the diffusion path's own exact nearest pass.
    } else {
        dither_canvas(nearest, &cfg);
    }

    epd_timings_t t = {0};
    t.run = 1;
    err = epd_refresh_frame(s_packed, EPD_FRS_STOCK, &t);
    *total_ms = (float)t.total_us / 1000.0f;
    return err;
}

// One full-screen flat in a native panel colour -- the maintenance course, ticket 68.
//
// **It does not touch s_canvas at all**, and that is the whole point: the operator asked for a mode
// that ignores the palette ("パレット等を無視して"), so the colour index goes straight into the
// packed frame and no stage that could reinterpret it ever runs. No epd_canvas, no epd_dither, no
// epd_adjust, no epd_epdopt, no auto flow -- a white flat here is EPD_COLOR_WHITE, not whatever the
// current palette's nearest match to (255,255,255) happens to be.
//
// epd_pack_solid() refuses an index this panel cannot render (index 4 is orange and is on neither
// board), so a bad colour comes back as a zero length rather than as a frame of something else.
static esp_err_t render_flat(uint8_t colour, float *total_ms)
{
    if (epd_pack_solid(s_packed, EPD_FRAME_BYTES, colour) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    epd_timings_t t = {0};
    t.run = 1;
    const esp_err_t err = epd_refresh_frame(s_packed, EPD_FRS_STOCK, &t);
    *total_ms = (float)t.total_us / 1000.0f;
    return err;
}

static esp_err_t render_blank(float *total_ms)
{
    epd_canvas_fill(&s_canvas, 255, 255, 255);
    const epd_render_t cfg = current_render();
    dither_canvas(true, &cfg);

    epd_timings_t t = {0};
    t.run = 1;
    const esp_err_t err = epd_refresh_frame(s_packed, EPD_FRS_STOCK, &t);
    *total_ms = (float)t.total_us / 1000.0f;
    return err;
}

// ------------------------------------------------------------------ the connect card
//
// FR-6.3, tickets 30 and 66. On white: the WIFI: URI that joins the access point, the pairing URL
// that carries the API token of ticket 29, and since ticket 66 the same three things in words --
// because a PC has no camera and could not pair at all.
//
// esp_qrcode_generate() hands the module grid back through a callback that takes no user
// pointer, so the target box is file-static. That is safe here and only here: the display
// task is the single caller and its requests are serialised by construction.
//
// The scale is computed inside the callback rather than guessed outside it, because the
// version -- and so the module count -- depends on the text, and the text depends on how
// long the SSID and the token are. A hardcoded "scale 4" would silently overflow the box
// the day somebody lengthens the URL.

static struct {
    int32_t x, y, w, h;
} s_qr_box;

// Four modules on every side, which is what the QR specification requires for a reader
// to find the symbol at all. It comes out of the box, not off the edge of the panel.
#define QR_QUIET_MODULES 4

static void qr_draw_cb(esp_qrcode_handle_t qr)
{
    const int size = esp_qrcode_get_size(qr);
    if (size <= 0) {
        return;
    }
    const int32_t total = (int32_t)size + 2 * QR_QUIET_MODULES;
    int32_t scale = (s_qr_box.w < s_qr_box.h ? s_qr_box.w : s_qr_box.h) / total;
    if (scale < 1) {
        scale = 1;  // draw it anyway; the scan is what says whether it is readable
    }

    const int32_t drawn = total * scale;
    const int32_t ox = s_qr_box.x + (s_qr_box.w - drawn) / 2 + QR_QUIET_MODULES * scale;
    const int32_t oy = s_qr_box.y + (s_qr_box.h - drawn) / 2 + QR_QUIET_MODULES * scale;

    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            if (esp_qrcode_get_module(qr, mx, my)) {
                epd_canvas_fill_rect(&s_canvas, ox + mx * scale, oy + my * scale, scale,
                                     scale, 0, 0, 0);
            }
        }
    }
    printf("# qr modules=%d scale=%ld origin=%ld,%ld\n", size, (long)scale, (long)ox, (long)oy);
}

static esp_err_t draw_qr(const char *text, int32_t x, int32_t y, int32_t w, int32_t h)
{
    // **esp_qrcode_generate() PRINTS ITS INPUT at ESP_LOG_INFO** -- "Encoding below text" and then
    // the whole payload. Measured 2026-09-18 on the first run of this card: the access point's
    // password and the API token both went into .scratch/captures/, which is not git-ignored, while
    // frame_main.c was carefully logging four characters and a length of each. A redaction that only
    // covers this project's own printf is not a redaction. WARN, not NONE, so a generator that
    // actually fails still says so.
    esp_log_level_set("QRCODE", ESP_LOG_WARN);

    s_qr_box.x = x;
    s_qr_box.y = y;
    s_qr_box.w = w;
    s_qr_box.h = h;

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_draw_cb;
    cfg.max_qrcode_version = 10;
    // Low ECC, as ticket 16 specified: this is a clean flat surface read from close up,
    // not a label on a box, and a lower level means fewer modules and so a larger module
    // for a given panel -- which is the thing that actually decides readability here.
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    return esp_qrcode_generate(&cfg, text);
}

// ---------------------------------------------------------------- the words beside them
//
// Ticket 66. The two codes used to be indistinguishable to a user -- `epd_canvas` had no font, so
// the screen had no text at all, and on 2026-09-09 the operator did not know which code joined the
// network and which opened the page and tried them in turn. That was answered with a COUNT of
// filled squares beside each, one and two, which said the order and nothing else.
//
// It is answered with words now, because the squares could not carry the thing that was actually
// missing: **a PC has no camera**, so the access point's password, the frame's address and the
// pairing code all had to become readable or a desktop browser could not pair at all. The numerals
// 1, 2 and 3 in the text do the job the squares were invented for, so they are gone.

#define CARD_MARGIN 8
#define CARD_GAP 8
// Leading between lines, in units of the glyph scale -- so a scale-3 line is followed by 9 px of
// space and a scale-2 line by 6. Proportional rather than fixed, or the small lines look grouped
// with the large ones.
#define CARD_LEADING 3
#define CARD_LINES_MAX 5

typedef struct {
    int32_t scale;
    const char *text;
} card_line_t;

// Where the composed strings live. On the display task's stack, which is where the two 160-byte QR
// payload copies used to be -- see s_card for why that trade was made deliberately.
typedef struct {
    char join_label[16];
    char ssid[40];
    char pass[40];
    char open_label[16];
    char host[72];
    char alt[40];
    char code[32];
} card_text_t;

// Upper-cases into `dst`. The card is uppercase because DNS names and URL schemes are
// case-insensitive and capitals survive the dither better -- **except the access point's password,
// which must never come through here**: it is lowercase hex and WPA2 derives its key from the
// passphrase bytes, so an uppercased one simply will not associate.
static void upper_copy(char *dst, size_t size, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++) {
        const char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = '\0';
}

static int32_t lines_height(const card_line_t *lines, int n)
{
    int32_t h = 0;
    for (int i = 0; i < n; i++) {
        h += epd_text_height(lines[i].scale) + CARD_LEADING * lines[i].scale;
    }
    return h;
}

static int32_t lines_widest(const card_line_t *lines, int n)
{
    int32_t w = 0;
    for (int i = 0; i < n; i++) {
        const int32_t lw = epd_text_width(lines[i].text, lines[i].scale);
        if (lw > w) {
            w = lw;
        }
    }
    return w;
}

// Draws the lines top to bottom and returns the y the next thing starts at.
//
// **A line wider than its column is refused and announced, not clipped.** The strings here include
// a user-settable device name and the access point's SSID, so this is reachable rather than
// theoretical -- and half an address on the glass is worse than none, because somebody will type
// it. The announcement carries the widths and NOT the text: the password is one of these lines and
// this output ends up in capture files.
static int32_t draw_lines(const card_line_t *lines, int n, int32_t x, int32_t y, int32_t limit_w)
{
    for (int i = 0; i < n; i++) {
        const int32_t w = epd_text_width(lines[i].text, lines[i].scale);
        if (w > limit_w) {
            printf("# card: line %d does not fit (%ld > %ld px at scale %ld)\n", i, (long)w,
                   (long)limit_w, (long)lines[i].scale);
        } else {
            epd_text_draw(&s_canvas, x, y, lines[i].scale, lines[i].text, 0, 0, 0);
        }
        y += epd_text_height(lines[i].scale) + CARD_LEADING * lines[i].scale;
    }
    return y;
}

// The lines above the join QR: what network, and how to get onto it.
static int build_join_lines(const app_display_card_t *c, card_text_t *t, card_line_t *out)
{
    int n = 0;
    snprintf(t->join_label, sizeof(t->join_label), "1 JOIN WI-FI");
    out[n++] = (card_line_t){2, t->join_label};

    if (c->ssid[0]) {
        upper_copy(t->ssid, sizeof(t->ssid), c->ssid);
        out[n++] = (card_line_t){3, t->ssid};
    }
    // An open access point is a real state -- FRAME_AP_OPEN, and a board_wifi_ap_secure() that
    // refused -- and the card has to say which, or a user hunts for a password that is not there.
    if (c->ap_pass[0]) {
        snprintf(t->pass, sizeof(t->pass), "PASS %s", c->ap_pass);
    } else {
        snprintf(t->pass, sizeof(t->pass), "NO PASSWORD");
    }
    out[n++] = (card_line_t){3, t->pass};
    return n;
}

// The lines above the pairing QR: where the page is, and the code for a browser that cannot scan.
static int build_open_lines(const app_display_card_t *c, card_text_t *t, card_line_t *out)
{
    int n = 0;
    snprintf(t->open_label, sizeof(t->open_label), "2 BROWSE TO");
    out[n++] = (card_line_t){2, t->open_label};

    // The name first and the address second: the name survives a DHCP lease changing, and mDNS is
    // FR-2.3. Both, because .local resolution is the half most likely to be broken on a given PC.
    if (c->host[0]) {
        upper_copy(t->host, sizeof(t->host), c->host);
        out[n++] = (card_line_t){2, t->host};
    }
    const char *addr = c->sta_ip[0] ? c->sta_ip : c->ap_ip;
    if (addr[0]) {
        snprintf(t->alt, sizeof(t->alt), "OR %s", addr);
        out[n++] = (card_line_t){2, t->alt};
    }
    if (c->code[0]) {
        snprintf(t->code, sizeof(t->code), "3 CODE %s", c->code);
        out[n++] = (card_line_t){3, t->code};
    }
    return n;
}

static esp_err_t render_card(const app_display_card_t *c, float *total_ms)
{
    epd_canvas_fill(&s_canvas, 255, 255, 255);

    const int32_t lw = epd_canvas_logical_width(&s_canvas);
    const int32_t lh = epd_canvas_logical_height(&s_canvas);

    card_text_t text;
    card_line_t join_lines[CARD_LINES_MAX], open_lines[CARD_LINES_MAX];
    const int join_n = build_join_lines(c, &text, join_lines);
    const int open_n = build_open_lines(c, &text, open_lines);

    // **Two columns are chosen by measurement, not by aspect ratio, and that is not tidiness.** A
    // 50/50 split was the first version and it is wrong on the M5Paper Color at rotation 1: the
    // logical card is 600x400, half of it is 284 px, and the widest line -- "PASS" plus sixteen hex
    // characters, 375 px -- does not fit, so the access point's password was announced as
    // unfittable and not drawn. Found by arithmetic before the first flash. So the words get what
    // they measure and the codes get the rest, and a panel too narrow for both falls back to
    // stacking them.
    const int32_t widest = lines_widest(join_lines, join_n) > lines_widest(open_lines, open_n)
                               ? lines_widest(join_lines, join_n)
                               : lines_widest(open_lines, open_n);
    // Three pixels a module on the smaller of the two codes. Below that a camera has no chance and
    // the stacked form, whatever it costs in words, is the better answer.
    const int32_t min_qr_col = 3 * (33 + 2 * QR_QUIET_MODULES);
    const int32_t qr_col_x = CARD_MARGIN + widest + CARD_GAP;
    const bool two_col = lh < lw && (lw - qr_col_x - CARD_MARGIN) >= min_qr_col;

    esp_err_t err;
    int32_t qr_h;
    if (!two_col) {
        // One column -- either portrait, or a landscape panel too narrow to put the codes beside
        // the words. Each block of words sits above the code it explains. **The QR boxes get
        // whatever the words leave**, and draw_qr() derives its own module scale from the box it is
        // given -- which is the mechanism that has to stay, because the module count depends on how
        // long the SSID and the token are. The scales this produces are what the flatbed decode
        // checks; they are not chosen here. At 400x600 that is 5 for the join code and 4 for the
        // pairing URL, against the 8 and 7 the codes had to themselves before there were words.
        const int32_t text_h = lines_height(join_lines, join_n) + lines_height(open_lines, open_n);
        int32_t avail = lh - 2 * CARD_MARGIN - text_h - 2 * CARD_GAP;
        if (avail < 2) {
            avail = 2;
        }
        qr_h = avail / 2;

        const int32_t colw = lw - 2 * CARD_MARGIN;
        int32_t y = CARD_MARGIN;
        y = draw_lines(join_lines, join_n, CARD_MARGIN, y, colw);
        err = draw_qr(c->join, 0, y, lw, qr_h);
        y += qr_h + CARD_GAP;
        y = draw_lines(open_lines, open_n, CARD_MARGIN, y, colw);
        if (err == ESP_OK) {
            err = draw_qr(c->url, 0, y, lw, qr_h);
        }
    } else {
        // Landscape: the words down the left, both codes stacked on the right, in the column the
        // words did not need. 800x480 gives the codes 401x228 and 600x400 gives them 201x188.
        const int32_t qr_w = lw - qr_col_x - CARD_MARGIN;
        qr_h = (lh - 2 * CARD_MARGIN - CARD_GAP) / 2;

        int32_t y = CARD_MARGIN;
        y = draw_lines(join_lines, join_n, CARD_MARGIN, y, widest);
        y += CARD_GAP;
        draw_lines(open_lines, open_n, CARD_MARGIN, y, widest);

        err = draw_qr(c->join, qr_col_x, CARD_MARGIN, qr_w, qr_h);
        if (err == ESP_OK) {
            err = draw_qr(c->url, qr_col_x, CARD_MARGIN + qr_h + CARD_GAP, qr_w, qr_h);
        }
    }

    // What the layout decided, so a capture records it beside the `qr modules=` lines. No text,
    // for the reason draw_lines() gives.
    printf("# card: %ldx%ld cols=%d lines=%d+%d widest=%ld qr_h=%ld\n", (long)lw, (long)lh,
           two_col ? 2 : 1, join_n, open_n, (long)widest, (long)qr_h);

    if (err != ESP_OK) {
        return err;
    }

    // Nearest, not dithered: a QR is one-bit art and error diffusion would scatter grey
    // into the quiet zone and soften the module edges, which is exactly what a decoder
    // has to resolve. The auto flow is deliberately not consulted -- this is not a photograph,
    // and a classifier that decided it was a flat illustration would start reshaping its tones.
    const epd_render_t cfg = current_render();
    dither_canvas(true, &cfg);

    epd_timings_t t = {0};
    t.run = 1;
    const esp_err_t rerr = epd_refresh_frame(s_packed, EPD_FRS_STOCK, &t);
    *total_ms = (float)t.total_us / 1000.0f;
    return rerr;
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);

        for (;;) {
            lock();
            const request_kind_t kind = s_pending_kind;
            char path[REQUEST_PATH_MAX];
            copy_str(path, s_pending_path, sizeof(path));
            const uint8_t flat = s_pending_flat;
            if (kind == REQ_CARD) {
                // Into the static in-flight copy, not onto this stack: see s_card.
                s_card = s_pending_card;
            }
            s_pending_kind = REQ_NONE;
            if (kind == REQ_NONE) {
                unlock();
                break;
            }
            s_busy = true;
            epd_canvas_set_rotation(&s_canvas, s_rotation);
            unlock();
            // Ticket 47: published for the heap sampler, which cannot call app_display_busy()
            // because that takes this module's mutex and the sampler runs at 20 ms from a
            // priority-3 task.
            app_heapwatch_set_activity(HEAPWATCH_F_DISPLAY, true);

            // The LED is the only sign of life during a 15.6 s refresh: the panel shows
            // the OLD image for all of it, so "working" and "hung" look identical
            // without this.
            board_led_set(BOARD_LED_REFRESH);

            const int64_t t0 = esp_timer_get_time();
            float total_ms = 0.0f;
            esp_err_t err;
            if (kind == REQ_BLANK) {
                err = render_blank(&total_ms);
            } else if (kind == REQ_FLAT) {
                err = render_flat(flat, &total_ms);
            } else if (kind == REQ_CARD) {
                err = render_card(&s_card, &total_ms);
            } else {
                err = render_image(path, &total_ms);
            }
            const float wall_ms = (float)(esp_timer_get_time() - t0) / 1000.0f;

            lock();
            s_busy = false;
            s_last_total_ms = total_ms;
            if (err == ESP_OK) {
                s_renders++;
                s_last_error[0] = '\0';
                if (kind == REQ_BLANK || kind == REQ_CARD || kind == REQ_FLAT) {
                    // Deliberately not remembered as the current image: a
                    // rotation change calls app_display_redraw(), and redrawing
                    // a screen full of credentials because somebody turned the
                    // frame sideways is not what that button means. A flat is here
                    // for the same reason, plus one of its own -- a maintenance
                    // course must not be re-entered one step at a time by a
                    // redraw (ticket 68).
                    s_current[0] = '\0';
                    s_current_path[0] = '\0';
                } else {
                    copy_str(s_current, basename_of(path), sizeof(s_current));
                    copy_str(s_current_path, path, sizeof(s_current_path));
                }
            } else {
                s_failures++;
                copy_str(s_last_error, esp_err_to_name(err), sizeof(s_last_error));
            }
            unlock();
            app_heapwatch_set_activity(HEAPWATCH_F_DISPLAY, false);

            // SUCCESS and ERROR both play a short pattern and fall back to the idle
            // heartbeat on their own, so nothing here has to schedule the return.
            board_led_set(err == ESP_OK ? BOARD_LED_SUCCESS : BOARD_LED_ERROR);

            // Raw numbers, one line per render: what was asked for, what the panel took,
            // and what the whole decode-plus-refresh cost. A verdict computed here would
            // be a second thing that can be wrong.
            // A flat names its COLOUR, not just its kind: the maintenance course is ten of these in
            // a fixed order, and a capture has to be countable by colour or a course that rendered
            // yellow twice reads exactly like one that rendered yellow and green (ticket 68).
            char flat_name[16];
            snprintf(flat_name, sizeof(flat_name), "(flat:%s)", epd_maint_colour_name(flat));
            printf("# render name=%s kind=%d err=%s panel_ms=%.1f wall_ms=%.1f\n",
                   kind == REQ_BLANK   ? "(blank)"
                       : kind == REQ_CARD ? "(card)"
                       : kind == REQ_FLAT ? flat_name
                                          : basename_of(path),
                   (int)kind,
                   esp_err_to_name(err), (double)total_ms, (double)wall_ms);
            fflush(stdout);
        }
    }
}

esp_err_t app_display_init(uint8_t rotation)
{
    if (s_mutex) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (!s_mutex || !s_wake) {
        return ESP_ERR_NO_MEM;
    }

    s_canvas_buf = heap_caps_malloc(epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT), MALLOC_CAP_SPIRAM);
    s_packed = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf || !s_packed) {
        ESP_LOGE(TAG, "canvas/frame allocation failed -- is PSRAM enabled?");
        return ESP_ERR_NO_MEM;
    }
    if (!epd_canvas_init(&s_canvas, s_canvas_buf, epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT),
                         EPD_WIDTH, EPD_HEIGHT)) {
        return ESP_ERR_INVALID_SIZE;
    }
    // **This line and app_display_set_rotation()'s guard are the two sites ticket 69 §6 had not
    // counted**, and they are the nastiest of the seven: this one SUBSTITUTES a default rather than
    // refusing, so with app_settings and epd_canvas widened but not these, a stored rotation of 2
    // survives NVS, reads back correctly over HTTP, and the panel comes up at 1 at every boot.
    s_rotation = rotation <= EPD_CANVAS_ROTATION_MAX ? rotation : 1;
    epd_canvas_set_rotation(&s_canvas, s_rotation);
    s_palette = app_settings_palette();
    s_auto_adjust = app_settings_auto_adjust();
    s_dither_diffuse = app_settings_dither_diffuse();
    s_auto_rotate = app_settings_auto_rotate();

    // 8 KB, and do NOT trim it from a high-water mark taken after a JPEG.
    //
    // It was cut to 4608 on that basis during the internal-RAM work (ticket 21) -- the
    // sample that said "3584 bytes spare" had only ever rendered a JPEG. The first PNG
    // afterwards left **16 bytes** of stack and the device went into a reboot loop:
    // `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)` every ~20 s, once per
    // render. libpng's call chain is far deeper than TJpgDec's, and the four photographs
    // the factory firmware left on /data are all PNGs, so this is the normal path rather
    // than an edge case.
    //
    // 8192 rather than the original 6144: 6144 survived a PNG, but with 1.5 KB spare at a
    // point where the measurement had already been wrong once.
    if (xTaskCreate(display_task, "display", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_display_set_rotation(uint8_t rotation)
{
    if (rotation > EPD_CANVAS_ROTATION_MAX) {
        return;
    }
    lock();
    s_rotation = rotation;
    unlock();
}

void app_display_set_palette(uint8_t palette)
{
    if (palette >= EPD_PALETTE_ID_COUNT) {
        return;
    }
    lock();
    s_palette = palette;
    unlock();
}

void app_display_set_auto_adjust(bool on)
{
    lock();
    s_auto_adjust = on;
    unlock();
}

void app_display_set_dither_diffuse(bool on)
{
    lock();
    s_dither_diffuse = on;
    unlock();
}

void app_display_set_auto_rotate(bool on)
{
    lock();
    s_auto_rotate = on;
    unlock();
}

static esp_err_t post(request_kind_t kind, const char *path)
{
    if (kind == REQ_IMAGE && (!path || !path[0] || strlen(path) >= REQUEST_PATH_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_pending_kind = kind;
    copy_str(s_pending_path, path ? path : "", sizeof(s_pending_path));
    unlock();
    xSemaphoreGive(s_wake);
    return ESP_OK;
}

esp_err_t app_display_request(const char *path)
{
    return post(REQ_IMAGE, path);
}

esp_err_t app_display_request_blank(void)
{
    return post(REQ_BLANK, NULL);
}

esp_err_t app_display_request_flat(uint8_t colour_index)
{
    // Refused here, at the door, rather than discovered by render_flat() fifteen seconds later: a
    // course that asked for index 4 would otherwise count a failure against a step that had never
    // been drawable in the first place.
    if (!epd_color_valid(colour_index)) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_pending_flat = colour_index;
    unlock();
    return post(REQ_FLAT, NULL);
}

esp_err_t app_display_request_card(const app_display_card_t *card)
{
    // The two QR payloads are the only fields without which this screen is pointless -- every
    // other line is skipped when empty, which is what makes an open access point and a frame with
    // no station link draw correctly rather than blankly.
    if (!card || !card->join[0] || !card->url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    s_pending_card = *card;
    unlock();
    return post(REQ_CARD, NULL);
}

esp_err_t app_display_redraw(void)
{
    char path[REQUEST_PATH_MAX];
    lock();
    copy_str(path, s_current_path, sizeof(path));
    unlock();
    if (!path[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    return post(REQ_IMAGE, path);
}

void app_display_state(app_display_state_t *out)
{
    if (!out) {
        return;
    }
    lock();
    out->busy = s_busy;
    out->pending = s_pending_kind != REQ_NONE;
    copy_str(out->current, s_current, sizeof(out->current));
    copy_str(out->last_error, s_last_error, sizeof(out->last_error));
    out->renders = s_renders;
    out->failures = s_failures;
    out->last_total_ms = s_last_total_ms;
    unlock();
}

bool app_display_busy(void)
{
    lock();
    const bool busy = s_busy || s_pending_kind != REQ_NONE;
    unlock();
    return busy;
}

bool app_display_wait_idle(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (app_display_busy()) {
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}
