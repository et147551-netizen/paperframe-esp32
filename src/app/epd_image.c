#include "epd_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "jpeg_decoder.h"
#include "png.h"

#include "epd_geom.h"

static const char *TAG = "img";

// The panel. Used to decide how far a JPEG may be scaled down during decode.
//
// **Derived from epd_regs.h rather than written out again**, which is ticket 43's seam 1 and the
// one item on that survey that did not need the port to justify it: these were a literal 400 and
// 600, the same numbers as EPD_WIDTH and EPD_HEIGHT with no #include relating them, so a panel of a
// different size would have to be found in two places and **a build that changed only one of them
// would compile**. The names stay so the uses below read as they did.
#define PANEL_W EPD_WIDTH
#define PANEL_H EPD_HEIGHT

// The ceiling now lives in epd_image.h, because /thumb/ reads files itself and has to agree
// with it. Kept as a local name so the uses below read as they did.
#define JPEG_MAX_FILE_BYTES EPD_IMAGE_MAX_FILE_BYTES

// ------------------------------------------------------------------- byte source

typedef struct {
    const uint8_t *data; // memory-backed
    size_t len;
    size_t pos;
    FILE *fp;            // file-backed
    uint8_t *owned;      // freed on close; set when a file was slurped for the JPEG path
} byte_src_t;

static size_t src_read(byte_src_t *s, uint8_t *dst, size_t n)
{
    if (s->fp != NULL) {
        return fread(dst, 1, n, s->fp);
    }
    const size_t avail = (s->pos < s->len) ? (s->len - s->pos) : 0;
    const size_t take = (n < avail) ? n : avail;
    memcpy(dst, s->data + s->pos, take);
    s->pos += take;
    return take;
}

static bool src_seek(byte_src_t *s, size_t off)
{
    if (s->fp != NULL) {
        return fseek(s->fp, (long)off, SEEK_SET) == 0;
    }
    if (off > s->len) {
        return false;
    }
    s->pos = off;
    return true;
}

static long src_size(byte_src_t *s)
{
    if (s->fp == NULL) {
        return (long)s->len;
    }
    const long here = ftell(s->fp);
    if (here < 0 || fseek(s->fp, 0, SEEK_END) != 0) {
        return -1;
    }
    const long end = ftell(s->fp);
    fseek(s->fp, here, SEEK_SET);
    return end;
}

// ------------------------------------------------------------------- reader state

struct epd_image_reader {
    epd_image_format_t format;
    int32_t width;
    int32_t height;
    int32_t next_row;

    // What the caller intends to draw into, which is what choose_jpeg_scale() reduces
    // towards. PANEL_W x PANEL_H for everything that draws on the panel; the thumbnail
    // route asks for a quarter of that and gets a quarter of the decode with it.
    int32_t fit_w;
    int32_t fit_h;

    // The caller will turn its canvas for a picture whose orientation disagrees with it
    // (app_display.c), so the fit this decode is chosen against is the SWAPPED one for
    // exactly those pictures. Without it choose_jpeg_scale() reduces against the fit the
    // draw will not use, which is the larger `need` and therefore the more aggressive
    // scale: a 1000x750 source into 400x600 decodes to 500x375 and is then drawn at
    // 533x400. That is soft rather than wrong, and so would not announce itself.
    bool fit_rotatable;

    // The caller would rather have a SOFT picture than none, so jpeg_open() may decode one
    // power of two below the fit when the buffer for the fit cannot be allocated. Only the
    // display path sets this: img_resize_to_fit() promises a result that is never smaller
    // than the draw size (img_resize.h) and resize_core_raw()'s `tw < took_w` guard would
    // pass an under-size decode through in silence, so the import path must keep failing to
    // ESP_ERR_NO_MEM and storing the original instead.
    bool fit_step_down;

    byte_src_t src;
    uint8_t *row; // width * 3, the row handed back to the caller

    // Set when the decoder could not stream and produced the whole image at once:
    // JPEG always, and interlaced PNG, which cannot be read row by row.
    uint8_t *full_rgb;

    // PNG
    png_structp png;
    png_infop png_info;

