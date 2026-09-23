#include "img_resize.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"

#include "epd_dither.h"
#include "epd_image.h"
#include "img_dims.h"
#include "img_scale.h"

static const char *TAG = "imgresize";

// heap_caps_malloc with a malloc fallback, as epd_image.c:104-111 and app_server.c do for
// the same reason: a small allocation can still legitimately come from internal RAM.
static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = malloc(n);
    }
    return p;
}

// 16-byte aligned AND in PSRAM. esp_new_jpeg's own jpeg_calloc_align() would satisfy the
// alignment its FAQ demands, but it is a prebuilt .a and does not document which pool it
// draws from, and these buffers run to hundreds of kilobytes against ~72 kB of free
// internal RAM. heap_caps_aligned_alloc gives both guarantees in one call.
static uint8_t *rgb_alloc(size_t n)
{
    return heap_caps_aligned_alloc(IMG_RESIZE_ALIGN, n, MALLOC_CAP_SPIRAM);
}

// The output size for a source reduced by `fac`, cropped to the alignment. Zero for
// anything that would not survive the crop, which the callers turn into ESP_FAIL.
static void reduced_size(int32_t w, int32_t h, int32_t fac, int32_t *ow, int32_t *oh)
{
    *ow = (w / fac) / IMG_RESIZE_ALIGN * IMG_RESIZE_ALIGN;
    *oh = (h / fac) / IMG_RESIZE_ALIGN * IMG_RESIZE_ALIGN;
}