    // BMP
    size_t bmp_offset;
    size_t bmp_stride;
    uint32_t bmp_bpp;
    bool bmp_bottom_up;
    uint8_t *bmp_raw;
};

_Static_assert(sizeof(struct epd_image_reader) <= EPD_IMAGE_READER_SIZE,
               "epd_image_reader outgrew EPD_IMAGE_READER_SIZE; raise it in epd_image.h");

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = malloc(n); // small allocations can still come from internal RAM
    }
    return p;
}

// The whole-image buffer, and 16-byte aligned because epd_image_take_rgb() can hand it to
// esp_new_jpeg, which requires that alignment (img_resize.c's rgb_alloc says why). No
// malloc fallback here on purpose: this allocation is megabytes and internal RAM cannot
// serve it, so a fallback would only turn a clean NO_MEM into a stranger failure.
//
// free() is correct for it. heap_caps_aligned_free() is deprecated in favour of
// heap_caps_free() in this IDF (components/heap/include/esp_heap_caps.h:149-154), and
// heap_caps_free() is free().
static void *psram_alloc_image(size_t n)
{
    return heap_caps_aligned_alloc(EPD_IMAGE_RGB_ALIGN, n, MALLOC_CAP_SPIRAM);
}

// ------------------------------------------------------------------ format sniffing

epd_image_format_t epd_image_format_from_name(const char *name)
{
    if (name == NULL) {
        return EPD_IMAGE_UNKNOWN;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return EPD_IMAGE_UNKNOWN;
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return EPD_IMAGE_JPEG;
    }
    if (strcasecmp(dot, ".png") == 0) {
        return EPD_IMAGE_PNG;
    }
    if (strcasecmp(dot, ".bmp") == 0) {
        return EPD_IMAGE_BMP;
    }
    return EPD_IMAGE_UNKNOWN;
}

epd_image_format_t epd_image_sniff(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 8) {
        return EPD_IMAGE_UNKNOWN;
    }
    if (memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return EPD_IMAGE_PNG;
    }
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return EPD_IMAGE_JPEG;
    }
    if (data[0] == 'B' && data[1] == 'M') {
        return EPD_IMAGE_BMP;
    }
    return EPD_IMAGE_UNKNOWN;
}

// Ticket 72: "can this JPEG be decoded at all", for a guard that has to answer before an upload is
// named. **`esp_jpeg_get_image_info()` IS the test and a marker allowlist is not**: the set of files
// this decoder refuses lives inside `espressif/esp_new_jpeg`, a prebuilt binary under "ESPRESSIF
// MIT" that can be neither read nor --wrap'd, and it refuses unusual sampling factors as well as
// progressive scans (ticket 06). Asking the decoder is the set, by definition -- and it is the same
// call jpeg_open() makes below, so the guard and the render path cannot disagree about which files
// are drawable.
//
// **Three-valued on purpose**, because two of these must not be confused:
//   ESP_OK                 the decoder parsed the header -- the frame can draw this
//   ESP_ERR_NOT_SUPPORTED  the decoder refused it -- the caller may refuse the file
//   anything else          the question could not be ASKED (no PSRAM for the file, unreadable).
//                          A caller must accept the file in that case: deleting somebody's
//                          photograph because the frame was briefly short of memory is a worse
//                          defect than the one this guard exists to fix.
//
// It costs one full read of the file into PSRAM, which is what esp_jpeg's pointer API requires.
// Feeding it only the first few KB would be cheaper and is deliberately not done -- how much of a
// file this decoder needs before it will answer is a property of a binary nobody here can read, and
// a truncated buffer that returns "refused" would delete valid photographs.
esp_err_t epd_image_jpeg_probe_file(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    if (size <= 0 || (size_t)size > JPEG_MAX_FILE_BYTES) {
        // Over the ceiling is not this function's refusal to make: app_server.c already answers
        // 413 for that, with its own message, before this is reached.
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = psram_alloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    const bool read_ok = fseek(f, 0, SEEK_SET) == 0 && fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = buf,
        .indata_size = (uint32_t)size,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    const esp_err_t err = esp_jpeg_get_image_info(&cfg, &info);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe: not a decodable JPEG (%s), %ldx%ld", esp_err_to_name(err),
                 (long)info.width, (long)info.height);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

// ------------------------------------------------------------------------- BMP

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static esp_err_t bmp_open(struct epd_image_reader *r)
{
    uint8_t hdr[54];
    if (!src_seek(&r->src, 0) || src_read(&r->src, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (hdr[0] != 'B' || hdr[1] != 'M') {
        return ESP_ERR_INVALID_ARG;
    }

    r->bmp_offset = rd32(&hdr[10]);
    const int32_t w = (int32_t)rd32(&hdr[18]);
    const int32_t h = (int32_t)rd32(&hdr[22]);
    r->bmp_bpp = rd16(&hdr[28]);
    const uint32_t compression = rd32(&hdr[30]);

    if (compression != 0) {
        ESP_LOGE(TAG, "BMP compression %u unsupported", (unsigned)compression);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (r->bmp_bpp != 24 && r->bmp_bpp != 32) {
        ESP_LOGE(TAG, "BMP %u bpp unsupported", (unsigned)r->bmp_bpp);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (w <= 0 || h == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    // A negative height means the rows are stored top-down; the usual case is
    // bottom-up, which is the single most common way to get a BMP upside down.
    r->bmp_bottom_up = (h > 0);
    r->width = w;
    r->height = (h > 0) ? h : -h;

    // Rows are padded out to a 4-byte boundary.
    r->bmp_stride = (((size_t)w * (r->bmp_bpp / 8)) + 3u) & ~(size_t)3u;

    r->bmp_raw = psram_alloc(r->bmp_stride);
    return (r->bmp_raw != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t bmp_next_row(struct epd_image_reader *r)
{
    const size_t file_row = r->bmp_bottom_up ? (size_t)(r->height - 1 - r->next_row)
                                             : (size_t)r->next_row;
    const size_t off = r->bmp_offset + file_row * r->bmp_stride;

    if (!src_seek(&r->src, off) ||
        src_read(&r->src, r->bmp_raw, r->bmp_stride) != r->bmp_stride) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t bytes = r->bmp_bpp / 8;
    for (int32_t x = 0; x < r->width; x++) {
        const uint8_t *s = &r->bmp_raw[(size_t)x * bytes];
        uint8_t *d = &r->row[(size_t)x * 3];
        // BMP stores BGR(A).
        if (bytes == 4) {
            const uint32_t a = s[3];
            d[0] = (uint8_t)((s[2] * a + 255u * (255u - a)) / 255u);
            d[1] = (uint8_t)((s[1] * a + 255u * (255u - a)) / 255u);
            d[2] = (uint8_t)((s[0] * a + 255u * (255u - a)) / 255u);
        } else {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
    }
    return ESP_OK;
}

// ------------------------------------------------------------------------- PNG

static void png_read_cb(png_structp png, png_bytep out, png_size_t n)
{
    byte_src_t *s = (byte_src_t *)png_get_io_ptr(png);
    if (src_read(s, out, n) != n) {
        png_error(png, "short read");
    }
}

static void png_err_cb(png_structp png, png_const_charp msg)
{
    ESP_LOGE(TAG, "libpng: %s", msg);
    png_longjmp(png, 1);
}

static void png_warn_cb(png_structp png, png_const_charp msg)
{
    (void)png;
    ESP_LOGW(TAG, "libpng: %s", msg);
}

static esp_err_t png_open(struct epd_image_reader *r)
{
    if (!src_seek(&r->src, 0)) {
        return ESP_ERR_INVALID_SIZE;
    }

    r->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_err_cb, png_warn_cb);
    if (r->png == NULL) {
        return ESP_ERR_NO_MEM;
    }
    r->png_info = png_create_info_struct(r->png);
    if (r->png_info == NULL) {
        png_destroy_read_struct(&r->png, NULL, NULL);
        return ESP_ERR_NO_MEM;
    }

    if (setjmp(png_jmpbuf(r->png))) {
        png_destroy_read_struct(&r->png, &r->png_info, NULL);
        r->png = NULL;
        return ESP_ERR_INVALID_ARG;
    }

    png_set_read_fn(r->png, &r->src, png_read_cb);
    png_read_info(r->png, r->png_info);

    png_uint_32 w = 0, h = 0;
    int depth = 0, colour = 0, interlace = 0;
    png_get_IHDR(r->png, r->png_info, &w, &h, &depth, &colour, &interlace, NULL, NULL);

    // Normalise everything to 8-bit RGB. Photographs from a phone are truecolour, but
    // screenshots, exported graphics and anything that has been through an optimiser
    // arrive as palettes, greyscale or 16-bit, and all of them have to display.
    if (colour == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(r->png);
    }
    if (colour == PNG_COLOR_TYPE_GRAY && depth < 8) {
        png_set_expand_gray_1_2_4_to_8(r->png);
    }
    if (png_get_valid(r->png, r->png_info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(r->png);
    }
    if (depth == 16) {
        png_set_strip_16(r->png);
    }
    if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(r->png);
    }

    // The panel has no alpha. Composite against white so a transparent PNG matches the
    // white matte the frame draws behind every image (FR-5.3), rather than going black.
    png_color_16 white = {0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    png_set_background(r->png, &white, PNG_BACKGROUND_GAMMA_SCREEN, 0, 1.0);
    png_set_strip_alpha(r->png);

    const int passes = png_set_interlace_handling(r->png);
    png_read_update_info(r->png, r->png_info);

    if (png_get_channels(r->png, r->png_info) != 3) {
        ESP_LOGE(TAG, "PNG did not normalise to 3 channels");
        png_destroy_read_struct(&r->png, &r->png_info, NULL);
        r->png = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    r->width = (int32_t)w;
    r->height = (int32_t)h;

    // Interlaced PNGs cannot be read a row at a time -- the data arrives in seven
    // passes scattered across the image -- so they are the one case that needs the
    // whole thing in memory. Rare, and usually small, but it must not be a crash.
    if (interlace != PNG_INTERLACE_NONE || passes > 1) {
        ESP_LOGW(TAG, "interlaced PNG: buffering %dx%d", (int)w, (int)h);
        const size_t bytes = (size_t)w * (size_t)h * 3u;
        r->full_rgb = psram_alloc_image(bytes);
        if (r->full_rgb == NULL) {
            png_destroy_read_struct(&r->png, &r->png_info, NULL);
            r->png = NULL;
            return ESP_ERR_NO_MEM;
        }
        png_bytep *rows = malloc(sizeof(png_bytep) * h);
        if (rows == NULL) {
            png_destroy_read_struct(&r->png, &r->png_info, NULL);
            r->png = NULL;
            return ESP_ERR_NO_MEM;
        }
        for (png_uint_32 y = 0; y < h; y++) {
            rows[y] = r->full_rgb + (size_t)y * (size_t)w * 3u;
        }
        png_read_image(r->png, rows);
        free(rows);
        png_destroy_read_struct(&r->png, &r->png_info, NULL);
        r->png = NULL;
    }

    return ESP_OK;
}

static esp_err_t png_next_row(struct epd_image_reader *r)
{
    if (setjmp(png_jmpbuf(r->png))) {
        return ESP_ERR_INVALID_SIZE; // truncated or corrupt
    }
    png_read_row(r->png, r->row, NULL);
    return ESP_OK;
}

// ------------------------------------------------------------------------ JPEG

// Largest power-of-two reduction that still leaves the image at least as big as it will
// be drawn. Drawing scales by min(fit_w/w, fit_h/h), so the decoded image only has to be
// 1/max(w/fit_w, h/fit_h) of the original -- decoding larger than that costs memory and
// buys nothing a viewer can see.
//
// `fit_w`/`fit_h` are a parameter rather than PANEL_W/PANEL_H because the cost is not
// small and not everything draws on the panel: measured 2026-09-07, the whole-image decode
// this feeds is 900 ms of a thumbnail's 1.35 s, and a thumbnail needs a quarter of the
// panel's pixels. Everything that does draw on the panel still passes PANEL_W/PANEL_H, so
// that path is unchanged.
static esp_jpeg_image_scale_t choose_jpeg_scale(int32_t w, int32_t h,
                                                int32_t fit_w, int32_t fit_h)
{
    const float need = (w / (float)fit_w > h / (float)fit_h) ? (w / (float)fit_w)
                                                             : (h / (float)fit_h);
    if (need >= 8.0f) {
        return JPEG_IMAGE_SCALE_1_8;
    }
    if (need >= 4.0f) {
        return JPEG_IMAGE_SCALE_1_4;
    }
    if (need >= 2.0f) {
        return JPEG_IMAGE_SCALE_1_2;
    }
    return JPEG_IMAGE_SCALE_0;
}

// One power of two coarser, saturating at 1/8 -- esp_jpeg goes no further. The enum's own
// order is not relied on: a switch says which follows which, so a future value added in the
// middle cannot silently change the ladder.
static esp_jpeg_image_scale_t coarser_jpeg_scale(esp_jpeg_image_scale_t s)
{
    switch (s) {
        case JPEG_IMAGE_SCALE_0:   return JPEG_IMAGE_SCALE_1_2;
        case JPEG_IMAGE_SCALE_1_2: return JPEG_IMAGE_SCALE_1_4;
        default:                   return JPEG_IMAGE_SCALE_1_8;
    }
}

// For the log lines alone, so a step down reads as "1/4 -> 1/8" rather than as two enum
// values nobody can decode from a capture.
static int jpeg_scale_divisor(esp_jpeg_image_scale_t s)
{
    switch (s) {
        case JPEG_IMAGE_SCALE_1_2: return 2;
        case JPEG_IMAGE_SCALE_1_4: return 4;
        case JPEG_IMAGE_SCALE_1_8: return 8;
        default:                   return 1;
    }
}

static esp_err_t jpeg_open(struct epd_image_reader *r)
{
    // esp_jpeg takes a pointer, not a stream, so a file has to be read in whole first.
    if (r->src.fp != NULL) {
        const long size = src_size(&r->src);
        if (size <= 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((size_t)size > JPEG_MAX_FILE_BYTES) {
            ESP_LOGE(TAG, "JPEG is %ld bytes, over the %u limit", size,
                     (unsigned)JPEG_MAX_FILE_BYTES);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t *buf = psram_alloc((size_t)size);
        if (buf == NULL) {
            // NAMED, because this and the image buffer below both returned a bare
            // ESP_ERR_NO_MEM and the console could not say which had failed. Ticket 56 was
            // diagnosed against the wrong arithmetic for exactly that reason: a 4.29 MB file
            // whose thumbnail succeeded in the same capture cannot have failed here, and
            // there was no line to prove it either way.
            ESP_LOGE(TAG, "no %ld-byte PSRAM buffer for the file (psram_largest=%u)", size,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return ESP_ERR_NO_MEM;
        }
        if (!src_seek(&r->src, 0) || src_read(&r->src, buf, (size_t)size) != (size_t)size) {
            free(buf);
            return ESP_ERR_INVALID_SIZE;
        }
        r->src.owned = buf;
        r->src.data = buf;
        r->src.len = (size_t)size;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)r->src.data,
        .indata_size = (uint32_t)r->src.len,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};

    esp_err_t err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not a decodable JPEG: %s", esp_err_to_name(err));
        return err;
    }

    // The same predicate app_display.c turns the canvas with, on the same dimensions, so
    // the two cannot disagree about which fit this decode is for.
    int32_t fit_w = r->fit_w;
    int32_t fit_h = r->fit_h;
    if (r->fit_rotatable && epd_fit_wants_rotate(info.width, info.height, fit_w, fit_h)) {
        const int32_t t = fit_w;
        fit_w = fit_h;
        fit_h = t;
    }

    cfg.out_scale = choose_jpeg_scale(info.width, info.height, fit_w, fit_h);

    // Ask again at the chosen scale, and take the SIZE from output_len -- NOT from
    // width * height. esp_jpeg_get_image_info() applies out_scale to `output_len` alone;
    // `width` and `height` stay the raw SOF values (jpeg_decoder.c:153-159), and
    // esp_jpeg_decode() is the call that divides them (:107-108). The struct's own field
    // comments say "of the output image", which is true after the decode and false here;
    // jpeg_decoder.h:91 is the half of the documentation that is right, and it says
    // "Allocate a buffer of size img->output_len".
    //
    // Sizing this from width * height * 3 allocated a FULL-RESOLUTION image every time and
    // then had only the reduced one written into it: 36.58 MB for a 12 MP photograph
    // against ~6.36 MB of PSRAM, so NO phone photograph could be displayed at all -- a
    // 274 KB one failed exactly as a 4.29 MB one did. Ticket 46 has the seven arms.
    err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK) {
        return err;
    }

    // Exact rather than generous, and that is correct: esp_jpeg_decode() checks
    // `outsize <= outbuf_size` with outsize computed the same way, and writes at stride
    // `dec->width / scale_div`, so the last byte it touches is output_len - 1.
    size_t bytes = info.output_len;
    r->full_rgb = psram_alloc_image(bytes);

    // A SOFT PICTURE BEATS NONE, and the margin that decides it is ~220 KB. esp_jpeg takes a
    // pointer rather than a stream, so the whole FILE is resident in PSRAM at the same time as
    // the output buffer, and it is a prebuilt binary -- neither the residency nor the order is
    // available to change.
    //
    // **THE SWAP ABOVE IS WHAT MAKES THIS REACHABLE, which is ticket 56.** The panel is 400x600,
    // so a landscape photograph with auto_rotate on is fitted to 600x400 and `need` falls from
    // max(4032/400, 3024/600) = 10.08 to max(4032/600, 3024/400) = 7.56 -- across the 8.0
    // boundary, so the scale goes from 1/8 to 1/4 and the buffer from 504x378 (0.57 MB) to
    // 1008x756 (2.29 MB). For a 4,291,798-byte 12 MP photograph that is 6.58 MB against a
    // `total_free` of 6,423,180 and the panel showed nothing for 8.5 s. **The same file rendered
    // ESP_OK in 31.9 s at 1/8 twelve hours before that rotation shipped** (measurements.md), and
    // the capture that found the failure proves the read is affordable from the other side: the
    // THUMBNAIL of that very file succeeded in the same run, at 1/8.
    //
    // **Retried on the allocation actually failing, not on a prediction.** The alternative is
    // a predicate over free PSRAM, and it would have to model fragmentation across two large
    // contiguous blocks; the allocator is the device's own answer and costs one call.
    //
    // Display path only, via r->fit_step_down -- the field's comment says why the import path
    // must not take this branch. **The cost is one power of two of sharpness and it is smaller
    // than the fit box suggests**: epd_fit_centre() draws the 504x378 decode at 533x400, not at
    // the full 600x400, so the upscale is 1.058x -- measured from `region=400x533` on the glass
    // arm, against `region=400x300` and a 0.79x DOWNSCALE with the rotation off. 88.8 % of the
    // panel against 50.0 %, so the step-down's picture is 1.78x the area for 5.8 % of stretch.
    // Logged rather than silent -- a capture reads `err=ESP_OK` with a `step down` line beside it.
    while (r->full_rgb == NULL && r->fit_step_down && cfg.out_scale != JPEG_IMAGE_SCALE_1_8) {
        const unsigned refused = (unsigned)bytes;
        const int from = jpeg_scale_divisor(cfg.out_scale);
        // Before the retry allocation, or a success would report the heap it has just spent.
        const unsigned largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        cfg.out_scale = coarser_jpeg_scale(cfg.out_scale);
        err = esp_jpeg_get_image_info(&cfg, &info);
        if (err != ESP_OK) {
            return err;
        }
        bytes = info.output_len;
        ESP_LOGW(TAG, "step down 1/%d -> 1/%d: %u bytes refused (psram_largest=%u), taking %u",
                 from, jpeg_scale_divisor(cfg.out_scale), refused, largest, (unsigned)bytes);
        r->full_rgb = psram_alloc_image(bytes);
    }
    if (r->full_rgb == NULL) {
        ESP_LOGE(TAG, "no %u-byte image buffer at 1/%d (psram_largest=%u)", (unsigned)bytes,
                 jpeg_scale_divisor(cfg.out_scale),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    cfg.outbuf = r->full_rgb;
    cfg.outbuf_size = (uint32_t)bytes;
    err = esp_jpeg_decode(&cfg, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        return err;
    }

    r->width = info.width;
    r->height = info.height;
    return ESP_OK;
}

// ------------------------------------------------------------------------- open

static esp_err_t reader_finish_open(struct epd_image_reader *r)
{
    esp_err_t err;
    switch (r->format) {
        case EPD_IMAGE_PNG: err = png_open(r); break;
        case EPD_IMAGE_JPEG: err = jpeg_open(r); break;
        case EPD_IMAGE_BMP: err = bmp_open(r); break;
        default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        return err;
    }

    if (r->width <= 0 || r->height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    r->row = psram_alloc((size_t)r->width * 3u);
    return (r->row != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static struct epd_image_reader *reader_from(epd_image_reader_storage_t *storage)
{
    memset(storage, 0, sizeof(*storage));
    struct epd_image_reader *r = (struct epd_image_reader *)storage->opaque;
    // The panel unless a caller says otherwise, so a reader that never sets these behaves
    // exactly as it did before the fit became a parameter.
    r->fit_w = PANEL_W;
    r->fit_h = PANEL_H;
    return r;
}

esp_err_t epd_image_open_mem(epd_image_reader_storage_t *storage, const uint8_t *data,
                             size_t len, epd_image_reader_t **out)
{
    return epd_image_open_mem_fit(storage, data, len, PANEL_W, PANEL_H, out);
}

esp_err_t epd_image_open_mem_fit(epd_image_reader_storage_t *storage, const uint8_t *data,
                                 size_t len, int32_t fit_w, int32_t fit_h,
                                 epd_image_reader_t **out)
{
    if (storage == NULL || data == NULL || out == NULL || fit_w <= 0 || fit_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct epd_image_reader *r = reader_from(storage);
    r->fit_w = fit_w;
    r->fit_h = fit_h;
    r->src.data = data;
    r->src.len = len;
    r->format = epd_image_sniff(data, len);

    const esp_err_t err = reader_finish_open(r);
    if (err != ESP_OK) {
        epd_image_close(r);
        return err;
    }
    *out = r;
    return ESP_OK;
}

static esp_err_t open_file_common(epd_image_reader_storage_t *storage, const char *path,
                                  bool rotatable, bool step_down, epd_image_reader_t **out)
{
    if (storage == NULL || path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct epd_image_reader *r = reader_from(storage);
    r->fit_rotatable = rotatable;
    r->fit_step_down = step_down;
    r->src.fp = fopen(path, "rb");
    if (r->src.fp == NULL) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Sniff the content rather than trusting the extension: a file named .png that is
    // really a JPEG is common enough, and decoding by extension would fail confusingly.
    uint8_t magic[8] = {0};
    src_read(&r->src, magic, sizeof(magic));
    r->format = epd_image_sniff(magic, sizeof(magic));
    if (r->format == EPD_IMAGE_UNKNOWN) {
        r->format = epd_image_format_from_name(path);
    }

    const esp_err_t err = reader_finish_open(r);
    if (err != ESP_OK) {
        epd_image_close(r);
        return err;
    }
    *out = r;
    return ESP_OK;
}

esp_err_t epd_image_open_file(epd_image_reader_storage_t *storage, const char *path,
                              epd_image_reader_t **out)
{
    return open_file_common(storage, path, false, false, out);
}

esp_err_t epd_image_open_file_display(epd_image_reader_storage_t *storage, const char *path,
                                      bool rotatable, epd_image_reader_t **out)
{
    return open_file_common(storage, path, rotatable, true, out);
}

int32_t epd_image_width(const epd_image_reader_t *r) { return r ? r->width : 0; }
int32_t epd_image_height(const epd_image_reader_t *r) { return r ? r->height : 0; }
epd_image_format_t epd_image_format(const epd_image_reader_t *r)
{
    return r ? r->format : EPD_IMAGE_UNKNOWN;
}

esp_err_t epd_image_take_rgb(epd_image_reader_t *r, uint8_t **rgb, int32_t *w, int32_t *h)
{
    if (r == NULL || rgb == NULL || w == NULL || h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r->full_rgb == NULL) {
        return ESP_ERR_NOT_SUPPORTED; // a row-streaming reader has nothing whole to give
    }

    *rgb = r->full_rgb;
    *w = r->width;
    *h = r->height;

    // The reader no longer owns it, so epd_image_close() must not free it and
    // epd_image_next_row() must not read it. Ending the stream is the honest way to say
    // that: a caller that takes the buffer and then asks for rows has a bug, and this makes
    // it a clean ESP_ERR_NOT_FOUND rather than a read of freed memory.
    r->full_rgb = NULL;
    r->next_row = r->height;
    return ESP_OK;
}

esp_err_t epd_image_next_row(epd_image_reader_t *r, const uint8_t **rgb)
{
    if (r == NULL || rgb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (r->next_row >= r->height) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_OK;
    if (r->full_rgb != NULL) {
        // Already decoded whole; hand back a pointer into it.
        *rgb = r->full_rgb + (size_t)r->next_row * (size_t)r->width * 3u;
        r->next_row++;
        return ESP_OK;
    }

    switch (r->format) {
        case EPD_IMAGE_PNG: err = png_next_row(r); break;
        case EPD_IMAGE_BMP: err = bmp_next_row(r); break;
        default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        return err;
    }

    *rgb = r->row;
    r->next_row++;
    return ESP_OK;
}

void epd_image_close(epd_image_reader_t *r)
{
    if (r == NULL) {
        return;
    }
    if (r->png != NULL) {
        png_destroy_read_struct(&r->png, &r->png_info, NULL);
        r->png = NULL;
    }
    free(r->row);
    free(r->full_rgb);
    free(r->bmp_raw);
    free(r->src.owned);
    if (r->src.fp != NULL) {
        fclose(r->src.fp);
    }
    memset(r, 0, sizeof(*r));
}

// ------------------------------------------------------------------ draw to canvas

esp_err_t epd_image_draw_fit(epd_image_reader_t *r, epd_canvas_t *c, const epd_fit_t *fit)
{
    if (r == NULL || c == NULL || fit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fit->width <= 0 || fit->height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Matte first, so an image that does not fill the screen sits on white rather than
    // on whatever was drawn before (FR-5.3).
    epd_canvas_fill(c, 255, 255, 255);

    // One pass over the source. For each source row that arrives, emit every destination
    // row that samples it -- which is what lets the source stay unmaterialised.
    int32_t dy = 0;
    for (int32_t sy = 0; sy < r->height; sy++) {
        const uint8_t *row = NULL;
        const esp_err_t err = epd_image_next_row(r, &row);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "decode stopped at row %d of %d: %s", (int)sy, (int)r->height,
                     esp_err_to_name(err));
            return err;
        }

        while (dy < fit->height &&
               (int32_t)(((int64_t)dy * r->height) / fit->height) == sy) {
            for (int32_t dx = 0; dx < fit->width; dx++) {
                int32_t sx = (int32_t)(((int64_t)dx * r->width) / fit->width);
                if (sx >= r->width) {
                    sx = r->width - 1;
                }
                const uint8_t *p = &row[(size_t)sx * 3];
                epd_canvas_fill_rect(c, fit->x + dx, fit->y + dy, 1, 1, p[0], p[1], p[2]);
            }
            dy++;
        }
    }

    return ESP_OK;
}

esp_err_t epd_image_draw(epd_image_reader_t *r, epd_canvas_t *c)
{
    if (r == NULL || c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const epd_fit_t fit = epd_fit_centre(r->width, r->height, epd_canvas_logical_width(c),
                                         epd_canvas_logical_height(c));
    return epd_image_draw_fit(r, c, &fit);
}

esp_err_t epd_image_draw_mem(const uint8_t *data, size_t len, epd_canvas_t *c)
{
    epd_image_reader_storage_t storage;
    epd_image_reader_t *r = NULL;
    esp_err_t err = epd_image_open_mem(&storage, data, len, &r);
    if (err != ESP_OK) {
        return err;
    }
    err = epd_image_draw(r, c);
    epd_image_close(r);
    return err;
}

esp_err_t epd_image_draw_file(const char *path, epd_canvas_t *c)
{
    epd_image_reader_storage_t storage;
    epd_image_reader_t *r = NULL;
    esp_err_t err = epd_image_open_file(&storage, path, &r);
    if (err != ESP_OK) {
        return err;
    }
    err = epd_image_draw(r, c);
    epd_image_close(r);
    return err;
}