// The reduction, over a row stream. Accumulates `fac` source rows into a `ow * 3` band of
// uint32 and divides once per output row, so a pixel is touched once and the division
// happens per output pixel rather than per input pixel.
static esp_err_t reduce_rows(epd_image_reader_t *reader, int32_t sw, int32_t sh, int32_t fac,
                             uint8_t *dst_base, int32_t ow, int32_t oh)
{
    uint32_t *acc = psram_alloc((size_t)ow * 3u * sizeof(uint32_t));
    if (acc == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(acc, 0, (size_t)ow * 3u * sizeof(uint32_t));

    int32_t out_y = 0;
    int32_t in_group = 0;
    for (int32_t y = 0; y < sh && out_y < oh; y++) {
        const uint8_t *row = NULL;
        if (epd_image_next_row(reader, &row) != ESP_OK) {
            break;
        }
        for (int32_t ox = 0; ox < ow; ox++) {
            for (int32_t k = 0; k < fac; k++) {
                const int32_t ix = ox * fac + k;
                if (ix >= sw) {
                    break;
                }
                acc[ox * 3 + 0] += row[ix * 3 + 0];
                acc[ox * 3 + 1] += row[ix * 3 + 1];
                acc[ox * 3 + 2] += row[ix * 3 + 2];
            }
        }
        if (++in_group == fac) {
            const uint32_t n = (uint32_t)fac * (uint32_t)fac;
            uint8_t *dst = dst_base + (size_t)out_y * (size_t)ow * 3u;
            for (int32_t i = 0; i < ow * 3; i++) {
                dst[i] = (uint8_t)(acc[i] / n);
            }
            memset(acc, 0, (size_t)ow * 3u * sizeof(uint32_t));
            in_group = 0;
            out_y++;
        }
    }
    free(acc);

    // A short stream is a truncated or corrupt file. It must not become a half-black
    // thumbnail: the caller gets an error and answers with a broken tile instead.
    return (out_y == oh) ? ESP_OK : ESP_FAIL;
}

// Shared by both entry points. `fit_w`/`fit_h` is the hint the decoder reduces towards;
// `fac_of` then decides the integer box factor from the DECODED dimensions, which is why
// the two rules cannot be applied before the open.
static esp_err_t resize_core_raw(const uint8_t *src, size_t src_len, int32_t fit_w,
                                 int32_t fit_h, bool long_edge_rule, int32_t rule_arg_w,
                                 int32_t rule_arg_h, uint8_t **out_rgb, int32_t *out_w,
                                 int32_t *out_h)
{
    if (src == NULL || src_len == 0 || out_rgb == NULL || out_w == NULL || out_h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rgb = NULL;
    *out_w = 0;
    *out_h = 0;

    epd_image_reader_storage_t rstore;
    epd_image_reader_t *reader = NULL;
    const esp_err_t oerr = epd_image_open_mem_fit(&rstore, src, src_len, fit_w, fit_h, &reader);
    if (oerr != ESP_OK) {
        // NO_MEM is not a malformed file. A 12 MP decode needs ~0.6 MB of PSRAM on top of
        // the file, so it is reachable whenever a large render is in flight.
        return (oerr == ESP_ERR_NO_MEM) ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    const int32_t sw = epd_image_width(reader);
    const int32_t sh = epd_image_height(reader);
    const int32_t box = rule_arg_w;
    int32_t fac;
    if (long_edge_rule) {
        const int32_t big = sw > sh ? sw : sh;
        fac = (big + rule_arg_w - 1) / rule_arg_w; // round UP: a thumbnail may be soft
        if (fac < 1) {
            fac = 1;
        }
    } else {
        fac = epd_fit_reduction(sw, sh, rule_arg_w, rule_arg_h, IMG_RESIZE_ALIGN);
    }

    int32_t ow, oh;
    reduced_size(sw, sh, fac, &ow, &oh);
    if (ow < IMG_RESIZE_ALIGN || oh < IMG_RESIZE_ALIGN) {
        epd_image_close(reader);
        return ESP_FAIL;
    }

    // A factor of 1 is not an edge case, it is the JPEG path: choose_jpeg_scale() leaves the
    // decode between 1x and 2x the fit, so a box factor of 2 would undershoot and the factor
    // is ALWAYS 1 for JPEG. reduce_rows() would then allocate a second full-size buffer and
    // memcpy the first into it -- 8.06 MB against 6.44 MB of PSRAM for a 12 MP photograph,
    // which is the whole difference between a phone photograph resizing and not. So take the
    // decoder's own buffer instead of copying it, and crop to the alignment in place.
    //
    // ow <= sw and oh <= sh, so the destination row never overtakes the source row and a
    // forward walk is safe. The buffer stays its original size and is freed whole.
    uint8_t *rgb = NULL;
    int32_t took_w = 0, took_h = 0;
    if (fac == 1 && epd_image_take_rgb(reader, &rgb, &took_w, &took_h) == ESP_OK) {
        epd_image_close(reader);

        // The factor being 1 does not mean there is nothing to reduce -- it means the
        // reduction that is left is not an INTEGER. For a 12 MP photograph the decoder leaves
        // 1008x752 against a fit of 600x450, a ratio of 1.68, and a box factor of 2 would
        // undershoot. Measured on hardware 2026-09-09: 2.8x the pixels the panel draws, stored
        // on the card and fetched over the wire for every photograph in the library.
        //
        // `box` is rule_arg_w for both entry points because both rules are square: the fit
        // rule takes a square deliberately so one stored file serves either orientation
        // (SMB_RESIZE_FIT_EDGE), and the long-edge rule is a square by definition.
        int32_t fw = 0, fh = 0, tw = 0, th = 0;
        if (img_scale_fit_target(took_w, took_h, box, IMG_RESIZE_ALIGN, &fw, &fh, &tw, &th) &&
            (tw < took_w || th < took_h)) {
            uint8_t *scaled = rgb_alloc((size_t)tw * (size_t)th * 3u);
            if (scaled == NULL) {
                // Not fatal: the unscaled buffer is still a correct answer, just a larger one,
                // and refusing here would store the ORIGINAL instead -- larger again.
                ESP_LOGW(TAG, "no room to resample %dx%d to %dx%d; keeping the decode",
                         (int)took_w, (int)took_h, (int)tw, (int)th);
            } else if (img_scale_area(rgb, took_w, took_h, fw, fh, scaled, tw, th)) {
                heap_caps_free(rgb);
                *out_rgb = scaled;
                *out_w = tw;
                *out_h = th;
                return ESP_OK;
            } else {
                heap_caps_free(scaled);
                ESP_LOGW(TAG, "resample %dx%d -> %dx%d refused; keeping the decode",
                         (int)took_w, (int)took_h, (int)tw, (int)th);
            }
        }

        // Nothing to resample, or it could not be done: crop the decode to the alignment in
        // place. ow <= sw and oh <= sh, so the destination row never overtakes the source row
        // and a forward walk is safe; the buffer keeps its original size and is freed whole.
        for (int32_t y = 0; y < oh; y++) {
            memmove(rgb + (size_t)y * (size_t)ow * 3u, rgb + (size_t)y * (size_t)took_w * 3u,
                    (size_t)ow * 3u);
        }
        *out_rgb = rgb;
        *out_w = ow;
        *out_h = oh;
        return ESP_OK;
    }

    rgb = rgb_alloc((size_t)ow * (size_t)oh * 3u);
    if (rgb == NULL) {
        epd_image_close(reader);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t rerr = reduce_rows(reader, sw, sh, fac, rgb, ow, oh);
    epd_image_close(reader);
    if (rerr != ESP_OK) {
        heap_caps_free(rgb);
        return rerr;
    }

    *out_rgb = rgb;
    *out_w = ow;
    *out_h = oh;
    return ESP_OK;
}

// The reduction, then the turn the camera asked for. Both entry points go through here, so
// the stored photograph and its thumbnail cannot disagree about which way up they are, and
// h_thumb_serve()'s live path gets it from the same place.
//
// **After the reduction, and that is a memory argument rather than an ordering preference.**
// A 90 degree turn needs a second buffer of the same size; here that buffer is bounded by
// the square fit box (600x600x3 = 1.08 MB) instead of by the decode, which for a 12 MP
// photograph is 1008x752 and would put the peak over the 6.36 MB of PSRAM this board has.
// The same reasoning is why the DISPLAY path does not do this yet -- ticket 53.
//
// **And nothing above has to know.** Both entry points pass a SQUARE rule argument, so
// choose_jpeg_scale()'s hint, epd_fit_reduction() and img_scale_fit_target() are all
// invariant under the exchange of width and height: the target for the upright picture is
// exactly the transpose of the target computed from the file's own dimensions. That is
// ticket 49's square box (SMB_RESIZE_FIT_EDGE) earning its keep a second time, after
// ticket 51's draw-time rotation.
static esp_err_t resize_core(const uint8_t *src, size_t src_len, int32_t fit_w, int32_t fit_h,
                             bool long_edge_rule, int32_t rule_arg_w, int32_t rule_arg_h,
                             uint8_t **out_rgb, int32_t *out_w, int32_t *out_h)
{
    const esp_err_t err = resize_core_raw(src, src_len, fit_w, fit_h, long_edge_rule,
                                          rule_arg_w, rule_arg_h, out_rgb, out_w, out_h);
    if (err != ESP_OK) {
        return err;
    }

    const int orient = img_exif_orientation(src, src_len);
    if (!img_orient_needed(orient)) {
        return ESP_OK;
    }

    const int32_t sw = *out_w;
    const int32_t sh = *out_h;
    const bool swaps = img_orient_swaps_axes(orient);
    const int32_t dw = swaps ? sh : sw;
    const int32_t dh = swaps ? sw : sh;

    uint8_t *turned = rgb_alloc((size_t)dw * (size_t)dh * 3u);
    if (turned == NULL || !img_orient_rgb(*out_rgb, sw, sh, orient, turned)) {
        // Sideways beats absent. The picture is still a correct decode of the file's own
        // pixels, which is what the frame did for every photograph before this existed.
        if (turned != NULL) {
            heap_caps_free(turned);
        }
        ESP_LOGW(TAG, "orientation %d not applied to %dx%d; storing it as it was", orient,
                 (int)sw, (int)sh);
        return ESP_OK;
    }

    heap_caps_free(*out_rgb);
    *out_rgb = turned;
    *out_w = dw;
    *out_h = dh;
    return ESP_OK;
}

esp_err_t img_resize_to_edge(const uint8_t *src, size_t src_len, int32_t max_edge,
                             uint8_t **out_rgb, int32_t *out_w, int32_t *out_h)
{
    if (max_edge < IMG_RESIZE_ALIGN) {
        return ESP_ERR_INVALID_ARG;
    }
    // HALF of max_edge as the decoder hint, and the halving is the point rather than a
    // fudge. epd_image's choose_jpeg_scale() rounds its reduction DOWN so the decode is
    // never smaller than what it will be drawn at, which is the right invariant for the
    // panel and the wrong one here: the decode is the dominant cost and a thumbnail may be
    // a little soft. At a nominal 384 the bench share's 768x1344 gives need=3.5, still
    // scale 1/2 and no gain at all; at 192 it gives need=7.0 and scale 1/4, which is
    // 192x336 -- exactly the intended thumbnail, from a quarter of the decode. The box
    // factor below then comes out 1 and the reduction degenerates to a copy, and TJpgDec's
    // own reduction is IDCT-based rather than a box filter, so nothing is lost by handing
    // the work to it.
    return resize_core(src, src_len, max_edge / 2, max_edge / 2, true, max_edge, 0, out_rgb,
                       out_w, out_h);
}

esp_err_t img_resize_to_fit(const uint8_t *src, size_t src_len, int32_t screen_w,
                            int32_t screen_h, uint8_t **out_rgb, int32_t *out_w,
                            int32_t *out_h)
{
    if (screen_w < IMG_RESIZE_ALIGN || screen_h < IMG_RESIZE_ALIGN) {
        return ESP_ERR_INVALID_ARG;
    }
    // The panel's own size as the hint, so choose_jpeg_scale()'s round-DOWN is exactly the
    // invariant wanted here, and epd_fit_reduction() then takes whatever is left. Both
    // stages round the same way, which is what makes the guarantee compose.
    return resize_core(src, src_len, screen_w, screen_h, false, screen_w, screen_h, out_rgb,
                       out_w, out_h);
}

esp_err_t img_resize_box(const uint8_t *rgb, int32_t w, int32_t h, int32_t fac,
                         uint8_t **out_rgb, int32_t *out_w, int32_t *out_h)
{
    if (rgb == NULL || w <= 0 || h <= 0 || fac < 1 || out_rgb == NULL || out_w == NULL ||
        out_h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rgb = NULL;
    *out_w = 0;
    *out_h = 0;

    int32_t ow, oh;
    reduced_size(w, h, fac, &ow, &oh);
    if (ow < IMG_RESIZE_ALIGN || oh < IMG_RESIZE_ALIGN) {
        return ESP_FAIL;
    }

    uint8_t *dst = rgb_alloc((size_t)ow * (size_t)oh * 3u);
    if (dst == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t n = (uint32_t)fac * (uint32_t)fac;
    for (int32_t oy = 0; oy < oh; oy++) {
        uint8_t *drow = dst + (size_t)oy * (size_t)ow * 3u;
        for (int32_t ox = 0; ox < ow; ox++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int32_t ky = 0; ky < fac; ky++) {
                const uint8_t *srow = rgb + (size_t)(oy * fac + ky) * (size_t)w * 3u;
                for (int32_t kx = 0; kx < fac; kx++) {
                    const int32_t ix = ox * fac + kx;
                    r += srow[ix * 3 + 0];
                    g += srow[ix * 3 + 1];
                    b += srow[ix * 3 + 2];
                }
            }
            drow[ox * 3 + 0] = (uint8_t)(r / n);
            drow[ox * 3 + 1] = (uint8_t)(g / n);
            drow[ox * 3 + 2] = (uint8_t)(b / n);
        }
    }

    *out_rgb = dst;
    *out_w = ow;
    *out_h = oh;
    return ESP_OK;
}

esp_err_t img_encode_jpeg(const uint8_t *rgb, int32_t w, int32_t h, int quality,
                          uint8_t **out, size_t *out_len)
{
    if (rgb == NULL || w <= 0 || h <= 0 || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;

    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = (int)w;
    cfg.height = (int)h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = quality;
    // false: task_enable spawns a helper task, and internal RAM is the scarce resource
    // here (ticket 21 -- two small tasks once cost the frame its whole web UI).
    cfg.task_enable = false;

    // One byte per pixel of headroom. A 4:2:0 photograph lands near an eighth of that at
    // q65 and near a fifth at q80; this is a ceiling, not an estimate, and it is PSRAM.
    const size_t rgb_len = (size_t)w * (size_t)h * 3u;
    const size_t cap = rgb_len / 3u + 1024u;
    uint8_t *buf = psram_alloc(cap);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // NO MUTEX, and that is measured rather than reasoned. The encoder is a constant 10 kB
    // of DRAM; esp_http_server serves one request at a time (measured 2026-09-07,
    // uxTaskGetSystemState shows exactly one httpd task), and the SMB window task is the
    // only other caller and runs one file at a time. If httpd ever gains a second worker
    // this becomes wrong, which is why the reason is written down.
    jpeg_enc_handle_t enc = NULL;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
        free(buf);
        ESP_LOGW(TAG, "jpeg_enc_open failed for %dx%d q%d", (int)w, (int)h, quality);
        return ESP_FAIL;
    }
    int size = 0;
    const jpeg_error_t jerr = jpeg_enc_process(enc, rgb, (int)rgb_len, buf, (int)cap, &size);
    jpeg_enc_close(enc);
    if (jerr != JPEG_ERR_OK || size <= 0) {
        free(buf);
        ESP_LOGW(TAG, "jpeg_enc_process=%d for %dx%d", (int)jerr, (int)w, (int)h);
        return ESP_FAIL;
    }

    *out = buf;
    *out_len = (size_t)size;
    return ESP_OK;
}
