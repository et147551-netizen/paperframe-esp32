#include "app_server.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include "img_resize.h"

#include "app_apply.h"
#include "app_auth.h"
#include "app_charge.h"
// GET /api/system/info reports whether the wall clock has been synced. Only frame_main.c links
// this file, and app_clock.c already had three callers in src/app/, so this adds nothing to the
// harness ELF -- the invariant app_clock.h asks to re-check.
#include "app_clock.h"
#include "app_display.h"
#include "app_heapwatch.h"
#include "app_maint.h"
#include "app_ota.h"
#include "epd_image.h"
// For the palette ids and their names; the API's `palette` field is that wire format.
#include "epd_canvas.h"  // EPD_CANVAS_ROTATION_MAX -- the wire's range is the mapping's range
#include "epd_dither.h"
// For EPD_WIDTH / EPD_HEIGHT: the `orientation` field's two words are derived from the
// panel's own shape, not assumed, and the page is told the size in pixels.
#include "epd_geom.h"
#include "app_settings.h"
#include "app_slideshow.h"
#include "app_smb_sync.h"
#include "app_gphotos_rules.h"
#include "app_gphotos_sync.h"
#include "assets_data.h"
#include "board.h"
#include "board_storage.h"
#include "board_usb_msc.h"
#include "board_wifi.h"
#include "photo_list.h"
#include "req_name.h"

static const char *TAG = "server";

#define BASE_PATH BOARD_STORAGE_MOUNT
#define PATH_BUF 160
#define PHOTOS_PER_PAGE 16
#define SCAN_ARENA_BYTES 16384
#define UPLOAD_TMP BASE_PATH "/.upload.tmp"

// Nothing here buffers the body, so the ceiling is about the volume rather than about
// RAM: the shipping firmware's 2 MB was a consequence of malloc(content_len), and a
// phone's 5 MB photograph has to be accepted (ticket 11). What must not happen is an
// upload that fills /data completely, so the real limit is the free space less a
// margin, and this is only a backstop against a client claiming an absurd length.
#define UPLOAD_MAX_SIZE (12 * 1024 * 1024)
#define UPLOAD_FREE_MARGIN (256 * 1024)

// How long a body may make no progress at all before the upload is abandoned.
//
// An IDLE budget, not a total one: a 12 MB photograph over Wi-Fi is 150 s of legitimate
// transfer, and a whole-body deadline would cut it. What is pathological is silence --
// a client that sent its headers and then stopped, which on an open AP (FR-2.1) is a
// phone walking out of range, not an attacker. Fifteen seconds is three of httpd's own
// 5 s recv ticks, so a client that is merely slow never reaches it.
//
// It bounds three things, and only the first is obvious. The handler holds
// board_storage_lock() across the whole parse, so the slideshow's next render
// (app_display.c:113) waits behind it. Handlers run in the single httpd task, so every
// other route waits too, storage or not. And app_smb_sync.c's window opens with
// app_server_stop(), which joins that task -- so without this the mirror stops as well.
#define UPLOAD_IDLE_MS 15000

static httpd_handle_t s_httpd;
static SemaphoreHandle_t s_state_mutex;
static int64_t s_last_activity_us = -1;

// The UI's two-step "pick a mode, then enter it" needs a requested mode that is not yet
// current. Only the current one is persisted (FR-8).
static char s_requested_mode[16];
static char s_conn_err[32];

// One arena, reused by every scan. Two handlers scanning at once would corrupt it, so
// both take this mutex -- httpd runs handlers on several sockets but one task, and
// relying on that would be relying on an implementation detail.
static SemaphoreHandle_t s_scan_mutex;
static char s_scan_arena[SCAN_ARENA_BYTES];
static photo_list_t s_scan_list;

static void mark_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

uint32_t app_server_ms_since_activity(void)
{
    if (s_last_activity_us < 0) {
        return UINT32_MAX;
    }
    return (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000);
}

static void state_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
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

// ------------------------------------------------------------------- responses

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (!s) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"oom\"}");
    }
    const esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    mark_activity();
    return err;
}

// Ticket 74. `err_code` and `data` are the machine-readable half of an error, beside the English
// `message`, and they exist because the web UI shows `message` VERBATIM -- so a page translated into
// Japanese still answered a refused upload in English.
//
// **This is opt-in per call site and deliberately not universal.** Of the 76 literal messages in
// this file, the great majority are terse strings a user is not expected to act on ("bad json",
// "write fail", "rename fail"); the ones that get a code are the ones somebody meets in normal use
// of the page and can do something about. A client that does not recognise a code falls back to
// `message`, so adding one to another route later changes nothing that exists.
//
// **A code is API surface and may not be reworded.** `message` stays free to change; the code is
// what the page's dictionary is keyed on (`err.<code>` in assets/index.html). Renaming one silently
// drops a translation in every language, which is exactly the failure ticket 74 section 3c rejected
// text-matching for.
//
// `data` carries the numbers a message interpolates -- a byte count, an attempt count -- because a
// translated sentence puts them in a different place. It is consumed here.
static esp_err_t send_error_full(httpd_req_t *req, int code, const char *err_code,
                                 const char *msg, cJSON *data)
{
    httpd_resp_set_status(req, code == 400   ? "400 Bad Request"
                               : code == 401 ? "401 Unauthorized"
                               : code == 403 ? "403 Forbidden"
                               : code == 404 ? "404 Not Found"
                               : code == 408 ? "408 Request Timeout"
                               // 413/415/503 were reaching this ladder and falling through to
                               // 500, which is how the thumbnail handler's own 415 "cannot
                               // decode" has always gone out as a 500 (found 2026-09-09 when a
                               // new 413 did the same). Anything not listed here is a 500, so a
                               // new code has to be added in both places.
                               : code == 413 ? "413 Content Too Large"
                               : code == 415 ? "415 Unsupported Media Type"
                               : code == 501 ? "501 Not Implemented"
                               : code == 503 ? "503 Service Unavailable"
                                             : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        // Nothing to add the fields to, and `data` would leak. The literal below is the same one
        // the print failure uses.
        cJSON_Delete(data);
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"oom\"}");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(r, "status", "error");
    cJSON_AddStringToObject(r, "message", msg);
    if (err_code) {
        cJSON_AddStringToObject(r, "code", err_code);
    }
    if (data) {
        // Attached only alongside a code: `data` on its own would be a field with nothing to say
        // which shape it has.
        if (err_code) {
            cJSON_AddItemToObject(r, "data", data);
        } else {
            cJSON_Delete(data);
        }
    }
    char *s = cJSON_PrintUnformatted(r);
    if (s) {
        httpd_resp_sendstr(req, s);
        free(s);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"oom\"}");
    }
    cJSON_Delete(r);
    return ESP_FAIL;
}

// The 56-odd terse developer messages keep using this, unchanged.
static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    return send_error_full(req, code, NULL, msg, NULL);
}

// A coded error with no numbers in it, which is most of the subset.
static esp_err_t send_error_code(httpd_req_t *req, int code, const char *err_code, const char *msg)
{
    return send_error_full(req, code, err_code, msg, NULL);
}

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".bmp") == 0) return "image/bmp";
    // The thumbnail sidecar the SMB import writes (app_smb_sync.c). It is a JPEG under a
    // name photo_list_is_image() must not accept, which is the whole reason for the
    // suffix -- so the extension cannot say what the bytes are and this has to.
    if (strcasecmp(dot, SMB_THUMB_SUFFIX) == 0) return "image/jpeg";
    if (strcasecmp(dot, ".html") == 0) return "text/html";
    return "application/octet-stream";
}

#define SEND_FILE_CHUNK 2048

// Read-then-send is bounded because PSRAM free is ~6.36 MB (measured 2026-09-08; the 7.5 MB
// this used to say was never measured) and UPLOAD_MAX_SIZE is 12 MB. Over the bound the
// response STREAMS with the bus held instead, so this is a demotion and not a refusal --
// which is why /thumb/, having no such fallback, must not share the number.
#define SEND_FILE_PSRAM_MAX (2u * 1024u * 1024u)

// heap_caps_malloc with a malloc fallback, as epd_image.c:104-111 does for the same
// reason: a small allocation can still legitimately come from internal RAM.
static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        p = malloc(n);
    }
    return p;
}

// `data`/`len` may be NULL/0. When they are not, the first bytes decide the type and the
// extension is only the fallback -- because the SMB import can leave JPEG bytes inside a
// `.png` name (app_smb_sync.c's store_photo(), which keeps the local name deliberately).
// The streaming branch below passes NULL: it is only reached over 2 MB, and a re-encoded
// photograph is never that, so the case cannot arise there.
static void set_file_headers(httpd_req_t *req, const char *path, const uint8_t *data,
                             size_t len)
{
    const char *type = mime_for(path);
    if (data != NULL && len > 0) {
        switch (epd_image_sniff(data, len)) {
        case EPD_IMAGE_JPEG: type = "image/jpeg"; break;
        case EPD_IMAGE_PNG:  type = "image/png";  break;
        case EPD_IMAGE_BMP:  type = "image/bmp";  break;
        default: break; // not an image, or too short to tell: the extension stands
        }
    }
    httpd_resp_set_type(req, type);
    // A day, and no ETag or Last-Modified with it. Both validators need stat(), which
    // costs 22-38 ms on this FATFS because it rescans the directory to resolve the name
    // (ticket 36) -- send_file() already pays that once inside fopen() and a validator
    // would pay it twice for nothing a plain max-age does not already give.
    //
    // The staleness this accepts: with smb_on_demand a name under /data comes and goes,
    // but re-fetching the same catalogue entry restores the same bytes. The reachable
    // failure is the documented catalogue ambiguity -- two share folders holding one
    // local name -- and its consequence is a wrong thumbnail in the management grid, never
    // a wrong photograph on the panel. **Upload names were NOT unique until 2026-09-23**: this
    // comment said they carried an epoch, but generate_next_photo_name() reused the lowest free
    // imaged###, so a photo uploaded after a delete took the deleted one's name and a browser
    // that had shown the old tile kept it for up to a day (reproduced in Chrome: 8,808 B cached
    // against 8,017 B served). A deleted number is no longer reused below 999 (.imgseq); past it
    // the fallback reuses again, and the page's "Clear thumbnail cache" button is the remedy for
    // that and for anything cached before the fix -- a new ?v= that request_file_name() ignores.
    //
    // `private` because the API is token-authenticated by cookie or ?t=: no shared proxy
    // may hold these.
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
}

// Sends bytes already in memory, in 2 KB chunks. No lock is held, which is the point.
static esp_err_t send_bytes(httpd_req_t *req, const uint8_t *data, size_t len)
{
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < len && err == ESP_OK; off += SEND_FILE_CHUNK) {
        const size_t n = (len - off < SEND_FILE_CHUNK) ? (len - off) : SEND_FILE_CHUNK;
        err = httpd_resp_send_chunk(req, (const char *)data + off, n);
    }
    if (err != ESP_OK) {
        return err;
    }
    mark_activity();
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Captures the body into PSRAM under one storage lock and sends it after releasing that
// lock. Both halves of that matter, and the second is why it is not the obvious stream:
//
//   * The whole read still happens inside one lock, so a concurrent delete cannot
//     truncate the response. Capturing the bytes BEFORE the lock drops is stronger than
//     streaming under it, not weaker.
//   * httpd_resp_send_chunk() blocks on socket send, and board_storage_lock() takes
//     board_spi_lock() underneath whenever the medium is the microSD
//     (board_storage.c:99-102). Streaming under the lock therefore let a slow HTTP client
//     pace the SPI bus, and the refresh path waits on that bus with portMAX_DELAY
//     (epd_el040ef1.c:502) -- so a client could stall the panel for as long as it liked.
//     The direction ticket 03 designed for, a refresh making file routes wait, is
//     intended; this one was not.
//
// The lock is still NOT held across a refresh, which would deadlock (board_spi.h).
static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(path, "rb");
    if (!f) {
        board_storage_unlock();
        return send_error(req, 404, "not found");
    }

    // ftell/fseek rather than stat(), for the 38.3 ms reason above. epd_image.c's
    // src_size() (:60-72) does the same thing for the same reason.
    uint8_t *whole = NULL;
    size_t whole_len = 0;
    bool read_failed = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long end = ftell(f);
        rewind(f);
        if (end > 0 && (size_t)end <= SEND_FILE_PSRAM_MAX) {
            uint8_t *buf = psram_alloc((size_t)end);
            if (buf != NULL) {
                if (fread(buf, 1, (size_t)end, f) == (size_t)end) {
                    whole = buf;
                    whole_len = (size_t)end;
                } else {
                    free(buf);
                    read_failed = true;
                }
            } else {
                // Under the bound and still no buffer. Worth a line rather than a silent
                // demotion, because the consequence is that this response streams with the
                // SPI bus held -- the behaviour the bound exists to avoid.
                ESP_LOGW(TAG, "send_file: no %ld-byte buffer, streaming with the bus held "
                              "(psram_largest=%u)", end,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            }
        } else if (end > (long)SEND_FILE_PSRAM_MAX) {
            ESP_LOGW(TAG, "send_file: %ld bytes is over the %u bound, streaming with the "
                          "bus held", end, (unsigned)SEND_FILE_PSRAM_MAX);
        }
    }

    if (whole != NULL || read_failed) {
        fclose(f);
        board_storage_unlock();
        if (read_failed) {
            return send_error(req, 500, "read failed");
        }
        set_file_headers(req, path, whole, whole_len);
        const esp_err_t err = send_bytes(req, whole, whole_len);
        free(whole);
        return err;
    }

    // Over SEND_FILE_PSRAM_MAX, or PSRAM would not give it. Stream with the lock held --
    // the old behaviour, deliberately unchanged, because this is the rare large upload
    // rather than anything the photo grid asks for. rewind() above left the position at 0.
    set_file_headers(req, path, NULL, 0);
    char buf[SEND_FILE_CHUNK];
    esp_err_t err = ESP_OK;
    for (;;) {
        const size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
            err = httpd_resp_send_chunk(req, buf, n);
            if (err != ESP_OK) {
                break;
            }
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                err = ESP_FAIL;
            }
            break;
        }
    }
    fclose(f);
    board_storage_unlock();
    if (err != ESP_OK) {
        return err;
    }
    mark_activity();
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ------------------------------------------------------------------- url coding
//
// Percent-coding and the path-traversal guard are src/core/req_name.[ch] since ticket 79 -- pure
// string code, and it was the one piece of request handling `pio test -e native` could not reach
// while it lived in this file. The behaviour is unchanged; what moved is where it can be tested.
// The ORDER is the property: decode, THEN check. See request_file_name() below.

// ------------------------------------------------------------------ json body

// Reads a small JSON body. Anything larger than `size` is a client that is not the UI.
static cJSON *read_json_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len == 0 || req->content_len >= size) {
        return NULL;
    }
    size_t off = 0;
    while (off < req->content_len) {
        const int n = httpd_req_recv(req, buf + off, req->content_len - off);
        if (n <= 0) {
            return NULL;
        }
        off += (size_t)n;
    }
    buf[off] = '\0';
    return cJSON_Parse(buf);
}

// ---------------------------------------------------------------- photo naming

// The highest number each prefix has ever been given on THIS card, as text `<d> <N>\n`. On the
// card rather than in NVS because the names it protects are the card's: a card moved to another
// frame keeps its sequence. Written with plain fopen, not board_storage_write_atomic(), because
// the caller holds the storage lock that helper would take; a torn file fails the sscanf and the
// sequence falls back to the highest name present, which is the old behaviour and no worse.
#define PHOTO_SEQ_PATH BASE_PATH "/.imgseq"

// imaged001.png / imageN001.png -- the prefix carries the upload's algorithm choice and
// the slideshow reads it back at display time (FR-3.3, app_server.cpp:276-278).
// Called with the storage lock held.
//
// **A deleted number is NOT reused** (2026-09-23). This used to take the lowest free number, so
// an upload after a delete took the deleted photo's name, and a browser that had shown the old
// tile kept it for a day under the same /thumb/ URL (reproduced in Chrome: 8,808 B cached against
// 8,017 B served). Now it is one past the highest ever given, which .imgseq remembers across the
// delete of the newest photo -- the common case, and the one "highest present + 1" alone would
// still get wrong. Only past 999 does it fall back to the lowest free number.
static esp_err_t generate_next_photo_name(const char *ext, const char *algorithm, char *out,
                                          size_t size)
{
    const bool nearest = (algorithm && strcmp(algorithm, "nearest") == 0);
    const char prefix = nearest ? 'N' : 'd';
    static bool used[1000];
    memset(used, 0, sizeof(used));

    unsigned seq[2] = {0, 0}; // [0] 'd', [1] 'N'
    FILE *sf = fopen(PHOTO_SEQ_PATH, "rb");
    if (sf) {
        if (fscanf(sf, "%u %u", &seq[0], &seq[1]) != 2 || seq[0] > 999 || seq[1] > 999) {
            seq[0] = seq[1] = 0;
        }
        fclose(sf);
    }
    unsigned high = seq[nearest ? 1 : 0];

    DIR *d = opendir(BASE_PATH);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            unsigned n;
            char pf;
            if (sscanf(e->d_name, "image%c%3u", &pf, &n) == 2 && pf == prefix && n < 1000) {
                used[n] = true;
                if (n > high) {
                    high = n;
                }
            }
        }
        closedir(d);
    }

    unsigned pick = 0;
    if (high < 999) {
        pick = high + 1;
    } else {
        for (unsigned n = 1; n <= 999 && pick == 0; n++) {
            if (!used[n]) {
                pick = n;
            }
        }
    }
    if (pick == 0) {
        return ESP_FAIL;
    }
    // Recorded before the rename that uses it. A failed upload then burns a number, which costs
    // nothing; recording after would let a crash between the two hand the same number out twice.
    seq[nearest ? 1 : 0] = pick;
    sf = fopen(PHOTO_SEQ_PATH, "wb");
    if (sf) {
        fprintf(sf, "%u %u\n", seq[0], seq[1]);
        fclose(sf);
    }
    snprintf(out, size, "image%c%03u.%s", prefix, pick, ext);
    return ESP_OK;
}

// --------------------------------------------------------- multipart streaming
//
// The body is NOT buffered. A phone camera's JPEG is several megabytes and the canvas
// already holds 720 KB of PSRAM; the reference gets away with malloc(content_len)
// because the browser only ever sends it a 400x600 PNG, and files arriving from any
// other client would not be so kind.
//
// The file part comes first in the UI's FormData (index.html:3276-3279), so the
// algorithm and action fields are not known until after the bytes are written. The file
// therefore streams to UPLOAD_TMP and is renamed once the trailing fields have been
// read.

#define STREAM_BUF 2048

typedef struct {
    httpd_req_t *req;
    uint8_t buf[STREAM_BUF];
    size_t len;
    size_t remaining;  // still to come from the socket
    bool error;
    bool timed_out;          // error was UPLOAD_IDLE_MS of silence, not a dead socket
    int64_t idle_deadline_us;  // pushed out by every byte that arrives
} body_stream_t;

static void bs_init(body_stream_t *b, httpd_req_t *req)
{
    memset(b, 0, sizeof(*b));
    b->req = req;
    b->remaining = req->content_len;
    b->idle_deadline_us = esp_timer_get_time() + (int64_t)UPLOAD_IDLE_MS * 1000;
}

// Tops the buffer up. Returns false only on a socket error; a short read at the end of
// the body is normal.
static bool bs_fill(body_stream_t *b)
{
    while (b->len < sizeof(b->buf) && b->remaining > 0) {
        size_t want = sizeof(b->buf) - b->len;
        if (want > b->remaining) {
            want = b->remaining;
        }
        const int n = httpd_req_recv(b->req, (char *)b->buf + b->len, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            // The retry used to be unconditional, which made a client that stopped
            // sending -- without closing -- a permanent hold on the storage lock and the
            // httpd task. Measured 2026-09-05: every route stopped answering while ICMP
            // and the heartbeat stayed healthy. Ticket 26.
            if (esp_timer_get_time() > b->idle_deadline_us) {
                b->timed_out = true;
                b->error = true;
                return false;
            }
            continue;
        }
        if (n <= 0) {
            b->error = true;
            return false;
        }
        b->len += (size_t)n;
        b->remaining -= (size_t)n;
        b->idle_deadline_us = esp_timer_get_time() + (int64_t)UPLOAD_IDLE_MS * 1000;
        // Ticket 59: mark_activity() otherwise fires only when a response is sent, so a
        // body still streaming in looks idle to the mirror -- which then opens a window on
        // top of it and kills the client after 130,826 bytes. Stamped on bytes ARRIVING,
        // not on being inside the handler, so the ticket 26 stall above stops counting as
        // activity and does not hold the mirror off for the whole idle timeout.
        mark_activity();
    }
    return true;
}

static void bs_consume(body_stream_t *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->buf, b->buf + n, b->len - n);
    b->len -= n;
}

static const uint8_t *mem_find(const uint8_t *hay, size_t hlen, const void *needle, size_t nlen)
{
    if (nlen == 0 || nlen > hlen) {
        return NULL;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

typedef struct {
    char filename[128];
    char action[32];
    char algorithm[16];
    size_t file_bytes;
    bool have_file;
} upload_result_t;

// Copies a part's Content-Disposition field value out of the header block.
//
// The match must start at a delimiter, because `filename="` CONTAINS `name="`. RFC 7578
// lets the parameters come in either order, so a client that sends
// `Content-Disposition: form-data; filename="x.png"; name="file"` would otherwise have
// its filename read as the field name and its file part silently ignored.
static void header_param(const uint8_t *hdr, size_t hlen, const char *key, char *out, size_t size)
{
    out[0] = '\0';
    const size_t klen = strlen(key);

    const uint8_t *p = NULL;
    for (const uint8_t *cur = hdr; cur < hdr + hlen; cur++) {
        const uint8_t *hit = mem_find(cur, (size_t)(hdr + hlen - cur), key, klen);
        if (!hit) {
            break;
        }
        const bool at_delimiter = hit == hdr || hit[-1] == ' ' || hit[-1] == ';';
        if (at_delimiter) {
            p = hit;
            break;
        }
        cur = hit;  // skip this occurrence and keep looking
    }
    if (!p) {
        return;
    }
    p += klen;
    const uint8_t *end = memchr(p, '"', (size_t)(hdr + hlen - p));
    if (!end) {
        return;
    }
    size_t n = (size_t)(end - p);
    if (n >= size) {
        n = size - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

// Walks the parts. The file part is written to `fp`; short fields are kept.
static esp_err_t parse_multipart(body_stream_t *b, const char *boundary, FILE *fp,
                                 upload_result_t *res)
{
    char sep[140];
    const int sep_len = snprintf(sep, sizeof(sep), "\r\n--%s", boundary);
    if (sep_len <= 0 || sep_len >= (int)sizeof(sep)) {
        return ESP_ERR_INVALID_ARG;
    }

    // The first boundary has no leading CRLF. Prepending one lets the same separator
    // find every part including the first.
    if (!bs_fill(b)) {
        return ESP_FAIL;
    }
    const uint8_t *first = mem_find(b->buf, b->len, sep + 2, (size_t)sep_len - 2);
    if (!first) {
        return ESP_ERR_INVALID_ARG;
    }
    bs_consume(b, (size_t)(first - b->buf) + (size_t)sep_len - 2);

    for (;;) {
        if (!bs_fill(b)) {
            return ESP_FAIL;
        }
        if (b->len >= 2 && b->buf[0] == '-' && b->buf[1] == '-') {
            return ESP_OK;  // closing boundary
        }
        if (b->len >= 2 && b->buf[0] == '\r' && b->buf[1] == '\n') {
            bs_consume(b, 2);
        }
        if (!bs_fill(b)) {
            return ESP_FAIL;
        }

        const uint8_t *hdr_end = mem_find(b->buf, b->len, "\r\n\r\n", 4);
        if (!hdr_end) {
            return ESP_ERR_INVALID_ARG;  // a header block larger than the buffer
        }
        const size_t hlen = (size_t)(hdr_end - b->buf);

        char name[32];
        header_param(b->buf, hlen, "name=\"", name, sizeof(name));
        const bool is_file = strcmp(name, "file") == 0;
        if (is_file) {
            header_param(b->buf, hlen, "filename=\"", res->filename, sizeof(res->filename));
        }
        char *field = NULL;
        size_t field_size = 0;
        if (strcmp(name, "action") == 0) {
            field = res->action;
            field_size = sizeof(res->action);
        } else if (strcmp(name, "algorithm") == 0) {
            field = res->algorithm;
            field_size = sizeof(res->algorithm);
        }
        size_t field_used = 0;

        bs_consume(b, hlen + 4);

        // Body of this part, up to the next separator. Anything not yet proven to be
        // outside the separator is kept, so a boundary split across two reads is found.
        for (;;) {
            if (!bs_fill(b)) {
                return ESP_FAIL;
            }
            const uint8_t *hit = mem_find(b->buf, b->len, sep, (size_t)sep_len);
            const size_t take = hit ? (size_t)(hit - b->buf)
                                    : (b->len > (size_t)sep_len ? b->len - (size_t)sep_len : 0);

            if (take > 0) {
                if (is_file) {
                    if (fwrite(b->buf, 1, take, fp) != take) {
                        return ESP_ERR_NO_MEM;  // out of space on /data
                    }
                    res->file_bytes += take;
                    res->have_file = true;
                } else if (field && field_used + 1 < field_size) {
                    size_t n = take;
                    if (field_used + n >= field_size) {
                        n = field_size - field_used - 1;
                    }
                    memcpy(field + field_used, b->buf, n);
                    field_used += n;
                    field[field_used] = '\0';
                }
                bs_consume(b, take);
            }

            if (hit) {
                bs_consume(b, (size_t)sep_len);
                break;
            }
            if (b->remaining == 0 && b->len <= (size_t)sep_len) {
                return ESP_ERR_INVALID_ARG;  // ran out before the closing boundary
            }
        }
    }
}

// ---------------------------------------------------------------- request guards
//
// Ticket 29. GUARD() applies two of these -- the Host check and the token -- to every
// handler except the one that serves the page itself. The third, request_from_ap(), is
// narrower and belongs to the SMB routes alone, which is why it is not in GUARD().
// Separate functions rather than one because they answer different questions and fail
// with different status codes.
//
// **guard(req) is the whole access control, and it is opt-in per handler.** There is no
// middleware hook in esp_http_server -- httpd_config_t has uri_match_fn and an error
// handler, neither of which can refuse a request before routing -- so the check is one
// line at the top of each handler and a route that forgets it is silently open. That is
// why ticket 29's acceptance sweeps all 22 routes rather than a sample of them.

#define COOKIE_NAME "pct"

// The token, from whichever of the three places carried it.
//
// A cookie first, because that is what the browser actually uses: the photo grid loads
// thumbnails as plain <img src> tags (assets/index.html:3465), which cannot carry a
// custom header, and fetch() sends cookies same-origin by default. The other two exist
// for clients that are not that browser -- curl on the bench, and a captive-portal
// mini-browser that may not keep cookies at all.
static bool auth_ok(httpd_req_t *req)
{
    char val[APP_AUTH_TOKEN_HEX_SIZE];

    size_t len = sizeof(val);
    if (httpd_req_get_cookie_val(req, COOKIE_NAME, val, &len) == ESP_OK && app_auth_check(val)) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Frame-Token", val, sizeof(val)) == ESP_OK &&
        app_auth_check(val)) {
        return true;
    }

    // ?t=, which is how the pairing QR hands the token to a browser that has never seen
    // this device. Accepted on any route rather than only on "/", because the QR's URL
    // may be opened at a deep link and because a client that cannot keep a cookie has
    // nothing else.
    // Sized for a whole query string, not for the token. It used to be
    // APP_AUTH_TOKEN_HEX_SIZE + 8, which fits "t=<32 hex>" and nothing else:
    // httpd_req_get_url_query_str() returns ESP_ERR_HTTPD_RESULT_TRUNC rather than ESP_OK
    // when the query does not fit, so ANY second parameter turned a valid token into a 401.
    // Found on 2026-09-06 by `GET /api/photos/list?t=<token>&per_page=500`, which is the
    // paged photo grid -- exactly what a pairing-QR client with no cookie would ask for, and
    // the one client for which ?t= is the only way in. A 401 that means "your query was too
    // long" is indistinguishable from one that means "your token is wrong".
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "t", val, sizeof(val)) == ESP_OK && app_auth_check(val)) {
        return true;
    }
    return false;
}

// True when this request arrived over the softAP rather than the station link.
//
// This is what station_is_up() was reaching for and did not implement: that function
// asked whether THE FRAME had a station link, which is true whenever the frame is on the
// house LAN -- so a client on the open AP passed a guard written to keep it out. The
// question the guard wants answered is about the client, and the socket knows: the local
// address of a connection accepted on the AP netif is the AP's own address.
// The tri-state core, because the two callers below need OPPOSITE biases for the case where the
// answer cannot be had. See each of them.
//
//   1 = the AP, 0 = the station link, -1 = cannot tell.
static int request_iface(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_storage local;
    socklen_t slen = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &slen) != 0) {
        return -1;
    }

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if (!ap || esp_netif_get_ip_info(ap, &ip) != ESP_OK) {
        return -1;
    }

    uint32_t addr = 0;
    if (local.ss_family == AF_INET) {
        addr = ((const struct sockaddr_in *)(const void *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        // httpd's listener is dual-stack here, so an IPv4 client arrives as the mapped
        // address ::ffff:a.b.c.d and the four bytes wanted are s6_addr[12..15].
        //
        // **They are not the last four bytes of the struct.** The first version of this
        // function read `(uint8_t *)&local + sizeof(local) - 4`, on a comment claiming
        // the address was at the end "either way" -- that is sin6_scope_id, and the
        // function silently returned false for every AP client. It passed review, built
        // clean, and was only caught when the AP was finally reachable and
        // POST /api/smb/sync answered 200 from a client that should have had 403.
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)&local;
        memcpy(&addr, &s6->sin6_addr.s6_addr[12], sizeof(addr));
    } else {
        return -1;
    }
    const bool from_ap = (addr == ip.ip.addr);
    if (from_ap) {
        // Raw addresses rather than "refused", because this function has already been
        // wrong once in a way that looked exactly like working code, and because only
        // two routes call it -- so the line is rare enough to be worth its space. It
        // fires on the AP side only; a station request is the silent case.
        printf("# req_from_ap fam=%d local=%08lx ap=%08lx\n", (int)local.ss_family,
               (unsigned long)addr, (unsigned long)ip.ip.addr);
    }
    return from_ap ? 1 : 0;
}

// For a REFUSAL: an answer that cannot be had counts as the AP, which is the untrusted side. This
// is the original function, and its two SMB callers are unchanged by the split above.
static bool request_from_ap(httpd_req_t *req)
{
    return request_iface(req) != 0;
}

// For a GRANT: only a definite AP answer will do, so the bias goes the other way.
//
// Its caller is the captive-portal redirect, which puts the API token in a Location header --
// "cannot tell" must not mean "hand it over". Calling request_from_ap() there would have done
// exactly that, because true is its permissive answer and this is the one place in the file where
// true would be the permissive one.
static bool request_certainly_from_ap(httpd_req_t *req)
{
    return request_iface(req) == 1;
}

// The other end of the socket, printed as an address and a port. Shares the v4-mapped
// unpacking of request_from_ap() -- see the note there about where the four bytes are.
static void log_peer(httpd_req_t *req, const char *what)
{
    const int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage peer;
    socklen_t slen = sizeof(peer);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&peer, &slen) != 0) {
        printf("# %s from <unknown peer> %s\n", what, req->uri);
        return;
    }
    uint32_t addr = 0;
    uint16_t port = 0;
    if (peer.ss_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)(const void *)&peer;
        addr = s4->sin_addr.s_addr;
        port = ntohs(s4->sin_port);
    } else {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)(const void *)&peer;
        memcpy(&addr, &s6->sin6_addr.s6_addr[12], sizeof(addr));
        port = ntohs(s6->sin6_port);
    }
    const uint8_t *b = (const uint8_t *)&addr;
    printf("# %s from %u.%u.%u.%u:%u %s\n", what, b[0], b[1], b[2], b[3], (unsigned)port,
           req->uri);
    fflush(stdout);
}

// Rejects a Host header that is not one of ours, which is what stops DNS rebinding: a
// browser tricked into resolving evil.example to this device still sends
// "Host: evil.example", and no amount of same-origin policy helps because to the browser
// it IS the same origin.
//
// **Only /api/* and /data/* may use this.** The captive portal of FR-2.2 works precisely
// by answering requests addressed to somebody else -- connectivitycheck.gstatic.com and
// captive.apple.com -- so applying it to the "/*" wildcard would break the flow that
// gets a phone onto the page in the first place.
static bool host_ok(httpd_req_t *req)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return true;  // HTTP/1.0 clients need not send one; curl and browsers always do
    }
    // Port suffix is not part of the name.
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }

    board_wifi_status_t st;
    board_wifi_status(&st);
    if (st.ap_ip[0] && strcmp(host, st.ap_ip) == 0) {
        return true;
    }
    if (st.sta_connected && st.ip[0] && strcmp(host, st.ip) == 0) {
        return true;
    }

    char name[APP_SETTINGS_NAME_SIZE + 8];
    char device[APP_SETTINGS_NAME_SIZE];
    app_settings_device_name(device, sizeof(device));
    snprintf(name, sizeof(name), "%s.local", device);
    return strcasecmp(host, name) == 0 || strcasecmp(host, device) == 0;
}

// The Host check on its own, with its log line.
//
// Split out for POST /api/auth/pair (ticket 66), which is the one API route that must NOT require
// a token -- it is how a browser with no camera gets one -- but which still needs the rebinding
// defence, because it answers with a Set-Cookie. One producer for the log line, because that line
// has already been the whole content of one finding.
static bool host_guard_ok(httpd_req_t *req, int *code, const char **msg)
{
    if (host_ok(req)) {
        return true;
    }
    // Logged for the same reason as the 401 below, and learned the hard way: ticket 30's
    // captive-portal defect is a 403 storm -- the portal serves the page as
    // connectivitycheck.gstatic.com and every relative API call the page makes is refused
    // here -- and on 2026-09-09 the console showed 64 anonymous
    // "httpd_uri: uri handler execution failed" lines and not one word about the host,
    // because only the auth branch said anything. The Host is printed because the whole
    // point is WHICH name was asked for.
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        host[0] = '\0';
    }
    printf("# 403 bad host \"%s\" %s\n", host, req->uri);
    fflush(stdout);
    *code = 403;
    *msg = "bad host";
    return false;
}

// Both checks in the order they should be answered: a request addressed to the wrong
// host is refused before its token is even looked at, so a rebinding attempt learns
// nothing about whether the token it guessed was right.
//
// Split into a decision and a send so the GUARD() macro below stays a single statement
// that returns; a version returning esp_err_t would need a sentinel for "not refused".
static bool guard_ok(httpd_req_t *req, int *code, const char **msg)
{
    if (!host_guard_ok(req, code, msg)) {
        return false;
    }
    if (!auth_ok(req)) {
        // Who was refused, not just that somebody was. Ticket 28 is stuck on identifying
        // a client that requests from this frame every ~10 s and has never been named;
        // since a rejected request does not call mark_activity(), that client is now
        // invisible to idle_ms as well, and this line is the only thing that can still
        // see it. Rate is not a concern -- a paired browser never reaches here.
        log_peer(req, "401");
        *code = 401;
        *msg = "unauthorized";
        return false;
    }
    return true;
}

#define GUARD(req)                                            \
    do {                                                      \
        int guard_code_ = 0;                                  \
        const char *guard_msg_ = NULL;                        \
        if (!guard_ok((req), &guard_code_, &guard_msg_)) {    \
            return send_error((req), guard_code_, guard_msg_); \
        }                                                     \
    } while (0)

// The Host check WITHOUT the token, for the one route that hands a token out. Ticket 66: exactly
// one handler in this file uses it, and it says so -- a second would be an unauthenticated API
// route, which is what ticket 29 exists to prevent.
#define HOST_GUARD(req)                                            \
    do {                                                           \
        int guard_code_ = 0;                                       \
        const char *guard_msg_ = NULL;                             \
        if (!host_guard_ok((req), &guard_code_, &guard_msg_)) {     \
            return send_error((req), guard_code_, guard_msg_);      \
        }                                                          \
    } while (0)

// Writes the Set-Cookie header into `buf`, which the CALLER owns until its response is sent.
//
// **httpd_resp_set_hdr() stores the pointer rather than copying**, so the buffer has to outlive the
// send -- which is why this takes one instead of holding a static. Extracted for ticket 66 so the
// pairing route and the ?t= path cannot drift apart on the flags: two places setting the same
// cookie with different SameSite is a bug nobody would see until a browser did something odd.
//
// No Secure flag: there is no TLS here and a Secure cookie would simply never be sent.
// SameSite=Strict is what makes this cookie useless to a cross-site request, which is the other
// half of the rebinding defence in host_ok().
#define TOKEN_COOKIE_BUF (APP_AUTH_TOKEN_HEX_SIZE + 96)

static void set_token_cookie(httpd_req_t *req, const char *token, char *buf, size_t size)
{
    snprintf(buf, size, COOKIE_NAME "=%s; Max-Age=31536000; Path=/; SameSite=Strict; HttpOnly",
             token);
    httpd_resp_set_hdr(req, "Set-Cookie", buf);
}

// ------------------------------------------------------------------- handlers

static esp_err_t h_get_modes(httpd_req_t *req)
{
    GUARD(req);

    // mode_1 only, and now it is the only mode that exists anywhere: ticket 22 cut the
    // Ezdata card, panel and polling out of index.html as well (2026-09-03).
    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "modes");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "id", APP_SETTINGS_MODE_LOCAL);
    cJSON_AddStringToObject(m, "name", "Local Photo Album");
    cJSON_AddItemToArray(a, m);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    GUARD(req);

    static board_wifi_network_t nets[24];
    size_t n = 0;
    board_wifi_scan(nets, sizeof(nets) / sizeof(nets[0]), &n);

    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "networks");
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", nets[i].rssi);
        cJSON_AddBoolToObject(e, "secure", nets[i].secure);
        cJSON_AddItemToArray(a, e);
    }
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_wifi_config(httpd_req_t *req)
{
    GUARD(req);

    char body[512];
    cJSON *j = read_json_body(req, body, sizeof(body));
    if (!j) {
        return send_error(req, 400, "bad json");
    }

    cJSON *ssid_j = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(j, "password");
    cJSON *mode_j = cJSON_GetObjectItem(j, "mode");
    cJSON *boot_j = cJSON_GetObjectItem(j, "boot_sound");
    cJSON *name_j = cJSON_GetObjectItem(j, "device_name");

    const bool has_ssid = cJSON_IsString(ssid_j) && ssid_j->valuestring;
    const bool has_boot = cJSON_IsBool(boot_j);
    const bool has_name = cJSON_IsString(name_j) && name_j->valuestring;
    const bool has_mode = cJSON_IsString(mode_j) && mode_j->valuestring && mode_j->valuestring[0];

    if (!has_ssid && !has_boot && !has_name && !has_mode) {
        cJSON_Delete(j);
        return send_error(req, 400, "ssid required");
    }

    if (has_mode) {
        // Through the normaliser rather than straight in: /api/wifi/status echoes this
        // back, and the raw copy let a client put arbitrary 15-character text there.
        // An unknown mode lands as "" -- the same "no mode selected" the setter uses.
        char mode[sizeof(s_requested_mode)];
        app_settings_normalize_mode_id(mode_j->valuestring, mode, sizeof(mode));
        state_lock();
        copy_str(s_requested_mode, mode, sizeof(s_requested_mode));
        state_unlock();
    }

    if (has_ssid) {
        char pass[APP_SETTINGS_PASS_SIZE];
        if (cJSON_IsString(pass_j) && pass_j->valuestring && pass_j->valuestring[0]) {
            copy_str(pass, pass_j->valuestring, sizeof(pass));
        } else {
            // An empty password field means "keep the saved one", which is how the UI
            // reconnects to a network it already knows -- and it is only meaningful for
            // the SAME network. Reusing the stored key against an SSID somebody else
            // chose hands it to them: WPA2's PMK is salted with the SSID, so an attacker
            // in range who names an access point to match captures a handshake they can
            // take away and grind against offline. A different SSID must arrive with its
            // own password (ticket 29, F3).
            char known[APP_SETTINGS_SSID_SIZE];
            app_settings_wifi_ssid(known, sizeof(known));
            if (known[0] == '\0' || strcmp(known, ssid_j->valuestring) != 0) {
                cJSON_Delete(j);
                return send_error_code(req, 400, "wifi.password_required",
                                       "password required for a new network");
            }
            app_settings_wifi_password(pass, sizeof(pass));
        }
        const esp_err_t serr = app_settings_set_wifi(ssid_j->valuestring, pass);
        if (serr != ESP_OK) {
            cJSON_Delete(j);
            return send_error_code(req, 400, "wifi.invalid_credentials", "invalid credentials");
        }
        state_lock();
        copy_str(s_conn_err, "connecting", sizeof(s_conn_err));
        state_unlock();
        // Queues the connect rather than performing it, so the reply below is sent before
        // the station link goes away -- board_wifi.h:58 has the account.
        board_wifi_connect(ssid_j->valuestring, pass);
    }

    if (has_boot) {
        app_settings_set_boot_sound(cJSON_IsTrue(boot_j));
    }
    if (has_name) {
        if (app_settings_set_device_name(name_j->valuestring) == ESP_OK) {
            char applied[APP_SETTINGS_NAME_SIZE];
            app_settings_device_name(applied, sizeof(applied));
            board_wifi_set_hostname(applied);
        }
    }
    if (has_mode) {
        const esp_err_t merr = app_settings_set_current_mode(mode_j->valuestring);
        if (merr != ESP_OK) {
            cJSON_Delete(j);
            return send_error(req, 400, "invalid mode");
        }
    }
    cJSON_Delete(j);

    app_settings_t s;
    app_settings_get(&s);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddBoolToObject(r, "boot_sound", s.boot_sound);
    cJSON_AddStringToObject(r, "device_name", s.device_name);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// The station state the UI shows, assembled from the driver rather than remembered
// here: a cached "connected" that disagrees with the radio is the classic way this
// screen lies.
static void wifi_state_snapshot(board_wifi_status_t *st, char *conn_err, size_t err_size,
                                char *current_mode, size_t mode_size, char *requested_mode,
                                size_t req_size)
{
    board_wifi_status(st);

    state_lock();
    if (st->sta_connected) {
        s_conn_err[0] = '\0';
    } else if (st->last_error[0] && strcmp(s_conn_err, "connecting") != 0) {
        copy_str(s_conn_err, st->last_error, sizeof(s_conn_err));
    }
    copy_str(conn_err, s_conn_err, err_size);
    copy_str(requested_mode, s_requested_mode, req_size);
    state_unlock();

    app_settings_current_mode(current_mode, mode_size);
}

static esp_err_t h_wifi_status(httpd_req_t *req)
{
    GUARD(req);

    board_wifi_status_t st;
    char conn_err[32], current_mode[16], requested_mode[16];
    wifi_state_snapshot(&st, conn_err, sizeof(conn_err), current_mode, sizeof(current_mode),
                        requested_mode, sizeof(requested_mode));

    app_settings_t s;
    app_settings_get(&s);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "connected", st.sta_connected);
    cJSON_AddStringToObject(r, "mode", current_mode);
    cJSON_AddStringToObject(r, "requested_mode", requested_mode);
    cJSON_AddStringToObject(r, "ssid", st.ssid);
    cJSON_AddStringToObject(r, "error", conn_err[0] ? conn_err : NULL);
    if (st.sta_connected) {
        cJSON_AddStringToObject(r, "ip", st.ip);
    }
    cJSON_AddBoolToObject(r, "boot_sound", s.boot_sound);
    cJSON_AddStringToObject(r, "device_name", s.device_name);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_device_ready(httpd_req_t *req)
{
    GUARD(req);

    board_wifi_status_t st;
    char conn_err[32], current_mode[16], requested_mode[16];
    wifi_state_snapshot(&st, conn_err, sizeof(conn_err), current_mode, sizeof(current_mode),
                        requested_mode, sizeof(requested_mode));

    const bool mode_ready = requested_mode[0] && strcmp(current_mode, requested_mode) == 0;
    const bool is_local = strcmp(current_mode, APP_SETTINGS_MODE_LOCAL) == 0;
    // The local album needs no network at all, which is the whole point of a frame you
    // can use over its own AP.
    const bool can_enter = mode_ready && (is_local || st.sta_connected);

    const char *wifi_state = "disconnected";
    if (conn_err[0]) {
        wifi_state = strcmp(conn_err, "connecting") == 0 ? "connecting" : "failed";
    } else if (st.sta_connected) {
        wifi_state = "connected";
    }

    cJSON *r = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(r, "wifi");
    cJSON *mode = cJSON_AddObjectToObject(r, "mode");
    cJSON *ui = cJSON_AddObjectToObject(r, "ui");
    cJSON_AddStringToObject(wifi, "state", wifi_state);
    cJSON_AddStringToObject(wifi, "ssid", st.ssid);
    cJSON_AddStringToObject(wifi, "ip", st.sta_connected ? st.ip : "");
    cJSON_AddStringToObject(wifi, "error",
                            (conn_err[0] && strcmp(conn_err, "connecting") != 0) ? conn_err : NULL);
    cJSON_AddStringToObject(mode, "requested", requested_mode);
    cJSON_AddStringToObject(mode, "current", current_mode);
    cJSON_AddBoolToObject(mode, "ready", mode_ready);
    cJSON_AddBoolToObject(ui, "can_enter_mode", can_enter);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_wifi_disconnect(httpd_req_t *req)
{
    GUARD(req);

    board_wifi_disconnect_keep_ap();
    // The stored credentials are deliberately NOT erased here, and this comment is the
    // point of the change (ticket 29, F4). The reference drops the link and stops the
    // retry timer, nothing more -- disconnect_sta_keep_ap_internal(),
    // refs/M5PaperColor-UserDemo/main/apps/app_manager/app_manager.cpp:105-120 -- and the
    // app_settings_set_wifi("", "") that used to be on this line was an undocumented
    // addition of ours. It also made one POST enough to put the frame permanently beyond
    // reach of everything except a phone standing next to it, which on this bench means
    // beyond reach altogether. FR-2.4 asks for a disconnect; forgetting the network is a
    // different request and does not have a route.

    state_lock();
    s_requested_mode[0] = '\0';
    s_conn_err[0] = '\0';
    state_unlock();
    app_settings_set_current_mode("");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "disconnected");
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// One scan of /data, shared by the list and delete handlers. photo_list already owns the
// FR-4.6 rules (extensions, no recursion, byte sort, 500 cap) and is host-tested.
static esp_err_t scan_photos(void)
{
    photo_list_init(&s_scan_list, s_scan_arena, sizeof(s_scan_arena));
    return board_storage_scan(&s_scan_list);
}

static esp_err_t h_photos_list(httpd_req_t *req)
{
    GUARD(req);

    int page = 1;
    int per = PHOTOS_PER_PAGE;
    // Opt-in, because filling `size` costs one stat() per listed photo and stat() on this
    // FATFS rescans the directory to resolve each name -- 22-27 ms measured here over a
    // 115-entry /data, agreeing with ticket 36's 38.3 ms over 215 entries. That made the
    // route linear in page size: 0.244 s at per_page=1 against 1.259 s at 48. Nothing in
    // index.html reads the field (photoCache keeps {name, url}; the only .size references
    // on the page are blob.size on the upload path), so the default is not to pay for it.
    // Kept as a parameter rather than deleted so the ported API shape survives for a
    // client that does want it.
    bool with_size = false;
    // 96 is enough for t=<32 hex> plus page, per_page and with_size. Grow it with any
    // further parameter: httpd_req_get_url_query_str() reports an over-long query as
    // RESULT_TRUNC, and the branch below treats every non-ESP_OK as "no parameters given"
    // and silently falls back to page 1 at PHOTOS_PER_PAGE.
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(query, "page", v, sizeof(v)) == ESP_OK && atoi(v) >= 1) {
            page = atoi(v);
        }
        if (httpd_query_key_value(query, "per_page", v, sizeof(v)) == ESP_OK && atoi(v) >= 1) {
            per = atoi(v);
        }
        if (httpd_query_key_value(query, "with_size", v, sizeof(v)) == ESP_OK && atoi(v) >= 1) {
            with_size = true;
        }
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    const esp_err_t serr = scan_photos();
    if (serr != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        return send_error(req, 500, "scan failed");
    }

    const int total = (int)photo_list_count(&s_scan_list);
    const int pages = total > 0 ? (total + per - 1) / per : 1;
    if (page > pages) {
        page = pages;
    }
    int start = (page - 1) * per;
    int end = start + per;
    if (end > total) {
        end = total;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "photos");
    for (int i = start; i < end; i++) {
        const char *name = photo_list_at(&s_scan_list, (size_t)i);
        char encoded[192];
        char url[208];
        if (!req_url_encode(name, encoded, sizeof(encoded))) {
            continue;
        }
        snprintf(url, sizeof(url), "/data/%s", encoded);
        // A sibling field rather than a change to `url`, which stays the full-size file: the
        // page uses url in exactly two places (photoCache and the <img src>) and the View
        // button still wants the original.
        char turl[208];
        snprintf(turl, sizeof(turl), "/thumb/%s", encoded);

        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", name);
        cJSON_AddStringToObject(p, "url", url);
        cJSON_AddStringToObject(p, "thumb_url", turl);
        if (with_size) {
            // The lock is taken and released per photo, so on a fitted card this also
            // churns spi_device_acquire_bus() once per listed file.
            char path[PATH_BUF];
            snprintf(path, sizeof(path), "%s/%s", BASE_PATH, name);
            struct stat st;
            long size = 0;
            board_storage_lock();
            if (stat(path, &st) == 0) {
                size = (long)st.st_size;
            }
            board_storage_unlock();
            cJSON_AddNumberToObject(p, "size", (double)size);
        }
        cJSON_AddItemToArray(a, p);
    }
    xSemaphoreGive(s_scan_mutex);

    cJSON_AddNumberToObject(r, "total", total);
    cJSON_AddNumberToObject(r, "page", page);
    cJSON_AddNumberToObject(r, "per_page", per);
    cJSON_AddNumberToObject(r, "total_pages", pages);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// The states thumb_produce() has to tell apart, because each is a different HTTP answer and
// saying the wrong one tells a user their photograph is corrupt when it is not. The mapping
// lives in h_thumb_serve(), which is the only caller that has a request to answer.
typedef enum {
    THUMB_OK = 0,
    THUMB_MISSING,      // 404, and deliberately silent: routine under smb_on_demand eviction
    THUMB_OVER_MAX,     // 413
    THUMB_NO_BUFFER,    // 503
    THUMB_DECODE_MEM,   // 503
    THUMB_DECODE_FAIL,  // 415
    THUMB_ENCODE_MEM,   // 503
    THUMB_ENCODE_FAIL,  // 500
} thumb_state_t;

// Read `path` off the card and produce its thumbnail JPEG in *out/*out_len; free with free().
//
// TWO callers, which is why it is not inline in a handler: `/thumb/` on a sidecar miss, and
// h_photos_upload() writing a sidecar of its own so that an uploaded photograph joins the
// population which never decodes again (ticket 48 option 4). A third open-coded copy is what
// this exists to prevent -- IMG_THUMB_MAX_EDGE and IMG_THUMB_QUALITY live in one place
// precisely because every producer must agree, and app_smb_sync.c's write_thumb() is already
// the second (it starts from an RGB buffer it has in hand, not from a file).
//
// Declared here rather than defined here because the upload handler comes first in this file
// and the body belongs next to h_thumb_serve(), which is where its reasoning was written.
// `tag` names the caller in the log lines so a capture can attribute them.
static thumb_state_t thumb_produce(const char *path, const char *name, const char *tag,
                                   uint8_t **out, size_t *out_len);

// True when the `bytes`-long file at `path` is a JPEG the display path can never decode.
//
// esp_jpeg takes a pointer rather than a stream, so the whole file has to be resident in PSRAM
// before its header is parsed, and epd_image.c:461 refuses anything over
// EPD_IMAGE_MAX_FILE_BYTES -- while UPLOAD_MAX_SIZE is twice that number. Two disagreeing byte
// ceilings meant a 6.52 MB photograph was stored, named, listed and then failed EVERY draw for
// ever, in 27.8 ms and with nothing saying so: no sidecar, a growing `failures=`, and
// POST /api/photos/display answering {"status":"ok"} for it (ticket 56's third item, ticket 46's
// A2 ladder).
//
// SNIFFED FROM THE BYTES, not from the extension, because the decoder dispatches the same way
// (epd_image.c:679-681) and the ceiling is JPEG-only by design: a non-interlaced PNG streams rows
// and has no file limit on the display path, so a 12 MP PNG is displayable and must stay
// accepted. The extension cannot be the test in either caller -- h_photos_upload() defaults a
// nameless blob's to "jpg", and its own comment says nothing else trusts the uploaded name.
//
// THE CALLER MUST ALREADY HOLD THE STORAGE LOCK. board_storage_lock() asserts rather than waits
// on a second take from the same task (board_storage.c:84), and both callers have the lock in
// hand at the point they ask.
//
// An unreadable file is NOT over the ceiling: this answers one question only, and a file that
// cannot be opened is a different failure that each caller already reports its own way.
//
// **Split in two on 2026-09-21 (ticket 72), and everything above applies to BOTH halves.** The
// sniff is now its own function because a second guard needs it: the progressive-JPEG probe in
// h_photos_upload() has to ask "is this a JPEG at all" without asking about its size. The
// paragraphs above are why the sniff reads bytes rather than the name, and why the caller must
// already hold the storage lock -- both are properties of the sniff, not of the ceiling.
static bool file_sniffs_jpeg(const char *path)
{
    uint8_t magic[8] = {0};
    size_t got = 0;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        got = fread(magic, 1, sizeof(magic), f);
        fclose(f);
    }
    return got > 0 && epd_image_sniff(magic, got) == EPD_IMAGE_JPEG;
}

static bool file_is_over_jpeg_ceiling(const char *path, long bytes)
{
    if (bytes <= (long)EPD_IMAGE_MAX_FILE_BYTES) {
        return false;
    }
    return file_sniffs_jpeg(path);
}

static esp_err_t h_photos_upload(httpd_req_t *req)
{
    GUARD(req);
    // FIRST LINE OF THE HANDLER, and the position was measured rather than chosen (ticket 59,
    // 2026-09-15). The stamp was tried below, just before board_storage_lock(), and a window still
    // opened 28.3 s into an upload -- because board_storage_usage() a few lines down ALREADY waits
    // for that lock (`usage=7,660 ms` in docs/agents/measurements.md, and a render is up to 32 s),
    // so the handler was blocked before it could say a client was here. Stamping on arrival is the
    // only point that cannot be behind a lock.
    //
    // One stamp, and it decays after SMB_SYNC_QUIET_MS like any other: a client that connects and
    // then says nothing stops counting, which is what ticket 26 needs. The receive loop in
    // bs_fill() keeps it fresh for as long as bytes actually arrive.
    mark_activity();
    // The moment the handler is entered, in the device's own ms. Ticket 59's arms had to infer it
    // as `the upload line minus the wall time curl measured`, which is the client's clock and
    // includes its connect and its wait for the response -- one cycle inferred a start 3.4 s
    // BEFORE the device booted. An upload arm cannot be read without this line.
    ESP_LOGI(TAG, "upload begin %u bytes", (unsigned)req->content_len);

    if (req->content_len == 0) {
        return send_error(req, 400, "no body");
    }
    if (req->content_len > UPLOAD_MAX_SIZE) {
        return send_error_code(req, 400, "upload.too_large", "file too large");
    }
    uint64_t total = 0, freeb = 0;
    if (board_storage_usage(&total, &freeb) == ESP_OK &&
        req->content_len + UPLOAD_FREE_MARGIN > freeb) {
        return send_error_code(req, 400, "upload.no_space", "not enough space");
    }

    char ct[256];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        return send_error(req, 400, "no content-type");
    }
    const char *bp = strstr(ct, "boundary=");
    if (!bp) {
        return send_error(req, 400, "no boundary");
    }
    bp += 9;
    if (*bp == '"') {
        bp++;
    }
    char boundary[128];
    size_t bi = 0;
    while (*bp && *bp != ' ' && *bp != ';' && *bp != '"' && bi + 1 < sizeof(boundary)) {
        boundary[bi++] = *bp++;
    }
    boundary[bi] = '\0';
    if (!boundary[0]) {
        return send_error(req, 400, "no boundary");
    }

    board_storage_lock();
    board_storage_prepare_access();
    FILE *fp = fopen(UPLOAD_TMP, "wb");
    if (!fp) {
        board_storage_unlock();
        return send_error(req, 500, "write fail");
    }

    body_stream_t bs;
    bs_init(&bs, req);
    upload_result_t res = {0};
    copy_str(res.action, "upload_only", sizeof(res.action));
    copy_str(res.algorithm, "dither", sizeof(res.algorithm));

    esp_err_t err = parse_multipart(&bs, boundary, fp, &res);
    fclose(fp);

    if (err != ESP_OK || !res.have_file || res.file_bytes == 0) {
        remove(UPLOAD_TMP);
        board_storage_unlock();
        if (bs.timed_out) {
            return send_error_code(req, 408, "upload.stalled", "upload stalled");
        }
        return send_error(req, err == ESP_ERR_NO_MEM ? 500 : 400,
                          err == ESP_ERR_NO_MEM ? "no space" : "no file");
    }

    // BEFORE the file is named, so a photograph that can never be drawn is never stored, never
    // listed and never reaches the slideshow. The bytes have already been spent -- an upload is
    // an API outage of its own length (46.6 s for 6.5 MB, ticket 46) and this refusal does not
    // shorten it, because req->content_len cannot be the test: it would refuse the 12 MP PNG
    // that works. A 413 costs the same upload and leaves nothing behind.
    if (file_is_over_jpeg_ceiling(UPLOAD_TMP, (long)res.file_bytes)) {
        remove(UPLOAD_TMP);
        board_storage_unlock();
        ESP_LOGW(TAG, "upload refused: JPEG is %u bytes, over the %u decoder ceiling",
                 (unsigned)res.file_bytes, (unsigned)EPD_IMAGE_MAX_FILE_BYTES);
        char msg[112];
        snprintf(msg, sizeof(msg), "JPEG is %u bytes; the frame cannot display over %u",
                 (unsigned)res.file_bytes, (unsigned)EPD_IMAGE_MAX_FILE_BYTES);
        // One of the three coded errors carrying numbers: a translation puts them in a different
        // place in the sentence, so they travel as fields rather than baked into the English.
        cJSON *d = cJSON_CreateObject();
        if (d) {
            cJSON_AddNumberToObject(d, "bytes", (double)res.file_bytes);
            cJSON_AddNumberToObject(d, "limit", (double)EPD_IMAGE_MAX_FILE_BYTES);
        }
        return send_error_full(req, 413, "upload.over_ceiling", msg, d);
    }

    // Ticket 72, and it is the same position and the same principle as the ceiling check above: a
    // photograph that can never be drawn is never stored, never listed and never reaches the
    // slideshow. The ceiling covered one member of that class -- a JPEG too big to fit in PSRAM --
    // and left the other, a JPEG this decoder does not implement. Both progressive files in ticket
    // 70's bench set uploaded with a 200, were given a thumbnail sidecar, appeared in the album and
    // then failed every render for ever, with the only outside symptom being a slideshow that
    // appears to skip an entry.
    //
    // **Option A of ticket 72 §4, chosen by the operator on 2026-09-21**: refuse, delete, and name
    // the cause, because that is what makes a refusal actionable -- the user can re-export as
    // baseline. B (store it and mark the album entry undisplayable) was the alternative and costs
    // three places instead of one.
    //
    // **Only a refusal refuses.** A probe that could not run -- no PSRAM for the file, an unreadable
    // temp file -- accepts the upload with a warning. Free PSRAM on this bench is ~5.7 MB against a
    // 6 MB ceiling, so "could not ask" is a state that really happens, and deleting a photograph
    // over it would be a worse defect than the one this guard closes.
    if (file_sniffs_jpeg(UPLOAD_TMP)) {
        const esp_err_t perr = epd_image_jpeg_probe_file(UPLOAD_TMP);
        if (perr == ESP_ERR_NOT_SUPPORTED) {
            remove(UPLOAD_TMP);
            board_storage_unlock();
            ESP_LOGW(TAG, "upload refused: the JPEG decoder cannot decode this file");
            // Ticket 74 quoted this one as the reason the subset exists: it is the refusal a user
            // most needs to understand, because re-exporting as baseline is something they can do.
            return send_error_code(req, 415, "upload.not_baseline",
                                   "this JPEG is progressive or uses an unsupported sampling "
                                   "factor; the frame can only display baseline JPEG");
        }
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "upload stored unprobed: %s", esp_err_to_name(perr));
        }
    }

    // Extension from the uploaded name, lowercased, .jpeg folded to .jpg. Nothing else
    // trusts the name -- the stored one is generated.
    char ext[16] = "jpg";
    const char *dot = res.filename[0] ? strrchr(res.filename, '.') : NULL;
    if (dot && dot[1]) {
        size_t e = 0;
        for (const char *p = dot + 1; *p && e + 1 < sizeof(ext); p++) {
            ext[e++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
        }
        ext[e] = '\0';
        if (strcmp(ext, "jpeg") == 0) {
            strcpy(ext, "jpg");
        }
    }

    char fname[64];
    if (generate_next_photo_name(ext, res.algorithm, fname, sizeof(fname)) != ESP_OK) {
        remove(UPLOAD_TMP);
        board_storage_unlock();
        return send_error(req, 500, "name gen fail");
    }
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, fname);
    if (rename(UPLOAD_TMP, path) != 0) {
        remove(UPLOAD_TMP);
        board_storage_unlock();
        return send_error(req, 500, "rename fail");
    }
    board_storage_unlock();

    ESP_LOGI(TAG, "upload %s (%u bytes) action=%s algorithm=%s", fname,
             (unsigned)res.file_bytes, res.action, res.algorithm);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "name", fname);
    if (strcmp(res.action, "upload_display") == 0) {
        // Through the slideshow, not straight to the panel: it owns the index, and an
        // image displayed behind its back would be advanced away from at the next tick.
        app_slideshow_show_name(fname);
        cJSON_AddStringToObject(r, "displaying", fname);
    }
    const esp_err_t serr = send_json(req, r);
    cJSON_Delete(r);

    // Ticket 48 option 4: give the upload the same sidecar the SMB import writes, so an
    // uploaded photograph joins the population /thumb/ answers with a file read instead of a
    // decode. Measured 2026-09-10 on this bench: without it a 12 MP upload costs 10.4 s per
    // tile on EVERY grid page for ever -- and read=5.65 s of that is the card read alone,
    // before any decode -- against 0.17-0.25 s from a sidecar.
    //
    // AFTER the rename and AFTER board_storage_unlock(), and both matter: the photograph is at
    // its final name, and thumb_produce() takes the storage lock itself, which asserts rather
    // than waits on a second take from the same task (board_storage.c:84).
    //
    // AFTER send_json() as well, which is a choice about who waits. The work blocks the single
    // httpd worker for the same ~10 s wherever it sits, so the outage is identical; putting it
    // after the response means the uploading client is not made to wait for a file it did not
    // ask for. The consequence is that a failure here cannot be reported in the response --
    // which is correct, because:
    //
    // A FAILURE HERE MUST NEVER FAIL THE UPLOAD. The photograph is already stored and named;
    // the sidecar is an optimisation, and app_smb_sync.c's write_thumb() has the same rule for
    // the same reason ("a missing sidecar is a slow /thumb/, not a failure"). That is also what
    // makes the over-ceiling case harmless: a file above EPD_IMAGE_MAX_FILE_BYTES gets no
    // sidecar, and /thumb/ answers its own 413 exactly as it did before.
    {
        char tpath[BOARD_STORAGE_PATH_MAX];
        app_smb_sync_sidecar_path(tpath, sizeof(tpath), fname, SMB_THUMB_SUFFIX);
        uint8_t *thumb = NULL;
        size_t thumb_len = 0;
        const thumb_state_t tst = thumb_produce(path, fname, "sidecar", &thumb, &thumb_len);
        if (tst == THUMB_OK) {
            board_storage_lock();
            board_storage_prepare_access();
            size_t wrote = 0;
            FILE *tf = fopen(tpath, "wb");
            if (tf != NULL) {
                wrote = fwrite(thumb, 1, thumb_len, tf);
                fclose(tf);
            }
            // A TRUNCATED SIDECAR IS WORSE THAN NONE: nothing else on the device would ever
            // notice it (it is not in .smbidx, the slideshow skips the suffix, and the delete
            // handler unlinks it blind), so /thumb/ would serve a broken tile for that
            // photograph's whole life. Unlink a short write rather than leave it.
            if (wrote != thumb_len) {
                unlink(tpath);
            }
            board_storage_unlock();
            if (wrote != thumb_len) {
                ESP_LOGW(TAG, "sidecar %s: wrote %u of %u B; removed", fname, (unsigned)wrote,
                         (unsigned)thumb_len);
            }
        } else {
            // thumb_produce() has already logged the cases that are worth a line of their own;
            // this says which state it was, so a missing sidecar can be told from a missing
            // attempt.
            ESP_LOGW(TAG, "sidecar %s: none written, state=%d", fname, (int)tst);
        }
        free(thumb);
    }
    return serr;
}

static esp_err_t h_photos_delete(httpd_req_t *req)
{
    GUARD(req);

    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error(req, 400, "name required");
    }
    char raw[96];
    if (httpd_query_key_value(query, "name", raw, sizeof(raw)) != ESP_OK) {
        return send_error(req, 400, "name required");
    }
    char name[96];
    if (!req_url_decode(raw, name, sizeof(name))) {
        return send_error(req, 400, "invalid name");
    }
    if (!req_name_is_safe(name)) {
        return send_error(req, 403, "invalid path");
    }

    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, name);
    // BOTH sidecars go with the photograph, as they do in app_smb_sync.c's delete_local().
    // Nothing else would ever remove either: photo_list skips both suffixes and so does the
    // mirror's directory scan, so an orphan would sit on the card for good. It is a leak rather
    // than a wrong thumbnail or a wrong caption -- store_photo() clears a stale sidecar before
    // writing, and an upload is given a fresh epoch-stamped name -- but it is still a leak.
    char tpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(tpath, sizeof(tpath), name, SMB_THUMB_SUFFIX);
    char mpath[BOARD_STORAGE_PATH_MAX];
    app_smb_sync_sidecar_path(mpath, sizeof(mpath), name, SMB_META_SUFFIX);

    board_storage_lock();
    board_storage_prepare_access();
    const int rc = unlink(path);
    unlink(tpath);
    unlink(mpath);
    board_storage_unlock();
    if (rc != 0) {
        return send_error(req, 404, "not found");
    }

    // Deleting what is on the glass is allowed and must not wedge anything: the panel
    // keeps showing it until the next navigation, which re-scans (FR-5.8).
    app_slideshow_invalidate();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "deleted");
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_storage(httpd_req_t *req)
{
    GUARD(req);

    uint64_t total = 0, freeb = 0;
    board_storage_usage(&total, &freeb);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "total", (double)total);
    cJSON_AddNumberToObject(r, "used", (double)(total - freeb));
    cJSON_AddNumberToObject(r, "free", (double)freeb);
    // WHICH VOLUME THESE NUMBERS DESCRIBE, and whether a card is in the slot at all. Added
    // 2026-09-08 (ticket 08): the capacity alone is not that answer -- after a card was pulled
    // and reseated this route went on reporting the card's 1.98 GB while the directory read
    // empty and every file 404'd, because the mount was stale and nothing said so. `media` is
    // what the mount believes; `card_present` is the detect line, which is the only reading
    // that can disagree with it.
    cJSON_AddStringToObject(
        r, "media", board_storage_get_media() == BOARD_STORAGE_MEDIA_SD ? "sd" : "flash");
    cJSON_AddBoolToObject(r, "card_present", board_card_present());
    // The volume's FAT type, so the page can tell the user to reformat a FAT16 card before its
    // 512-entry root fills (board_storage_fat_type() has the account). "" when unreadable.
    static const char *const FAT_NAMES[] = {"", "fat12", "fat16", "fat32", "exfat"};
    const int fat = board_storage_fat_type();
    cJSON_AddStringToObject(r, "fat", (fat >= 0 && fat <= 4) ? FAT_NAMES[fat] : "");
    // A card the frame fell back from because it has no filesystem this build reads (exFAT,
    // NTFS, blank) -- as opposed to one it failed to talk to, where reformatting is no answer.
    cJSON_AddBoolToObject(r, "card_unreadable", board_storage_card_unreadable());
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// The media watch belongs to app_main(), which owns it for the life of the process and polls it
// every 10 s; POST /api/storage/rescan needs the SAME one, because clearing the lock on a copy
// would clear nothing the poll can see. A pointer rather than a copy for exactly that reason.
static board_storage_watch_t *s_storage_watch;

void app_server_set_storage_watch(board_storage_watch_t *w)
{
    s_storage_watch = w;
}

// THE WAY OUT OF A HELD FALLBACK LOCK, which board_storage.h:82-84 has always specified and
// nothing provided. board_storage_ensure()'s lock is deliberate -- a card that failed to
// initialise must not be retried on every pass, or the frequency ladder runs forever and the UI
// stalls three seconds at a time -- and it clears only on a reseat. The media poll in
// frame_main.c is EDGE-TRIGGERED on the detect line, so it cannot clear it either: measured
// 2026-09-08, a power cut that coincided with an insertion left `media: flash` with
// `card_present: true` and no way back except another reseat or a reboot.
//
// So this is the button the header asked for. It clears the lock and asks once; it does not
// loop, because "ask once when a human says so" is exactly the thing the lock is not meant to
// prevent.
static esp_err_t h_storage_rescan(httpd_req_t *req)
{
    GUARD(req);

    if (!s_storage_watch) {
        return send_error(req, 503, "no media watch");
    }
    board_storage_rescan_request(s_storage_watch);
    const bool changed = board_storage_ensure(s_storage_watch);
    if (changed) {
        app_smb_sync_media_changed();
        app_slideshow_invalidate();
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "changed", changed);
    cJSON_AddStringToObject(
        r, "media", board_storage_get_media() == BOARD_STORAGE_MEDIA_SD ? "sd" : "flash");
    cJSON_AddBoolToObject(r, "card_present", board_card_present());
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// Ticket 09: restart as a USB drive (board_usb_msc.h). 501 on a board whose connector does not
// reach the SoC's USB, and the page hides its button there off `storage.usb_msc`.
//
// **Quiesced before the restart, not just restarted.** Waiting for the panel keeps a refresh from
// being cut off mid-waveform, and taking the storage lock waits out whatever file is being
// written and keeps the next one from starting -- the lock is held into the reset on purpose.
static esp_err_t h_storage_usb(httpd_req_t *req)
{
    GUARD(req);

    if (!BOARD_HAS_USB_MSC) {
        return send_error(req, 501, "this board has no USB drive mode");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "restarting");
    httpd_resp_set_status(req, "202 Accepted");
    send_json(req, r);
    cJSON_Delete(r);

    app_display_wait_idle(60000);
    board_storage_lock();
    board_usb_msc_request();
    board_storage_unlock();  // only reached if the request was refused
    return ESP_OK;
}

// Ticket 61. The body is the signed firmware.bin itself, not multipart:
//     curl --data-binary @.pio/build/frame/firmware.bin http://<frame>/api/system/ota?t=<token>
// Streamed into the OTA slot 2 KB at a time, so nothing near 1.7 MB is ever held in RAM. The
// signature and the board are checked after the last byte and before the boot slot changes; a
// refusal leaves the running image as the one that boots.
static esp_err_t h_system_ota(httpd_req_t *req)
{
    GUARD(req);

    if (req->content_len == 0) {
        return send_error(req, 400, "empty body");
    }
    const char *why = "";
    esp_err_t err = app_ota_begin(req->content_len, &why);
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_SIZE ? 413 : 503, why);
    }
    body_stream_t bs;
    bs_init(&bs, req);
    for (;;) {
        const bool ok = bs_fill(&bs);
        if (bs.len > 0) {
            if (app_ota_write(bs.buf, bs.len, &why) != ESP_OK) {
                return send_error(req, 400, why);
            }
            bs.len = 0;
        }
        if (!ok) {
            app_ota_abort();
            return send_error(req, bs.timed_out ? 408 : 400, "the body stopped arriving");
        }
        if (bs.remaining == 0) {
            break;
        }
    }
    if (app_ota_finish(&why) != ESP_OK) {
        return send_error(req, 400, why);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "restarting");
    httpd_resp_set_status(req, "202 Accepted");
    send_json(req, r);
    cJSON_Delete(r);

    // The same order as the USB drive mode above: no restart under a refresh or a volume write.
    app_display_wait_idle(60000);
    board_storage_lock();
    vTaskDelay(pdMS_TO_TICKS(500));  // let the 202 leave before the socket dies with the chip
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_battery(httpd_req_t *req)
{
    GUARD(req);

    board_power_t pwr = {0};
    const uint16_t mv = board_power_read(&pwr) == ESP_OK ? pwr.vbat_mv : 0;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "voltage_mv", (double)mv);

    // The charger, and **the whole object is ABSENT on a board with no reachable one** rather
    // than present with zeroes. board.h's rule is that a caller must not read a false field as a
    // measurement, and `"charging": false` on the M5Paper Color -- which cannot report charge
    // state at all (ticket 71 §1) -- would be exactly that mistake served as JSON.
    // **From app_charge's cache, NOT read here.** Twelve more I2C transactions per request, from
    // the task holding the server's only worker, is not worth a fresher battery icon.
    // `regs_age_ms` says how old the block is; `voltage_mv` above is live. (This comment used to
    // justify itself with a bus that "takes turns by luck"; that was wrong -- the IDF driver
    // serialises transactions per bus. Ticket 76. The cost argument is the real one.)
    app_charge_state_t ch;
    app_charge_get(&ch);
    const board_charger_t chg = ch.last;
    if (ch.last_valid) {
        cJSON *c = cJSON_AddObjectToObject(r, "charger");
        cJSON_AddNumberToObject(c, "chrg_stat", (double)chg.chrg_stat);
        cJSON_AddBoolToObject(c, "power_good", chg.power_good);
        cJSON_AddNumberToObject(c, "vreg_mv", (double)chg.vreg_mv);
        cJSON_AddNumberToObject(c, "regs_age_ms", (double)ch.last_age_ms);
        // The cap's own state, beside the hardware it acts on. `corrections` is the instrument for
        // ticket 71 §9.3's open question -- whether this part's I2C watchdog restores defaults --
        // so it is served rather than only printed, and a long-running frame answers it.
        cJSON_AddNumberToObject(c, "limit_pct", (double)ch.limit_pct);
        cJSON_AddNumberToObject(c, "target_mv", (double)ch.target_mv);
        cJSON_AddNumberToObject(c, "applied_mv", (double)ch.applied_mv);
        cJSON_AddNumberToObject(c, "corrections", (double)ch.corrections);
        cJSON_AddNumberToObject(c, "writes", (double)ch.writes);
        cJSON_AddBoolToObject(c, "watchdog_off", ch.watchdog_off);
        // **The raw block too, because the decode above may be reading the wrong bits.** REG08's
        // field positions have three mutually inconsistent readings among the sources available
        // here and no datasheet is on this disk (ticket 71 §9.2), so the bytes go out beside the
        // verdict and an analysis can disagree with this build without a reflash. This is the
        // same "emit raw numbers, compute the verdict at analysis time" rule the harness follows.
        char hex[sizeof(chg.regs) * 2 + 1];
        for (size_t i = 0; i < sizeof(chg.regs); i++) {
            static const char d[] = "0123456789ABCDEF";
            hex[i * 2] = d[chg.regs[i] >> 4];
            hex[i * 2 + 1] = d[chg.regs[i] & 0x0F];
        }
        hex[sizeof(hex) - 1] = '\0';
        cJSON_AddStringToObject(c, "regs", hex);
    }

    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// `orientation` is the wire name for `rotation`, and the two numberings are not the same.
// epd_canvas's rotation 0 is THE PANEL'S NATIVE ORIENTATION and 1 turns it (epd_canvas.h:31,
// epd_canvas.c's logical accessors). Upstream's numbering is the other way round, and its
// mapping -- `rotation == 0 ? "landscape" : "portrait"` at
// refs/M5PaperColor-UserDemo/main/apps/app_server/app_server.cpp:1047,1078 -- was copied here
// verbatim, so selecting "landscape" rendered portrait and vice versa. Measured on hardware
// 2026-09-06 from the auto flow's `region=` line, which is derived from the live canvas:
// docs/agents/measurements.md.
//
// THAT CORRECTION WAS STILL A CONSTANT, and a constant is a portrait-native panel's answer.
// `rotation == 1 ? "landscape" : "portrait"` is right on the EL040EF1 (400x600) and inverted on
// the ED2208-GCA (800x480), where rotation 0 is already landscape -- ticket 62 item 4. So the
// word is derived from the panel's own dimensions here and in h_mode_cfg_set(), by the two
// helpers below rather than at each site, because a read and a write that disagree about this
// is a setting that flips itself.
//
// The STORED VALUE's meaning is unchanged by any of it: rotation is "native" or "turned", on
// both panels, so no NVS migration is needed -- the same argument as 2026-09-06.
//
// **AND SINCE TICKET 69 THE AUTHORITATIVE WIRE FIELD IS THE NUMBER, not this word.** Four quarter
// turns need four names and `orientation` has two, so `rotation: 0..3` was added as the real field
// and `orientation` stays as a two-valued DERIVED one for compatibility -- which is what the two
// failures above argue for: the setting is a count of quarter turns, and the panel-relative word is
// output. The operator chose that shape on 2026-09-20. The word cannot say which of the two
// landscape directions a frame is in, so a client that only speaks `orientation` keeps working and
// loses nothing it used to have.
static bool panel_is_native_landscape(void)
{
    return EPD_WIDTH > EPD_HEIGHT;
}

// The word for a stored rotation, on this panel. Rotations 1 and 3 are the turned ones -- the odd
// bit, exactly as epd_canvas.c derives it, rather than `== 1`, which silently called rotation 3
// by the native orientation's name.
static const char *orientation_name(uint8_t rotation)
{
    const bool turned = (rotation & 1u) != 0u;
    const bool landscape = panel_is_native_landscape() ? !turned : turned;
    return landscape ? "landscape" : "portrait";
}

// And back: the rotation a word asks for. An unrecognised word means "not the native one",
// which is what the previous `strcmp(..., "portrait") == 0 ? 0 : 1` did on this panel.
//
// **Two values out of four, deliberately.** A word cannot name a half turn, so this answers with
// the un-flipped member of each pair; a client that wants 2 or 3 sends `rotation`. Which is also
// why `rotation` WINS over `orientation` in h_mode_cfg_set() when a body carries both and they
// disagree -- see there.
static uint8_t orientation_rotation(const char *name)
{
    const bool wants_landscape = (strcmp(name, "portrait") != 0);
    return (wants_landscape == panel_is_native_landscape()) ? 0 : 1;
}

static void add_mode_config(cJSON *r, const app_settings_t *s)
{
    // **`rotation` is the authoritative field and `orientation` is derived from it** (ticket 69).
    // Both are served: the number is a count of quarter turns from the panel's native orientation
    // and says everything, the word is two-valued and is what an older client reads.
    cJSON_AddNumberToObject(r, "rotation", s->rotation);
    cJSON_AddNumberToObject(r, "rotation_max", EPD_CANVAS_ROTATION_MAX);
    cJSON_AddStringToObject(r, "orientation", orientation_name(s->rotation));

    // The panel's PHYSICAL size, in its native orientation, so the page works in the glass's
    // own pixels instead of the 400x600 pair it used to have written into it in five places.
    // `panel_native` is derivable from the two numbers and is served anyway: this mapping has
    // been got the wrong way round once already, and a name is cheaper to check than an
    // inequality.
    cJSON_AddNumberToObject(r, "panel_width", EPD_WIDTH);
    cJSON_AddNumberToObject(r, "panel_height", EPD_HEIGHT);
    cJSON_AddStringToObject(r, "panel_native",
                            panel_is_native_landscape() ? "landscape" : "portrait");

    cJSON_AddBoolToObject(r, "auto_slideshow", s->auto_slideshow);
    cJSON_AddNumberToObject(r, "interval_minutes", s->interval_minutes);
    cJSON_AddBoolToObject(r, "low_power_mode", s->low_power_mode);

    // The palette, plus the list of them. The list is here rather than in index.html so that
    // adding a palette to epd_dither.c makes it appear in the interface -- a hardcoded set of
    // <option>s in a 134 KB embedded asset is the kind of duplicate that goes stale silently.
    cJSON_AddStringToObject(r, "palette",
                            epd_palette_id_name((epd_palette_id_t)s->palette));
    //
    // Each entry also carries the numbers the page needs to quantise the way the device does:
    // `match` is the palette's own table **in table order**, four values per colour
    // (r, g, b, index), and `tone` is its compression amount. Order matters and is not
    // cosmetic -- epd_nearest_index_cfg() and the pair search keep the earlier entry on a
    // distance tie, so shuffling these changes the output (epd_dither.h:59-65). The page also
    // draws the resulting indices in these same colours, as epdoptimize's demo does.
    cJSON *palettes = cJSON_AddArrayToObject(r, "palettes");
    for (int i = 0; i < EPD_PALETTE_ID_COUNT; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", epd_palette_id_name((epd_palette_id_t)i));
        cJSON_AddStringToObject(entry, "label", epd_palette_id_label((epd_palette_id_t)i));

        const epd_render_t cfg = epd_render_for_palette((epd_palette_id_t)i);
        cJSON_AddNumberToObject(entry, "tone", cfg.tone);
        cJSON *match = cJSON_AddArrayToObject(entry, "match");
        for (int c = 0; c < EPD_PALETTE_COUNT; c++) {
            cJSON_AddItemToArray(match, cJSON_CreateNumber(cfg.palette[c].r));
            cJSON_AddItemToArray(match, cJSON_CreateNumber(cfg.palette[c].g));
            cJSON_AddItemToArray(match, cJSON_CreateNumber(cfg.palette[c].b));
            cJSON_AddItemToArray(match, cJSON_CreateNumber(cfg.palette[c].index));
        }
        cJSON_AddItemToArray(palettes, entry);
    }

    // The dither strength the device renders at, so the page cannot drift from it: the shipping
    // preview used 200 where every device render uses EPD_DITHER_STRENGTH_QUALITY (140).
    cJSON_AddNumberToObject(r, "dither_strength", EPD_DITHER_STRENGTH_QUALITY);

    // epdoptimize's per-image auto flow (src/epd_auto.h). Another addition to FR-8's nine, and
    // like the palette it redraws when it changes.
    cJSON_AddBoolToObject(r, "auto_adjust", s->auto_adjust);

    // epdoptimize's error diffusion as the quantiser (src/epd_diffuse.h). A third addition, and
    // deliberately its own field rather than folded into auto_adjust -- app_settings.h says why.
    cJSON_AddBoolToObject(r, "dither_diffuse", s->dither_diffuse);

    // Whether the slideshow walks a shuffled permutation instead of filename order
    // (app_slideshow.c's epoch section). A fourth addition to FR-8's nine. `slideshow_seed` is
    // deliberately NOT served: it is state rather than a setting, and nothing outside the
    // slideshow has a use for it.
    cJSON_AddBoolToObject(r, "slideshow_random", s->slideshow_random);

    // Whether a picture the frame is not hung the way up for is turned 90 degrees so that it
    // fills the panel (ticket 51). A fifth addition to FR-8's nine, and distinct from
    // `orientation`: that one says how the frame hangs, this one says whether one picture may
    // depart from it.
    cJSON_AddBoolToObject(r, "auto_rotate", s->auto_rotate);

    // The clock and the schedule (tickets 41 and 42). A sixth and seventh addition to FR-8's
    // nine -- except that the schedule's on/off is not new: it is `low_power_mode`, already
    // served above, which before this meant nothing at all. app_settings.h argues that reuse.
    //
    // Served as an offset in minutes rather than a zone name because that is what is stored;
    // there is no tzset() and no IANA database in this build.
    cJSON_AddNumberToObject(r, "tz_offset_minutes", s->tz_offset_minutes);
    cJSON_AddNumberToObject(r, "active_start_hour", s->active_start_hour);
    cJSON_AddNumberToObject(r, "active_end_hour", s->active_end_hour);

    // Panel maintenance (ticket 68). Both belong to the schedule above rather than to the render
    // settings: one is what the closed window shows, the other is which day inside it the colour
    // course runs. `maint_day` is 0=Sunday..6=Saturday with 7 for never, served as the raw code --
    // a client that mapped 7 to "off" and back would be a second place the encoding lives.
    cJSON_AddBoolToObject(r, "standby_white", s->standby_white);
    cJSON_AddBoolToObject(r, "standby_deep", s->standby_deep);
    cJSON_AddNumberToObject(r, "maint_day", s->maint_day);
    // Ticket 71. Served on both boards -- the SETTING exists everywhere and only one board can act
    // on it, so the page needs the value here and the "can this board do it" answer from
    // /api/battery's `charger` object, which is absent where it cannot.
    cJSON_AddNumberToObject(r, "charge_limit_pct", s->charge_limit_pct);
}

static esp_err_t h_mode_cfg_get(httpd_req_t *req)
{
    GUARD(req);

    app_settings_t s;
    app_settings_get(&s);
    cJSON *r = cJSON_CreateObject();
    add_mode_config(r, &s);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_mode_cfg_set(httpd_req_t *req)
{
    GUARD(req);

    char body[256];
    cJSON *j = read_json_body(req, body, sizeof(body));
    if (!j) {
        return send_error(req, 400, "bad json");
    }

    // Validate everything before applying anything. Applying as we parse means a bad
    // interval leaves the orientation already changed and saved, and the client is told
    // 400 -- so the device and the page disagree about what happened, and the next GET
    // is the only way to find out.
    cJSON *orientation = cJSON_GetObjectItem(j, "orientation");
    cJSON *rotation = cJSON_GetObjectItem(j, "rotation");
    cJSON *auto_slideshow = cJSON_GetObjectItem(j, "auto_slideshow");
    cJSON *interval = cJSON_GetObjectItem(j, "interval_minutes");
    cJSON *low_power = cJSON_GetObjectItem(j, "low_power_mode");
    cJSON *palette = cJSON_GetObjectItem(j, "palette");
    cJSON *auto_adjust = cJSON_GetObjectItem(j, "auto_adjust");
    cJSON *dither_diffuse = cJSON_GetObjectItem(j, "dither_diffuse");
    cJSON *slideshow_random = cJSON_GetObjectItem(j, "slideshow_random");
    cJSON *auto_rotate = cJSON_GetObjectItem(j, "auto_rotate");
    cJSON *tz_offset = cJSON_GetObjectItem(j, "tz_offset_minutes");
    cJSON *active_start = cJSON_GetObjectItem(j, "active_start_hour");
    cJSON *active_end = cJSON_GetObjectItem(j, "active_end_hour");
    cJSON *standby_white = cJSON_GetObjectItem(j, "standby_white");
    cJSON *standby_deep = cJSON_GetObjectItem(j, "standby_deep");
    cJSON *maint_day = cJSON_GetObjectItem(j, "maint_day");
    cJSON *charge_limit = cJSON_GetObjectItem(j, "charge_limit_pct");

    const bool has_rotation = cJSON_IsNumber(rotation);
    // **`rotation` wins over `orientation` when a body carries both**, and this is written down
    // rather than left to fall out of the parse order, which is cJSON's iteration order and would
    // make the answer depend on how a client happened to serialise its object. The number is the
    // field named as authoritative, so it is the one that decides; the word is then ignored rather
    // than applied second. Ticket 69 §4 -- a read and a write that disagree about this is a setting
    // that flips itself, and that has already happened twice here.
    const bool has_orientation = !has_rotation && cJSON_IsString(orientation) &&
                                 orientation->valuestring;
    if (has_rotation &&
        (rotation->valueint < 0 || rotation->valueint > EPD_CANVAS_ROTATION_MAX)) {
        cJSON_Delete(j);
        return send_error(req, 400, "rotation out of range");
    }
    const bool has_interval = cJSON_IsNumber(interval);
    if (has_interval && (interval->valueint < 1 || interval->valueint > 255)) {
        cJSON_Delete(j);
        return send_error(req, 400, "interval out of range");
    }

    // Validated before anything is applied, like the interval above: a body that gets one of
    // these wrong changes nothing at all rather than half of it.
    if (cJSON_IsNumber(tz_offset) &&
        (tz_offset->valueint < -720 || tz_offset->valueint > 840)) {
        cJSON_Delete(j);
        return send_error(req, 400, "tz_offset_minutes out of range");
    }
    // The two hours are a pair, so BOTH must be present or neither. Accepting one alone would
    // silently combine it with whatever is stored and produce a window the client never asked
    // for -- and for a schedule that means the panel holding at the wrong time, which is
    // exactly the class of bug ticket 42 is written to avoid.
    const bool has_start = cJSON_IsNumber(active_start);
    const bool has_end = cJSON_IsNumber(active_end);
    if (has_start != has_end) {
        cJSON_Delete(j);
        return send_error(req, 400, "active_start_hour and active_end_hour must be sent together");
    }
    if (has_start && (active_start->valueint < 0 || active_start->valueint > 23 ||
                      active_end->valueint < 0 || active_end->valueint > 23)) {
        cJSON_Delete(j);
        return send_error(req, 400, "active hours out of range");
    }
    // 0..6 is a weekday and 7 is never, so 8 is not "off by one from Saturday" -- it is a client
    // that has a different encoding, and refusing it is how that gets found. Validated here rather
    // than left to the setter, so a body with one bad field changes nothing (ticket 68).
    if (cJSON_IsNumber(maint_day) && (maint_day->valueint < 0 || maint_day->valueint > 7)) {
        cJSON_Delete(j);
        return send_error(req, 400, "maint_day out of range");
    }
    // Ticket 71, and the same argument one field over: three values mean something and **90 is the
    // one a user will try**, so it is refused by name here rather than turned into a silent no-op
    // by the setter.
    if (cJSON_IsNumber(charge_limit) && charge_limit->valueint != 0 &&
        charge_limit->valueint != 80 && charge_limit->valueint != 100) {
        cJSON_Delete(j);
        return send_error(req, 400, "charge_limit_pct must be 0, 80 or 100");
    }

    // Refused rather than ignored: a client that sends a palette this build does not have
    // has a bug, and answering 200 would hide it behind a picture that did not change.
    epd_palette_id_t palette_id = EPD_PALETTE_ID_AITJCIZE;
    bool has_palette = false;
    if (cJSON_IsString(palette) && palette->valuestring) {
        if (!epd_palette_id_from_name(palette->valuestring, &palette_id)) {
            cJSON_Delete(j);
            return send_error(req, 400, "unknown palette");
        }
        has_palette = true;
    }

    // Everything above this line refuses; everything below it applies. The six settings that owe
    // more than a persist go through the transaction, which holds the read-compare, the live half
    // and the redraw decision for all of them -- see app_apply.h. The other ten fields in this
    // body owe only a persist, so they keep calling their setter directly: there is no second half
    // to forget, and putting them through the transaction would be ceremony rather than depth.
    app_apply_txn_t txn;
    app_apply_begin(&txn);

    if (has_rotation || has_orientation) {
        // See add_mode_config(): which rotation a WORD means is a property of the panel, and the
        // number means the same thing on both boards. `has_orientation` is already false when a
        // number was sent, so this is the precedence rule and not a second application of it.
        const uint8_t rot = has_rotation ? (uint8_t)rotation->valueint
                                         : orientation_rotation(orientation->valuestring);
        app_apply_set(&txn, APP_SETTING_ROTATION, (int)rot);
    }
    if (cJSON_IsBool(auto_slideshow)) {
        app_settings_set_auto_slideshow(cJSON_IsTrue(auto_slideshow));
    }
    if (has_interval) {
        app_settings_set_interval_minutes(interval->valueint);
    }
    if (cJSON_IsBool(low_power)) {
        app_settings_set_low_power_mode(cJSON_IsTrue(low_power));
    }
    if (has_palette) {
        app_apply_set(&txn, APP_SETTING_PALETTE, (int)palette_id);
    }
    if (cJSON_IsBool(auto_adjust)) {
        app_apply_set(&txn, APP_SETTING_AUTO_ADJUST, cJSON_IsTrue(auto_adjust) ? 1 : 0);
    }
    if (cJSON_IsBool(dither_diffuse)) {
        app_apply_set(&txn, APP_SETTING_DITHER_DIFFUSE, cJSON_IsTrue(dither_diffuse) ? 1 : 0);
    }
    if (cJSON_IsBool(auto_rotate)) {
        app_apply_set(&txn, APP_SETTING_AUTO_ROTATE, cJSON_IsTrue(auto_rotate) ? 1 : 0);
    }
    // No redraw flag: this changes which photograph comes NEXT, not how the displayed one
    // looks, so redrawing would be a visible no-op that costs a 15 s refresh.
    if (cJSON_IsBool(slideshow_random) &&
        cJSON_IsTrue(slideshow_random) != app_settings_slideshow_random()) {
        app_settings_set_slideshow_random(cJSON_IsTrue(slideshow_random));
    }
    // No redraw flag on either of these for the same reason as slideshow_random: they change
    // WHEN the frame refreshes, not what the displayed photograph looks like. A change that
    // opens the window is picked up by the slideshow's next tick, within 200 ms.
    if (cJSON_IsNumber(tz_offset) && tz_offset->valueint != app_settings_tz_offset_minutes()) {
        app_settings_set_tz_offset_minutes(tz_offset->valueint);
    }
    if (has_start) {
        uint8_t cur_start = 0, cur_end = 0;
        app_settings_active_hours(&cur_start, &cur_end);
        if (active_start->valueint != (int)cur_start || active_end->valueint != (int)cur_end) {
            app_settings_set_active_hours(active_start->valueint, active_end->valueint);
        }
    }
    // Ticket 68. No redraw flag either: `standby_white` changes what the NEXT window edge does and
    // `maint_day` which night the course runs, so neither has anything to show now. In particular
    // turning the standby ON must not park the panel immediately -- the setting is about the window
    // edge, and a photograph replaced by white the moment a switch is flipped in the afternoon would
    // read as the frame having broken.
    if (cJSON_IsBool(standby_white) && cJSON_IsTrue(standby_white) != app_settings_standby_white()) {
        app_settings_set_standby_white(cJSON_IsTrue(standby_white));
    }
    if (cJSON_IsBool(standby_deep) && cJSON_IsTrue(standby_deep) != app_settings_standby_deep()) {
        app_settings_set_standby_deep(cJSON_IsTrue(standby_deep));
    }
    // Ticket 71. Applying it is the other half, and app_setting_effect.h is where that is written
    // down now: the row routes this key to the charger, so app_charge_apply() runs from inside the
    // transaction when the cap actually changed -- and only then, which is what keeps a re-sent
    // 80 from rewriting the register. No redraw, because the cap changes nothing on the glass.
    if (cJSON_IsNumber(charge_limit)) {
        app_apply_set(&txn, APP_SETTING_CHARGE_LIMIT, charge_limit->valueint);
    }
    if (cJSON_IsNumber(maint_day) && maint_day->valueint != (int)app_settings_maint_day()) {
        app_settings_set_maint_day(maint_day->valueint);
    }
    cJSON_Delete(j);

    // FR-5.2: an orientation change re-renders what is displayed. A palette change gets the same
    // treatment for the same reason -- the point of choosing one is to see it, and waiting for the
    // slideshow's next tick to find out would make the setting feel broken. Turning the auto flow
    // on or off changes the picture just as visibly, and so does swapping the quantiser.
    //
    // Which of the FIVE render settings that covers -- the four reasons above plus `auto_rotate`,
    // which the sentence above never named -- is app_setting_effect.h's table now, rather than five
    // booleans and a disjunction here. At most ONE refresh, however many of them the body carried.
    app_apply_commit(&txn);

    app_settings_t s;
    app_settings_get(&s);
    cJSON *r = cJSON_CreateObject();
    add_mode_config(r, &s);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_mode_switch(httpd_req_t *req)
{
    GUARD(req);

    char body[256];
    cJSON *j = read_json_body(req, body, sizeof(body));
    if (!j) {
        return send_error(req, 400, "bad json");
    }
    cJSON *m = cJSON_GetObjectItem(j, "mode");
    if (!cJSON_IsString(m) || !m->valuestring) {
        cJSON_Delete(j);
        return send_error(req, 400, "mode required");
    }
    // mode_1 is the only mode there is (ticket 22). Anything else, including the stock
    // firmware's mode_2, is refused here and normalises to "" in app_settings.
    if (strcmp(m->valuestring, APP_SETTINGS_MODE_LOCAL) != 0) {
        cJSON_Delete(j);
        return send_error(req, 400, "invalid mode");
    }

    const esp_err_t serr = app_settings_set_current_mode(m->valuestring);
    cJSON_Delete(j);
    if (serr != ESP_OK) {
        return send_error(req, 500, "mode switch failed");
    }
    state_lock();
    copy_str(s_requested_mode, APP_SETTINGS_MODE_LOCAL, sizeof(s_requested_mode));
    state_unlock();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "mode", APP_SETTINGS_MODE_LOCAL);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_photos_display(httpd_req_t *req)
{
    GUARD(req);

    char body[256];
    cJSON *j = read_json_body(req, body, sizeof(body));
    if (!j) {
        return send_error(req, 400, "bad json");
    }
    cJSON *n = cJSON_GetObjectItem(j, "name");
    if (!cJSON_IsString(n) || !n->valuestring || !n->valuestring[0]) {
        cJSON_Delete(j);
        return send_error(req, 400, "name required");
    }
    if (!req_name_is_safe(n->valuestring)) {
        cJSON_Delete(j);
        return send_error(req, 403, "invalid path");
    }

    char name[96];
    copy_str(name, n->valuestring, sizeof(name));
    cJSON_Delete(j);

    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, name);
    struct stat st;
    board_storage_lock();
    board_storage_prepare_access();
    const bool exists = stat(path, &st) == 0 && S_ISREG(st.st_mode);
    // In the same lock section as the stat, whose st_size this needs anyway: eight bytes off a
    // file already being opened costs nothing measurable, and it is the only way a photograph
    // that predates the upload refusal above gets reported instead of answered with
    // {"status":"ok"} and then failing silently on the panel. The upload check closes the way in;
    // this closes the way a file already on the card is asked for.
    const bool over_ceiling = exists && file_is_over_jpeg_ceiling(path, (long)st.st_size);
    board_storage_unlock();
    if (!exists) {
        return send_error(req, 404, "not found");
    }
    if (over_ceiling) {
        ESP_LOGW(TAG, "display %s refused: JPEG is %ld bytes, over the %u decoder ceiling", name,
                 (long)st.st_size, (unsigned)EPD_IMAGE_MAX_FILE_BYTES);
        char msg[112];
        snprintf(msg, sizeof(msg), "JPEG is %ld bytes; the frame cannot display over %u",
                 (long)st.st_size, (unsigned)EPD_IMAGE_MAX_FILE_BYTES);
        return send_error(req, 413, msg);
    }

    // Returns as soon as the request is posted. The refresh itself takes 15.6 s and the
    // UI must not wait on it (NFR-5).
    if (app_slideshow_show_name(name) != ESP_OK) {
        return send_error(req, 404, "not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "displaying", name);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// Turns a request URI into a bare, safe filename. `prefix_len` is the length of the route
// prefix -- 6 for "/data/", 7 for "/thumb/".
//
// req->uri carries the query string, and these routes have a reason to receive one:
// GUARD() accepts ?t=<token> as the cookie-less fallback, so /data/x.jpg?t=... is a
// request the handler must answer rather than turn into a filename ending in "?t=...".
// Found in a browser on 2026-09-05 -- a thumbnail with any query at all came back 404,
// which looks exactly like an auth failure and is not one.
static bool request_file_name(const httpd_req_t *req, size_t prefix_len,
                              char *out, size_t out_size)
{
    const char *fp = req->uri + prefix_len;
    char raw[128];
    const char *q = strchr(fp, '?');
    if (q) {
        const size_t n = (size_t)(q - fp);
        if (n >= sizeof(raw)) {
            return false;
        }
        memcpy(raw, fp, n);
        raw[n] = '\0';
        fp = raw;
    }
    return *fp != '\0' && req_url_decode(fp, out, out_size) && req_name_is_safe(out);
}

static esp_err_t h_data_serve(httpd_req_t *req)
{
    GUARD(req);

    char name[96];
    if (!request_file_name(req, sizeof("/data/") - 1, name, sizeof(name))) {
        return send_error(req, 404, "not found");
    }
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, name);
    return send_file(req, path);
}

// ------------------------------------------------------------------- thumbnails
//
// The photo grid used to point <img class="thumb"> straight at /data/<name>, so it fetched
// the ORIGINAL to draw a small square: a median of 162 KB across the bench share's 1,713
// files, all 768x1344, twelve to a page and 1.9 MB a page. This decodes through
// epd_image's existing reduced-scale path, box-downsamples, and re-encodes at about 11 KB.
//
// The encoder's own "no mutex" reasoning moved to img_resize.c with the code it explains.

// The size and quality live in img_resize.h, because the SMB import writes the same
// thumbnail as a sidecar and the two must agree.

static thumb_state_t thumb_produce(const char *path, const char *name, const char *tag,
                                   uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    // The bytes come out from under the storage lock before anything decodes them, exactly
    // as send_file() does, so the decode runs with the SPI bus free. It is also why this
    // uses epd_image_open_mem() and NEVER epd_image_open_file(): the file variant expects
    // the caller to hold the storage lock (render_image() does, app_display.c:377-380) and
    // board_storage_lock() asserts rather than waits on a second take from the same task
    // (board_storage.c:84), so a double lock is a crash. The upload caller relies on the same
    // property from the other direction: it must have DROPPED the lock before calling here.
    // EPD_IMAGE_MAX_FILE_BYTES and NOT SEND_FILE_PSRAM_MAX, which is what this used to read
    // and is a different question. send_file()'s 2 MB is the point at which it stops slurping
    // and STREAMS with the bus held -- a fallback; there is no such fallback here, so over the
    // bound this handler simply gave up, and every phone photograph was a permanently broken
    // tile with no log line anywhere (ticket 46, A0: 3.02 / 4.29 / 6.52 MB all 404 in 50 ms).
    // The ceiling that belongs here is the decoder's own, so that a file the panel can draw
    // gets a tile.
    const int64_t t0 = esp_timer_get_time();
    uint8_t *src = NULL;
    long src_len = 0;
    long file_len = 0;   // what the file measures, whether or not it was read
    bool over_max = false;
    bool no_buffer = false;
    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        if (fseek(f, 0, SEEK_END) == 0) {
            const long end = ftell(f);
            rewind(f);
            file_len = end;
            if (end > 0 && (size_t)end > EPD_IMAGE_MAX_FILE_BYTES) {
                over_max = true;
            } else if (end > 0) {
                uint8_t *buf = psram_alloc((size_t)end);
                if (buf != NULL) {
                    if (fread(buf, 1, (size_t)end, f) == (size_t)end) {
                        src = buf;
                        src_len = end;
                    } else {
                        free(buf);
                        no_buffer = true;
                    }
                } else {
                    no_buffer = true;
                }
            }
        }
        fclose(f);
    }
    board_storage_unlock();
    const int64_t t_read = esp_timer_get_time();
    if (src == NULL) {
        // THREE reasons, three answers. They used to be one 404 with no log, so a broken tile
        // could not be told from a routine one -- and the routine one really is routine, which
        // is why it stays silent while the other two do not.
        if (over_max) {
            ESP_LOGW(TAG, "%s %s: %ld bytes is over the %u decoder ceiling", tag, name, file_len,
                     (unsigned)EPD_IMAGE_MAX_FILE_BYTES);
            return THUMB_OVER_MAX;
        }
        if (no_buffer) {
            ESP_LOGW(TAG, "%s %s: no %ld-byte buffer (psram_largest=%u)", tag, name, file_len,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return THUMB_NO_BUFFER;
        }
        // Under smb_on_demand a listed name can be evicted between the list and this
        // request, so a miss here is routine rather than exceptional.
        return THUMB_MISSING;
    }

    // The whole decode-reduce-encode pipeline is img_resize.c's, so that the SMB import
    // path runs the same one rather than a second copy of it. The reasoning that used to
    // sit here -- why the decoder is asked for HALF of IMG_THUMB_MAX_EDGE, why the buffer is
    // 16-byte aligned in PSRAM, why the encoder needs no mutex -- moved with the code.
    uint8_t *rgb = NULL;
    int32_t tw = 0, th = 0;
    const esp_err_t rerr = img_resize_to_edge(src, (size_t)src_len, IMG_THUMB_MAX_EDGE, &rgb,
                                              &tw, &th);
    free(src);
    const int64_t t_scale1 = esp_timer_get_time();
    if (rerr != ESP_OK) {
        // NO_MEM is not a malformed file, and 415 said it was. A 12 MP decode needs ~0.6 MB
        // of PSRAM on top of the file, so this is reachable whenever a large render is in
        // flight; it must not read as "your photograph is corrupt".
        if (rerr == ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "%s %s: decode out of memory (psram_largest=%u)", tag, name,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return THUMB_DECODE_MEM;
        }
        return THUMB_DECODE_FAIL;
    }

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    const esp_err_t eerr = img_encode_jpeg(rgb, tw, th, IMG_THUMB_QUALITY, &enc, &enc_len);
    heap_caps_free(rgb);
    if (eerr != ESP_OK) {
        return eerr == ESP_ERR_NO_MEM ? THUMB_ENCODE_MEM : THUMB_ENCODE_FAIL;
    }

    // Raw stages rather than a total, because the total cannot say which stage to argue
    // with. read is card + PSRAM copy under the lock; resize is the header parse, the whole
    // decode and the box reduction, which the row stream interleaves and which therefore
    // cannot be separated here.
    ESP_LOGI(TAG, "%s %s %dx%d %u B: read=%lld resize=%lld encode=%lld ms", tag, name, (int)tw,
             (int)th, (unsigned)enc_len, (t_read - t0) / 1000, (t_scale1 - t_read) / 1000,
             (esp_timer_get_time() - t_scale1) / 1000);
    *out = enc;
    *out_len = enc_len;
    return THUMB_OK;
}

static esp_err_t h_thumb_serve(httpd_req_t *req)
{
    GUARD(req);

    char name[96];
    if (!request_file_name(req, sizeof("/thumb/") - 1, name, sizeof(name))) {
        return send_error(req, 404, "not found");
    }
    char path[BOARD_STORAGE_PATH_MAX];

    // The sidecar first. With smb_resize on, the SMB import writes `.thumbs/<name>.thm` for the
    // photograph from the same decode it re-encoded the photograph with (app_smb_sync.c),
    // so this is a file read instead of a 1.2-1.4 s decode. A miss is routine and falls
    // straight through to the decode below: web-UI uploads, the four factory PNGs and
    // everything imported before the setting was turned on have no sidecar and never will.
    //
    // send_file() rather than an open-coded read, so the sidecar gets the same lock
    // discipline, the same Cache-Control and the same 2 MB demotion as everything else
    // under /data; mime_for() knows the suffix. send_file() answers a miss with its own
    // 404, which is the wrong answer here because the decode below can still produce a
    // tile -- hence the probe, and hence two name resolutions on a hit. That is ~40 ms of
    // FATFS against the 1.2-1.4 s it replaces, and unpicking it means teaching send_file()
    // to take an open handle.
    app_smb_sync_sidecar_path(path, sizeof(path), name, SMB_THUMB_SUFFIX);
    board_storage_lock();
    board_storage_prepare_access();
    FILE *tf = fopen(path, "rb");
    const bool sidecar = (tf != NULL);
    if (tf != NULL) {
        fclose(tf);
    }
    board_storage_unlock();
    if (sidecar) {
        return send_file(req, path);
    }

    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, name);

    uint8_t *out = NULL;
    size_t out_len = 0;
    const thumb_state_t st = thumb_produce(path, name, "thumb", &out, &out_len);
    switch (st) {
    case THUMB_OK:
        break;
    case THUMB_OVER_MAX:
        return send_error(req, 413, "too large to thumbnail");
    case THUMB_NO_BUFFER:
    case THUMB_DECODE_MEM:
        return send_error(req, 503, "no memory");
    case THUMB_DECODE_FAIL:
        return send_error(req, 415, "cannot decode");
    case THUMB_ENCODE_MEM:
        return send_error(req, 503, "encode failed");
    case THUMB_ENCODE_FAIL:
        return send_error(req, 500, "encode failed");
    case THUMB_MISSING:
    default:
        // 404 leaves the page with its broken-image behaviour, which is already what /data/
        // gives it.
        return send_error(req, 404, "not found");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
    const esp_err_t err = send_bytes(req, out, out_len);
    free(out);
    return err;
}

// GET /api/system/info -- the read-only figures the Settings page's System panel shows. Added
// 2026-09-18 with the tabbed page (the tabbed-page plan): the SSID and the IP used to
// be in a status bar that the tab bar replaced, and once they had to live somewhere the rest of
// what the device can say about itself was worth putting beside them.
//
// EVERY VALUE HERE IS RAW -- bytes, milliseconds, dBm, a UNIX epoch and a timezone offset in
// minutes. The page formats them. That is what the rest of this API does (`/api/storage` sends
// bytes, `/api/battery` millivolts), and it keeps a threshold out of the firmware: a derived
// boolean is a second thing that can be wrong, and finding out costs a rebuild and a flash
// (docs/agents/method.md).
//
// TWO THINGS IT DELIBERATELY DOES NOT REPORT:
//
//   * the photograph count, which needs scan_photos() under s_scan_mutex -- a directory read on
//     a server that serves one request at a time -- while the page already has the number from
//     GET /api/photos/list;
//   * either mirror's state, which is GET /api/smb/status and GET /api/gphotos/status. The panel
//     reads those two alongside this one rather than have the same state assembled twice.
static esp_err_t h_system_info(httpd_req_t *req)
{
    GUARD(req);

    app_settings_t s;
    app_settings_get(&s);

    board_wifi_status_t st;
    board_wifi_status(&st);

    board_power_t pwr = {0};
    const bool pwr_ok = (board_power_read(&pwr) == ESP_OK);

    // The board's own ambient sensor, and THIS IS ITS FIRST CALLER IN THE APPLICATION -- until
    // now only bring-up and the colour-chart run read it. About 10 ms of I2C including the
    // conversion wait, on the same bus /api/battery already uses, so the precedent is there --
    // but they are 10 ms in which nothing else is served, which is why the panel does not poll.
    // A failure has to reach the page as ABSENT rather than as 0 degrees, so both fields are
    // omitted together: ESP_ERR_NOT_SUPPORTED on a board with no sensor, ESP_ERR_INVALID_STATE
    // where it was never probed, and a bus error otherwise.
    float temp_c = 0.0f, humidity_pct = 0.0f;
    const bool sht_ok = (board_sht40_read(&temp_c, &humidity_pct) == ESP_OK);

    uint64_t total = 0, freeb = 0;
    board_storage_usage(&total, &freeb);

    app_clock_status_t ck;
    app_clock_get_status(&ck);

    char unit[7];
    board_wifi_unit_id(unit, sizeof(unit), false);

    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON *r = cJSON_CreateObject();

    cJSON *dev = cJSON_AddObjectToObject(r, "device");
    cJSON_AddStringToObject(dev, "device_name", s.device_name);
    cJSON_AddStringToObject(dev, "unit_id", unit);
    // The ONE board-conditional string in src/app/. Everything else that differs between the two
    // boards reaches the page as a number it already asks for -- panel_width and panel_height --
    // so this is a label, not a fact anything computes with. If the layer map ever forbids it,
    // it becomes a board_display_name() in board.h and nothing else changes.
#if defined(BOARD_M5PAPER_COLOR)
    cJSON_AddStringToObject(dev, "board", "M5Paper Color (EL040EF1)");
#elif defined(BOARD_RETERMINAL_E1002)
    cJSON_AddStringToObject(dev, "board", "reTerminal E1002 (ED2208-GCA)");
#endif
    cJSON_AddStringToObject(dev, "fw_version", desc->version);
    cJSON_AddStringToObject(dev, "fw_date", desc->date);
    cJSON_AddStringToObject(dev, "fw_time", desc->time);
    cJSON_AddStringToObject(dev, "idf_version", desc->idf_ver);
    cJSON_AddNumberToObject(dev, "uptime_ms", (double)(esp_timer_get_time() / 1000));

    // Ticket 61. `running` and `state` are how a person tells an OTA-booted image from a USB-flashed
    // one ("undefined") and whether it is still on trial ("pending").
    app_ota_status_t ota;
    app_ota_get_status(&ota);
    cJSON *o = cJSON_AddObjectToObject(r, "ota");
    cJSON_AddStringToObject(o, "running", ota.running);
    cJSON_AddStringToObject(o, "state", ota.state);
    cJSON_AddStringToObject(o, "board", ota.board);
    cJSON_AddBoolToObject(o, "writing", ota.writing);
    cJSON_AddNumberToObject(o, "written", (double)ota.written);
    cJSON_AddStringToObject(o, "last_result", ota.last_result);

    cJSON *net = cJSON_AddObjectToObject(r, "network");
    cJSON *sta = cJSON_AddObjectToObject(net, "sta");
    cJSON_AddBoolToObject(sta, "connected", st.sta_connected);
    cJSON_AddStringToObject(sta, "ssid", st.ssid);
    cJSON_AddStringToObject(sta, "ip", st.sta_connected ? st.ip : "");
    // 0 rather than absent when not joined, and 0 dBm is not a plausible reading, so the page
    // reads `connected` and not this number to decide whether to show it.
    cJSON_AddNumberToObject(sta, "rssi", st.rssi);
    cJSON *ap = cJSON_AddObjectToObject(net, "ap");
    cJSON_AddStringToObject(ap, "ssid", st.ap_ssid);
    cJSON_AddStringToObject(ap, "ip", st.ap_ip);
    cJSON_AddNumberToObject(ap, "clients", st.ap_clients);
    // Whether the radio carries WPA2 NOW, which is the same question the portal redirect asks
    // before it will part with the API token (ticket 30 step 2) -- not whether FRAME_AP_OPEN was
    // defined at build time. A half-applied config answers those two differently.
    cJSON_AddBoolToObject(ap, "secured", board_wifi_ap_secured());
    // Ticket 67. **`up` and `secured` are different questions and both are needed**: the access
    // point is down between windows, and a `secured` true then means "the password is stored and the
    // next window will carry it", not "there is something on the air to join". `window_s` is the
    // seconds left before it closes itself, 0 when it is already down -- and it stops counting down
    // while a client is associated, so a steady number means somebody is using it.
    cJSON_AddBoolToObject(ap, "up", board_wifi_ap_is_up());
    cJSON_AddNumberToObject(ap, "window_s", board_wifi_ap_window_left_s());

    cJSON *sens = cJSON_AddObjectToObject(r, "sensors");
    if (sht_ok) {
        cJSON_AddNumberToObject(sens, "temp_c", temp_c);
        cJSON_AddNumberToObject(sens, "humidity_pct", humidity_pct);
    }
    cJSON_AddNumberToObject(sens, "vbat_mv", pwr_ok ? pwr.vbat_mv : 0);
    // A board that cannot answer these reads them false, so `!vin_present` is NOT "on battery"
    // (board.h:64-67). Both are sent as they come back and the page shows them as flags rather
    // than concluding anything from one of them.
    cJSON_AddBoolToObject(sens, "vin_present", pwr_ok && pwr.vin_present);
    cJSON_AddBoolToObject(sens, "bat_present", pwr_ok && pwr.bat_present);

    cJSON *sto = cJSON_AddObjectToObject(r, "storage");
    cJSON_AddNumberToObject(sto, "total", (double)total);
    cJSON_AddNumberToObject(sto, "used", (double)(total - freeb));
    cJSON_AddNumberToObject(sto, "free", (double)freeb);
    // The same pair h_storage() sends and for the same reason: `media` is what the mount
    // believes and `card_present` is the detect line, which is the only reading that can
    // disagree with it.
    cJSON_AddStringToObject(
        sto, "media", board_storage_get_media() == BOARD_STORAGE_MEDIA_SD ? "sd" : "flash");
    cJSON_AddBoolToObject(sto, "card_present", board_card_present());
    // Whether POST /api/storage/usb can work here; the page shows its button off this (ticket 09).
    cJSON_AddBoolToObject(sto, "usb_msc", BOARD_HAS_USB_MSC);

    cJSON *clk = cJSON_AddObjectToObject(r, "clock");
    cJSON_AddBoolToObject(clk, "synced", ck.synced);
    // A UNIX epoch and the offset, not a formatted string: the page has a locale and this does
    // not. Sent whatever `synced` says, because time(NULL) before a sync is seconds since boot
    // and reading that as a date is exactly what `synced` is there to prevent.
    cJSON_AddNumberToObject(clk, "epoch", (double)time(NULL));
    cJSON_AddNumberToObject(clk, "tz_offset_minutes", s.tz_offset_minutes);
    // -1 rather than app_clock's own UINT32_MAX sentinel, which is the convention the two mirror
    // routes already use for "no run to date" (`last_run_age_s`). It matters here because
    // `synced` true WITH no age is a real and documented state, not a contradiction: a software
    // reset keeps the SoC's clock but not the flag, so app_clock trusts the clock and leaves the
    // age unset until this boot's first sync lands (app_clock.c:219-237). Passing the sentinel
    // through as a number made the page draw it as "1193046 h ago" -- seen on hardware
    // 2026-09-18, 33 s into a boot, before the first attempt was due.
    cJSON_AddNumberToObject(clk, "since_sync_s",
                            ck.since_sync_s == UINT32_MAX ? -1 : (double)ck.since_sync_s);
    cJSON_AddNumberToObject(clk, "local_hour", ck.local_hour);

    cJSON *pan = cJSON_AddObjectToObject(r, "panel");
    cJSON_AddNumberToObject(pan, "width", EPD_WIDTH);
    cJSON_AddNumberToObject(pan, "height", EPD_HEIGHT);
    cJSON_AddStringToObject(pan, "native",
                            panel_is_native_landscape() ? "landscape" : "portrait");
    // Both, for the reason add_mode_config() gives: the number is what the frame is set to and the
    // word is what it looks like. `rotation` is 0..3 since ticket 69.
    cJSON_AddNumberToObject(pan, "rotation", s.rotation);
    cJSON_AddStringToObject(pan, "orientation", orientation_name(s.rotation));

    // Panel maintenance (ticket 68), under `panel` because that is what it is about. This is the
    // status side of POST /api/panel/maintenance: the page polls it to draw a step counter, and it
    // rides an existing route rather than getting one of its own because the handler table is full
    // to its configured limit -- see app_server_start().
    //
    // `window_closed` and `parked` are separate on purpose. The first is the schedule's own answer
    // and the second is whether this module acted on it, so `window_closed` true with `parked` false
    // is the standby switched off rather than a fault, and `parked` true with the window open would
    // be a contradiction worth seeing.
    {
        app_maint_status_t mt;
        app_maint_get_status(&mt);
        cJSON *mn = cJSON_AddObjectToObject(pan, "maintenance");
        cJSON_AddBoolToObject(mn, "running", mt.running);
        // Which sequence: the ten-flat course, or the seven-step clear cycle. `steps` below is the
        // ACTIVE one's length, so a client must not assume ten.
        cJSON_AddBoolToObject(mn, "clearing", mt.clearing);
        cJSON_AddBoolToObject(mn, "spread", mt.spread);
        cJSON_AddNumberToObject(mn, "step", mt.step);
        cJSON_AddNumberToObject(mn, "steps", mt.steps);
        cJSON_AddStringToObject(mn, "colour", mt.colour);
        cJSON_AddNumberToObject(mn, "next_step_s", mt.next_step_s);
        cJSON_AddNumberToObject(mn, "courses", mt.courses);
        cJSON_AddBoolToObject(mn, "window_closed", mt.window_closed);
        cJSON_AddBoolToObject(mn, "parked", mt.parked);
    }

    // Internal RAM is the scarce resource here, not PSRAM, and `dma_largest` under about 2 KB
    // makes lwIP drop arriving frames with a perfectly healthy console. The only way to read
    // that before this route was a serial cable. Raw, and with no colour or verdict attached --
    // see the header comment.
    cJSON *diag = cJSON_AddObjectToObject(r, "diag");
    cJSON_AddNumberToObject(diag, "int_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(diag, "int_min",
                            (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(diag, "dma_largest",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(diag, "psram_free",
                            (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_system_reset(httpd_req_t *req)
{
    GUARD(req);

    // FR-8.2 restores the defaults, shows the guide image and powers the device off.
    // The power-off is refused by the driver while -DBOARD_NO_POWER_OFF is set, and a
    // half-done factory reset -- settings wiped, device still on, no guide image -- is
    // worse than none. Ticket 16 implements it whole.
    // Coded, because this one DOES reach a reader: executeReset() in index.html shows it as a toast
    // after confirmReset()'s dialog, which is the most alarming dialog on the page.
    return send_error_code(req, 501, "reset.not_implemented",
                           "factory reset not implemented in this build");
}

// --------------------------------------------------------- the panel maintenance course
//
// Ticket 68. `{"action": "start"}`, `{"action": "stop"}`, `{"action": "white"}` or
// `{"action": "clear"}`.
//
// **`clear` is the escalation of `white`**, added 2026-09-20 when the operator asked whether the
// existing mechanisms could deal with mild ghosting: seven refreshes, white/black alternating, three
// black passes bracketed by white, ending white. One white is *measured* to remove a plainly legible
// 1-bit ghost completely (ticket 30), so this is for the case beyond that — and **that case has never
// been produced on this bench, because the operator declined to manufacture a ghost to test it.** It
// is a mechanism, not a measured remedy.
//
// **`white` parks the panel on white NOW**, added 2026-09-20 at the operator's request. It is the
// same one-shot the window edge takes and it exists because there was no other way to ask for it:
// GooDisplay's own precaution is to ship and store the panel showing a fully white image
// (`docs/research/reference-source-review.md`), and before this the only routes to one were to run
// the whole ten-flat course or to empty the album. It does **not** touch `standby_white`, the
// schedule or anything persisted — a frame about to be switched off or carried somewhere wants one
// white refresh, not a new setting.
//
// **IT RETURNS IMMEDIATELY AND MUST.** This server serves ONE REQUEST AT A TIME, so a handler that
// drove the course synchronously would take the entire Web UI down for the length of it -- two and a
// half minutes for the manual form and an hour for the scheduled one, which from a browser is
// indistinguishable from the frame having crashed. app_maint_start_course() posts a flag and the
// application task's tick drives the ten refreshes.
//
// No `request_from_ap()` guard, unlike the mirror routes: nothing here is a credential, and refusing
// it over the frame's own access point would mean a frame that cannot be serviced by somebody
// standing next to it with a phone, which is the one situation this mode is for.
static esp_err_t h_panel_maint(httpd_req_t *req)
{
    GUARD(req);

    char body[128];
    cJSON *j = read_json_body(req, body, sizeof(body));
    if (!j) {
        return send_error(req, 400, "bad json");
    }
    const cJSON *action = cJSON_GetObjectItem(j, "action");
    static const char *const WHAT = "action must be \"start\", \"stop\", \"white\" or \"clear\"";
    if (!cJSON_IsString(action) || !action->valuestring) {
        cJSON_Delete(j);
        return send_error(req, 400, WHAT);
    }
    const bool start = strcmp(action->valuestring, "start") == 0;
    const bool stop = strcmp(action->valuestring, "stop") == 0;
    const bool white = strcmp(action->valuestring, "white") == 0;
    const bool clear = strcmp(action->valuestring, "clear") == 0;
    // Read before the body is freed, and defaulted to one rather than refused when absent: the
    // operator asked for a repeat count as an option, not as a required field, and one cycle is what
    // `clear` meant before it existed. Validated by app_maint_start_clear(), which is the only place
    // that knows the bound.
    const cJSON *cyc = cJSON_GetObjectItem(j, "cycles");
    const int cycles = cJSON_IsNumber(cyc) ? cyc->valueint : 1;
    cJSON_Delete(j);
    if (!start && !stop && !white && !clear) {
        return send_error(req, 400, WHAT);
    }

    if (clear) {
        const esp_err_t cerr = app_maint_start_clear(cycles);
        // **A bad `cycles` is a 400 and a busy panel is a 503**, and they have to be told apart:
        // one is the client's mistake and the other is a state it can wait out. Collapsing them
        // would make a retry loop spin on a body that will never be accepted.
        if (cerr == ESP_ERR_INVALID_ARG) {
            return send_error(req, 400, "cycles must be 1..5");
        }
        if (cerr != ESP_OK) {
            return send_error_code(req, 503, "panel.busy",
                                   "a colour course or clear is already running");
        }
    } else if (start) {
        const esp_err_t serr = app_maint_start_course(false);
        if (serr != ESP_OK) {
            // **503 and not 409**, which is not in send_error()'s ladder and would therefore go out
            // as a 500 -- the defect that ladder's comment records having shipped twice already.
            return send_error_code(req, 503, "panel.busy",
                                   "a colour course or clear is already running");
        }
    } else if (white) {
        // Refused while a course runs, for the same reason and with the same code: the display has
        // ONE pending slot, so this would discard whichever flat was waiting and the course would
        // silently show nine colours.
        if (app_maint_course_running()) {
            return send_error_code(req, 503, "panel.busy",
                                   "a colour course or clear is already running");
        }
        const esp_err_t werr = app_display_request_blank();
        if (werr != ESP_OK) {
            return send_error_code(req, 503, "panel.refused",
                                   "the panel would not take the request");
        }
    } else {
        app_maint_stop_course();
    }

    app_maint_status_t mt;
    app_maint_get_status(&mt);
    cJSON *r = cJSON_CreateObject();
    // "queued", like the mirror's sync: neither the course nor the white render has begun when this
    // is sent, and saying "started" would be a claim this handler cannot make.
    cJSON_AddStringToObject(r, "status", stop ? "stopping" : "queued");
    // The QUEUED sequence's length for a start, not the status's — the tick has not begun it yet, so
    // `mt.steps` still describes whatever ran last and a clear answered "steps": 10. `white` is one
    // refresh and not a sequence, and `stop` is about the one already running, so both take the
    // status. Found on hardware 2026-09-20.
    cJSON_AddNumberToObject(r, "steps", (start || clear)
                                            ? app_maint_sequence_steps(clear, cycles)
                                            : mt.steps);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// --------------------------------------------------------------- the SMB mirror
//
// Ticket 25, revised by ticket 29. The NAS credentials are the most valuable thing on
// this device -- they are somebody else's machine, not this one -- so these four routes
// carry a guard of their own on top of GUARD():
//
//   1. No credential is ever returned, in any form. has_password and has_user are how
//      the page knows what is stored without being handed it. Ticket 29 extended this
//      from the password to the username and domain: the account name plus the host and
//      share is most of what an attack on the NAS itself needs, and the page has no use
//      for it beyond showing that a value exists.
//   2. The two writes are refused for a client that came in over the softAP.
//
// **Guard 2 used to be station_is_up(), and it did not do this.** That function asked
// whether THE FRAME had a station link, not whether THIS CLIENT arrived over one -- and
// the frame having a station link is the normal state, so every client on the open AP
// passed a guard whose comment said it was keeping them out. It was written to mean
// "being on the home network is the minimum evidence that the owner configured this
// device"; request_from_ap() is that sentence implemented. Found while adding ticket 29,
// which also makes it defence in depth rather than the only thing standing there.

static esp_err_t h_smb_config_get(httpd_req_t *req)
{
    GUARD(req);

    app_settings_t s;
    app_settings_get(&s);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", s.smb_enabled);
    // host, share and path stay visible: they say what the mirror points at, which the
    // page has to render and which is not a credential.
    cJSON_AddStringToObject(r, "host", s.smb_host);
    cJSON_AddStringToObject(r, "share", s.smb_share);
    cJSON_AddStringToObject(r, "path", s.smb_path);
    cJSON_AddBoolToObject(r, "has_user", s.smb_user[0] != '\0');
    cJSON_AddBoolToObject(r, "has_domain", s.smb_domain[0] != '\0');
    cJSON_AddBoolToObject(r, "has_password", s.smb_password[0] != '\0');
    // Ticket 37 Phase 3. Served here rather than with the display settings because it changes
    // what the MIRROR does, not how a photograph is drawn.
    cJSON_AddBoolToObject(r, "on_demand", s.smb_on_demand);
    cJSON_AddBoolToObject(r, "resize", s.smb_resize);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static esp_err_t h_smb_config_set(httpd_req_t *req)
{
    GUARD(req);

    if (request_from_ap(req)) {
        return send_error_code(req, 403, "nas.ap_scope",
                               "configure the share from your home network, not the frame's own "
                               "access point");
    }

    char body[512];
    cJSON *j = read_json_body(req, body, sizeof(body));
    if (!j) {
        return send_error(req, 400, "bad json");
    }

    // Validate everything before applying anything, as h_mode_cfg_set does: applying as
    // we parse means a bad share leaves the host already stored while the client is told
    // 400, and the page and the device then disagree about what happened.
    const cJSON *host = cJSON_GetObjectItem(j, "host");
    const cJSON *share = cJSON_GetObjectItem(j, "share");
    const cJSON *path = cJSON_GetObjectItem(j, "path");
    const cJSON *user = cJSON_GetObjectItem(j, "user");
    const cJSON *password = cJSON_GetObjectItem(j, "password");
    const cJSON *domain = cJSON_GetObjectItem(j, "domain");
    const cJSON *enabled = cJSON_GetObjectItem(j, "enabled");

    const char *host_s = cJSON_IsString(host) ? host->valuestring : NULL;
    const char *share_s = cJSON_IsString(share) ? share->valuestring : NULL;
    const char *path_s = cJSON_IsString(path) ? path->valuestring : NULL;
    const char *user_s = cJSON_IsString(user) ? user->valuestring : NULL;
    // An absent password leaves the stored one alone; an empty string clears it. The two
    // are different requests, and the page relies on the first to save a changed host
    // without ever being given the password to send back.
    const char *pass_s = cJSON_IsString(password) ? password->valuestring : NULL;
    const char *domain_s = cJSON_IsString(domain) ? domain->valuestring : NULL;

    app_settings_t cur;
    app_settings_get(&cur);
    const bool on = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : cur.smb_enabled;

    // Enabling a mirror with nowhere to point it would leave the sync task failing every
    // hour and reporting a connect error for a share nobody configured.
    const char *eff_host = host_s ? host_s : cur.smb_host;
    const char *eff_share = share_s ? share_s : cur.smb_share;
    if (on && (eff_host[0] == '\0' || eff_share[0] == '\0')) {
        cJSON_Delete(j);
        return send_error_code(req, 400, "nas.host_required",
                               "host and share are required to enable the mirror");
    }

    const cJSON *on_demand = cJSON_GetObjectItem(j, "on_demand");
    const cJSON *resize = cJSON_GetObjectItem(j, "resize");

    const esp_err_t set = app_settings_set_smb(host_s, share_s, path_s, user_s, pass_s,
                                               domain_s, on);
    // Its own key and its own setter, so it is not silently reset by a form that does not carry
    // it -- the seven mirror fields move together, this does not move with them.
    if (set == ESP_OK && cJSON_IsBool(on_demand) &&
        cJSON_IsTrue(on_demand) != cur.smb_on_demand) {
        app_settings_set_smb_on_demand(cJSON_IsTrue(on_demand));
    }
    if (set == ESP_OK && cJSON_IsBool(resize) && cJSON_IsTrue(resize) != cur.smb_resize) {
        app_settings_set_smb_resize(cJSON_IsTrue(resize));
    }
    cJSON_Delete(j);
    if (set == ESP_ERR_INVALID_ARG) {
        return send_error(req, 400, "a field is too long");
    }
    if (set != ESP_OK) {
        return send_error(req, 500, "could not save");
    }
    return h_smb_config_get(req);
}

static esp_err_t h_smb_sync(httpd_req_t *req)
{
    GUARD(req);

    if (request_from_ap(req)) {
        return send_error_code(req, 403, "nas.ap_scope.sync",
                               "start a sync from your home network, not the frame's own access "
                               "point");
    }
    app_settings_t s;
    app_settings_get(&s);
    if (!s.smb_enabled) {
        return send_error_code(req, 400, "nas.disabled", "the mirror is disabled");
    }

    // Returns immediately: the sync stops httpd for the duration of a window, so an
    // answer that waited for it could not be sent.
    app_smb_sync_request();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "queued");
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// Connect, tree-connect, disconnect -- no listing and no transfers, so it answers "are these
// settings right" without a mirroring window. It is the only SMB route that is NOT gated on
// smb_enabled, because the reason to test is to find out before switching the mirror on.
static esp_err_t h_smb_test(httpd_req_t *req)
{
    GUARD(req);

    if (request_from_ap(req)) {
        return send_error_code(req, 403, "nas.ap_scope.test",
                               "test the share from your home network, not the frame's own access "
                               "point");
    }

    app_settings_t s;
    app_settings_get(&s);
    if (s.smb_host[0] == '\0' || s.smb_share[0] == '\0') {
        return send_error_code(req, 400, "nas.not_configured", "no host or share configured");
    }

    // Returns immediately for the same reason POST /api/smb/sync does, though not the same
    // mechanism: this one does not stop httpd, but it does have to wait for any run in
    // progress to release the single libsmb2 session, and that can be several windows away.
    // The result arrives in GET /api/smb/status as test_state.
    app_smb_sync_test_request();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "queued");
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

static const char *smb_test_state_str(app_smb_test_state_t s)
{
    switch (s) {
    case APP_SMB_TEST_IDLE:
        return "idle";
    case APP_SMB_TEST_QUEUED:
        return "queued";
    case APP_SMB_TEST_RUNNING:
        return "running";
    case APP_SMB_TEST_DONE:
        return "done";
    case APP_SMB_TEST_REFUSED:
        return "refused";
    }
    return "unknown";
}

static esp_err_t h_smb_status(httpd_req_t *req)
{
    GUARD(req);

    app_smb_sync_status_t st;
    app_smb_sync_get_status(&st);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", st.enabled);
    cJSON_AddBoolToObject(r, "syncing", st.syncing);
    cJSON_AddBoolToObject(r, "capped", st.capped);
    cJSON_AddBoolToObject(r, "listing_truncated", st.listing_truncated);
    cJSON_AddNumberToObject(r, "files_total", st.files_total);
    cJSON_AddNumberToObject(r, "files_done", st.files_done);
    cJSON_AddNumberToObject(r, "files_mirrored", st.files_mirrored);
    cJSON_AddNumberToObject(r, "bytes_mirrored", (double)st.bytes_mirrored);
    // The share catalogue (ticket 37). `catalog_count` is how many photographs EXIST to choose
    // from and `files_mirrored` is how many are on the card; the two being different is the
    // design working, not a fault, and until now the page had no way to say so.
    cJSON_AddNumberToObject(r, "catalog_count", st.catalog_count);
    cJSON_AddNumberToObject(r, "catalog_folders", st.catalog_folders);
    cJSON_AddBoolToObject(r, "catalog_truncated", st.catalog_truncated);

    // On-demand (ticket 37 Phase 3). `on_demand` says which of two quite different behaviours
    // the rest of this object describes: with it on, files_mirrored is a CACHE bounded at
    // SMB_SYNC_CACHE_FILES rather than a mirror of a folder, and it being far below
    // catalog_count is the design working. `want_pending` is whether the fetcher is keeping up
    // with the selection. `evicted` was meant to distinguish a working cache from a thrashing
    // one and cannot -- eviction is 1:1 with fetching either way (ticket 44). The pair that
    // can is `fetch_total` against how many photographs were shown. `fetch_distinct` counts
    // distinct catalogue INDICES, which the hourly refresh renumbers -- it is a lower bound on
    // distinct photographs, not a re-fetch count (tickets 44 and 45). Served raw either way:
    // there is deliberately no "thrashing" flag here to be wrong.
    cJSON_AddBoolToObject(r, "on_demand", st.on_demand);
    cJSON_AddBoolToObject(r, "resize", app_settings_smb_resize());
    cJSON_AddNumberToObject(r, "want_pending", st.want_pending);
    cJSON_AddNumberToObject(r, "evicted", st.evicted);
    cJSON_AddNumberToObject(r, "fetch_total", st.fetch_total);
    cJSON_AddNumberToObject(r, "fetch_distinct", st.fetch_distinct);
    cJSON_AddNumberToObject(r, "cache_files", SMB_SYNC_CACHE_FILES);
    // Ticket 27. NOT a count of orphans -- it counts the factory images and every Web-UI upload
    // too, because a mirrored name and an uploaded one are not distinguishable, which is the same
    // fact that makes "delete what the manifest does not own" unsafe. It is a baseline plus the
    // leak, so the reading is the number MOVING with no upload behind it. `unowned_exact` false
    // makes it a lower bound. The long form is in app_smb_sync.h beside the field; served raw,
    // with no derived verdict here to be wrong.
    cJSON_AddNumberToObject(r, "unowned_files", st.unowned_files);
    cJSON_AddBoolToObject(r, "unowned_exact", st.unowned_exact);
    cJSON_AddStringToObject(r, "last_file", st.last_file);
    // Classified, so the page can say "wrong share name" instead of "failed".
    cJSON_AddStringToObject(r, "last_error", board_smb_err_str(st.last_error));
    if (st.last_sync_ms == UINT32_MAX) {
        cJSON_AddNullToObject(r, "last_sync_ms");
    } else {
        cJSON_AddNumberToObject(r, "last_sync_ms", (double)st.last_sync_ms);
    }
    // The connect test, reported apart from last_error: "the settings are wrong" and "the
    // last mirroring run failed" are different statements, and a page that merged them would
    // show a stale sync failure as the verdict on a share the user just fixed. test_error is
    // meaningful only at test_state "done".
    cJSON_AddStringToObject(r, "test_state", smb_test_state_str(st.test_state));
    cJSON_AddStringToObject(r, "test_error", board_smb_err_str(st.test_error));
    cJSON_AddNumberToObject(r, "test_ms", (double)st.test_ms);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// Google Photos (ticket 60, FR-10.4): whether the frame is still getting photographs from its
// album, in a form somebody away from the site can read. Nobody at the frame can read a log, and
// the failure this exists for is silent -- a frame that simply stops changing.
//
// AGES, NOT UPTIMES, and computed here: an uptime means nothing to a reader who does not know when
// the frame last booted. -1 means "not since boot". `http_status` 404 is an album that is
// account-shared rather than link-shared (FR-10.1). Served raw, with no verdict to be wrong.
static esp_err_t h_gphotos_status(httpd_req_t *req)
{
    GUARD(req);

    app_gphotos_status_t st;
    app_gphotos_sync_get_status(&st);
    const int64_t now_ms = esp_timer_get_time() / 1000;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", st.enabled);
    cJSON_AddBoolToObject(r, "running", st.running);
    cJSON_AddNumberToObject(r, "last_run_age_s",
                            st.last_run_ms < 0 ? -1 : (double)((now_ms - st.last_run_ms) / 1000));
    cJSON_AddNumberToObject(r, "owned", st.owned);
    cJSON_AddNumberToObject(r, "fetched", st.fetched);
    cJSON_AddNumberToObject(r, "failed", st.failed);
    cJSON_AddNumberToObject(r, "deleted", st.deleted);
    // On demand (the operator's ruling of 2026-09-13): the slideshow asks for one photograph at a
    // time. fetched/failed/deleted/evicted are since boot.
    cJSON_AddNumberToObject(r, "want_pending", st.want_pending);
    cJSON_AddNumberToObject(r, "evicted", st.evicted);

    // One entry per album slot, configured or not, so the page can index them by slot. The
    // top-level album_items is their sum, and last_result / http_status are the first configured
    // slot's that is not "ok" (else "ok"), for a reader that wants one line.
    char album[APP_SETTINGS_GPHOTOS_ALBUM_SIZE];
    cJSON *arr = cJSON_AddArrayToObject(r, "albums");
    unsigned items = 0;
    app_gphotos_result_t worst = APP_GPHOTOS_NEVER;
    int worst_http = 0;
    bool any = false;
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
        const app_gphotos_album_status_t *as = &st.albums[s];
        app_settings_gphotos_album(s, album, sizeof(album));
        const bool configured = album[0] != '\0';
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "configured", configured);
        cJSON_AddStringToObject(o, "last_result", app_gphotos_result_str(as->last_result));
        cJSON_AddNumberToObject(o, "http_status", as->last_http_status);
        cJSON_AddNumberToObject(o, "last_ok_age_s",
                                as->last_ok_ms < 0 ? -1
                                                   : (double)((now_ms - as->last_ok_ms) / 1000));
        cJSON_AddNumberToObject(o, "items", as->items);
        cJSON_AddBoolToObject(o, "over_cap", as->over_cap);
        cJSON_AddNumberToObject(o, "album_bytes", as->album_bytes);
        // A new link that could not be used, and the last good one put back in its place.
        cJSON_AddBoolToObject(o, "reverted", as->reverted);
        cJSON_AddStringToObject(o, "reverted_reason",
                                as->reverted ? app_gphotos_result_str(as->reverted_result) : "");
        cJSON_AddNumberToObject(o, "reverted_http", as->reverted ? as->reverted_http : 0);
        cJSON_AddItemToArray(arr, o);

        items += as->items;
        if (configured) {
            if (!any || (worst == APP_GPHOTOS_OK && as->last_result != APP_GPHOTOS_OK)) {
                worst = as->last_result;
                worst_http = as->last_http_status;
            }
            any = true;
        }
    }
    cJSON_AddNumberToObject(r, "album_items", items);
    cJSON_AddStringToObject(r, "last_result", app_gphotos_result_str(worst));
    cJSON_AddNumberToObject(r, "http_status", worst_http);

    // The scrape rules (the scrape-rules plan): the set in force, one on trial, the
    // highest refused, and what the last rule file offered came to.
    app_gprules_status_t rs;
    app_gphotos_rules_get_status(&rs);
    cJSON *ro = cJSON_AddObjectToObject(r, "rules");
    cJSON_AddBoolToObject(ro, "key_compiled_in", rs.key_compiled_in);
    cJSON_AddNumberToObject(ro, "seq", rs.adopted_seq);
    cJSON_AddStringToObject(ro, "source", app_gphotos_rules_source_str(rs.adopted_source));
    cJSON_AddNumberToObject(ro, "trial_seq", rs.candidate_seq);
    cJSON_AddNumberToObject(ro, "rejected_seq", rs.rejected_seq);
    cJSON_AddStringToObject(ro, "last_check", rs.last_check);
    cJSON_AddStringToObject(ro, "last_reason", rs.last_reason);
    cJSON_AddNumberToObject(ro, "last_http", rs.last_http);
    cJSON_AddNumberToObject(ro, "last_check_age_s",
                            rs.last_check_ms < 0 ? -1
                                                 : (double)((now_ms - rs.last_check_ms) / 1000));
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// FR-10.1's one setting: the album link, entered from a phone.
//
// THE LINK IS A BEARER CAPABILITY, so GET never returns it -- only whether one is set and the host
// it points at, which is what the page needs to say "set" without handing the album to anybody who
// can read this route.
static void album_host(const char *album, char *out, size_t size)
{
    out[0] = '\0';
    const char *p = strstr(album, "://");
    if (!p) {
        return;
    }
    p += 3;
    size_t n = strcspn(p, "/?");
    if (n >= size) {
        n = size - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

static esp_err_t h_gphotos_config_get(httpd_req_t *req)
{
    GUARD(req);

    char album[APP_SETTINGS_GPHOTOS_ALBUM_SIZE];
    char host[64];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", app_settings_gphotos_enabled());
    cJSON *arr = cJSON_AddArrayToObject(r, "albums");
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
        app_settings_gphotos_album(s, album, sizeof(album));
        album_host(album, host, sizeof(host));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "has_album", album[0] != '\0');
        cJSON_AddStringToObject(o, "album_host", host);
        cJSON_AddItemToArray(arr, o);
    }
    // A secret Gist's raw URL is a capability too: its host only, and whether it is the build's.
    bool rules_default = true;
    app_gphotos_rules_get_url(album, sizeof(album), &rules_default);
    album_host(album, host, sizeof(host));
    cJSON_AddStringToObject(r, "rules_url_host", host);
    cJSON_AddBoolToObject(r, "rules_url_is_default", rules_default);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// Four links of APP_SETTINGS_GPHOTOS_ALBUM_SIZE, a rule URL, a rule file -- twice its size, since
// JSON escapes its line breaks and quotes -- and the JSON around them.
#define GPHOTOS_CONFIG_BODY_MAX                                                               \
    (APP_SETTINGS_GPHOTOS_SLOTS * APP_SETTINGS_GPHOTOS_ALBUM_SIZE + APP_GPHOTOS_RULES_URL_SIZE + \
     2 * GPHOTOS_RULES_FILE_MAX + 512)

#define GPHOTOS_SHORT_PREFIX "https://photos.app.goo.gl/"
#define GPHOTOS_SHARE_PREFIX "https://photos.google.com/share/"

// NULL when `s` is acceptable. Only the two forms a phone produces are accepted: the short link
// Google Photos' "Create link" shares, and the share URL it resolves to. Anything else is refused
// rather than fetched -- the device would otherwise make HTTPS requests to whatever URL somebody
// holding the token typed. A share URL without `key=` is an album shared with ACCOUNTS, which
// answers every request the frame can make with 404 (measured 2026-09-12), so it is refused here,
// with the fix, rather than failing an hour later.
static const char *album_problem(const char *s)
{
    if (s[0] == '\0') {
        return NULL; // clearing the link is allowed
    }
    if (strlen(s) >= APP_SETTINGS_GPHOTOS_ALBUM_SIZE) {
        return "the link is too long";
    }
    if (strncmp(s, GPHOTOS_SHORT_PREFIX, sizeof(GPHOTOS_SHORT_PREFIX) - 1) == 0) {
        return NULL;
    }
    if (strncmp(s, GPHOTOS_SHARE_PREFIX, sizeof(GPHOTOS_SHARE_PREFIX) - 1) == 0) {
        return strstr(s, "key=") ? NULL
                                  : "this album is shared with people, not by link: in Google "
                                    "Photos use Share, then Create link, and paste that link";
    }
    return "paste a Google Photos album link (https://photos.app.goo.gl/... or "
           "https://photos.google.com/share/...)";
}

static esp_err_t h_gphotos_config_set(httpd_req_t *req)
{
    GUARD(req);

    // NOT refused from the access point, unlike /api/smb/config. The site this exists for has
    // Wi-Fi and nothing else, and a phone on the frame's own AP -- WPA2 and the token since ticket
    // 30 -- is one of the ways the link gets in.
    //
    // {"enabled": bool, "albums": [slot 0, slot 1, ...]}, both optional. Per slot, null or a missing
    // entry keeps the stored link, "" removes it, and a string sets it. The body is in PSRAM: four
    // 255-character links do not fit the 512 bytes this handler used to keep on its stack.
    char *body = heap_caps_malloc(GPHOTOS_CONFIG_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        return send_error(req, 500, "out of memory");
    }
    cJSON *j = read_json_body(req, body, GPHOTOS_CONFIG_BODY_MAX);
    heap_caps_free(body);
    if (!j) {
        return send_error(req, 400, "bad json");
    }
    const cJSON *albums = cJSON_GetObjectItem(j, "albums");
    const cJSON *enabled = cJSON_GetObjectItem(j, "enabled");
    if (albums && !cJSON_IsNull(albums) &&
        (!cJSON_IsArray(albums) || cJSON_GetArraySize(albums) > APP_SETTINGS_GPHOTOS_SLOTS)) {
        cJSON_Delete(j);
        return send_error(req, 400, "albums must be an array of at most 4 links");
    }
    // The scrape rules and where they are fetched from (the scrape-rules plan). A rule
    // URL must be https, and what it serves is used only if its signature verifies. A rule file is
    // only queued here -- a verify and a JSON parse do not belong on this stack -- so its fate is
    // read back from GET /api/gphotos/status's `rules`.
    const cJSON *rules_url = cJSON_GetObjectItem(j, "rules_url");
    const cJSON *rules = cJSON_GetObjectItem(j, "rules");
    if (rules_url && !cJSON_IsNull(rules_url) &&
        (!cJSON_IsString(rules_url) ||
         (rules_url->valuestring[0] != '\0' &&
          (strncmp(rules_url->valuestring, "https://", 8) != 0 ||
           strlen(rules_url->valuestring) >= APP_GPHOTOS_RULES_URL_SIZE)))) {
        cJSON_Delete(j);
        return send_error(req, 400,
                          "rules_url must be an https:// link under 256 characters, \"\" or null");
    }
    if (rules && !cJSON_IsNull(rules) &&
        (!cJSON_IsString(rules) || rules->valuestring[0] == '\0' ||
         strlen(rules->valuestring) > GPHOTOS_RULES_FILE_MAX)) {
        cJSON_Delete(j);
        return send_error(req, 400, "rules must be a signed rule file's text, at most 4096 bytes");
    }

    // Validated in full before anything is stored, as h_smb_config_set() does.
    char cur[APP_SETTINGS_GPHOTOS_ALBUM_SIZE];
    bool any = false;
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
        const cJSON *e = cJSON_IsArray(albums) ? cJSON_GetArrayItem(albums, (int)s) : NULL;
        if (cJSON_IsString(e)) {
            const char *problem = album_problem(e->valuestring);
            if (problem) {
                char msg[256];
                snprintf(msg, sizeof(msg), "album %u: %s", (unsigned)(s + 1), problem);
                cJSON_Delete(j);
                return send_error(req, 400, msg);
            }
            any = any || e->valuestring[0] != '\0';
        } else if (e && !cJSON_IsNull(e)) {
            cJSON_Delete(j);
            return send_error_code(req, 400, "gp.bad_link",
                                   "each album must be a link, \"\" or null");
        } else {
            app_settings_gphotos_album(s, cur, sizeof(cur));
            any = any || cur[0] != '\0';
        }
    }
    const bool on = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : app_settings_gphotos_switch();
    if (on && !any) {
        cJSON_Delete(j);
        return send_error_code(req, 400, "gp.link_required",
                               "an album link is needed to turn this on");
    }

    esp_err_t saved = ESP_OK;
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS && saved == ESP_OK; s++) {
        const cJSON *e = cJSON_IsArray(albums) ? cJSON_GetArrayItem(albums, (int)s) : NULL;
        if (!cJSON_IsString(e)) {
            continue;
        }
        app_settings_gphotos_album(s, cur, sizeof(cur));
        if (strcmp(cur, e->valuestring) != 0) {
            saved = app_settings_set_gphotos_album(s, e->valuestring);
        }
    }
    if (saved == ESP_OK && cJSON_IsString(rules_url)) {
        saved = app_gphotos_rules_set_url(rules_url->valuestring);
    }
    if (saved == ESP_OK && cJSON_IsString(rules)) {
        saved = app_gphotos_rules_submit(rules->valuestring, strlen(rules->valuestring));
    }
    cJSON_Delete(j);
    if (saved == ESP_OK && on != app_settings_gphotos_switch()) {
        saved = app_settings_set_gphotos_enabled(on);
    }
    if (saved != ESP_OK) {
        return send_error(req, 500, "could not save");
    }
    // A run now rather than at the next day, so the page can show whether the links work while
    // the person who typed them is still looking.
    if (on) {
        app_gphotos_sync_request();
    }
    return h_gphotos_config_get(req);
}

// The page's Sync now: every album re-read at the next tick, whatever the daily schedule says.
// Not refused from the access point, for h_gphotos_config_set()'s reason, and it does not stop
// httpd, so the answer is immediate.
static esp_err_t h_gphotos_sync(httpd_req_t *req)
{
    GUARD(req);

    if (!app_settings_gphotos_enabled()) {
        return send_error_code(req, 400, "gp.off", "Google Photos is off or has no album");
    }
    app_gphotos_sync_request();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "queued");
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// POST /api/auth/pair -- ticket 66, and the reason a PC can log in at all.
//
// **The defect this closes:** the API token was only ever handed out by the pairing QR, so a
// desktop browser -- no camera -- got the page, a 401 on its first call, and a banner telling it to
// scan a code it could not see. The panel now prints a code as well, and this is where it is spent.
//
// **It is a delivery channel for the existing token, not a second authentication scheme**, and that
// distinction is what answers ticket 29's rejection of a PIN ("needs rate limiting and a session
// table"). There is no session: this route hands over the same permanent cookie the QR does.
//
// Three things about it that are deliberate:
//
//   1. **HOST_GUARD, not GUARD.** The only handler in this file that may skip the token, because
//      handing one out is its purpose. The Host check stays: this reply carries a Set-Cookie, so a
//      DNS-rebinding attempt must not be able to ask.
//   2. **It answers on the station link as well as the access point** -- the mirror image of the SMB
//      routes' request_from_ap() refusal. A PC on the house LAN is the case this exists for, so
//      refusing the LAN here would close the door this opens.
//   3. **A malformed body is not an attempt.** app_auth_pairing_claim() only spends one of the five
//      on a string that is the shape of a code; otherwise a slip of the keyboard would close a
//      window the user has to walk back to the frame to reopen.
static esp_err_t h_auth_pair(httpd_req_t *req)
{
    HOST_GUARD(req);

    // Enough for {"code":"XXXX-XXXX"} many times over and nowhere near a request this route has
    // any business reading.
    char buf[128];
    if (req->content_len >= sizeof(buf)) {
        return send_error(req, 413, "body too large");
    }
    cJSON *root = read_json_body(req, buf, sizeof(buf));
    const cJSON *code = root ? cJSON_GetObjectItem(root, "code") : NULL;
    if (!cJSON_IsString(code)) {
        cJSON_Delete(root);
        return send_error(req, 400, "code required");
    }

    char token[APP_AUTH_TOKEN_HEX_SIZE];
    int left = 0;
    const app_auth_pair_result_t res =
        app_auth_pairing_claim(code->valuestring, token, sizeof(token), &left);
    cJSON_Delete(root);

    // One line per attempt, with who asked. **Never the code**, right or wrong: this output ends up
    // in capture files, and a wrong guess is still a near miss worth not publishing.
    switch (res) {
    case APP_AUTH_PAIR_OK:
        log_peer(req, "pair ok");
        break;
    case APP_AUTH_PAIR_WRONG:
        log_peer(req, "pair wrong");
        break;
    case APP_AUTH_PAIR_MALFORMED:
        log_peer(req, "pair malformed");
        break;
    case APP_AUTH_PAIR_NO_WINDOW:
    default:
        log_peer(req, "pair no window");
        break;
    }

    if (res == APP_AUTH_PAIR_NO_WINDOW) {
        // Says which of the three it is -- never opened, expired, or the attempts ran out -- as one
        // message, because the device cannot tell them apart either once the window is closed, and
        // because the recovery is the same for all three.
        return send_error_code(req, 403, "pair.no_window",
                               "no pairing code is active -- hold the button on the frame for 5 "
                               "seconds");
    }
    if (res == APP_AUTH_PAIR_MALFORMED) {
        return send_error_code(req, 400, "pair.malformed",
                               "that is not a pairing code: 8 characters, as shown on the panel");
    }
    if (res == APP_AUTH_PAIR_WRONG) {
        char msg[80];
        if (left > 0) {
            snprintf(msg, sizeof(msg), "wrong code -- %d attempt%s left", left,
                     left == 1 ? "" : "s");
        } else {
            snprintf(msg, sizeof(msg),
                     "wrong code -- hold the button on the frame for 5 seconds again");
        }
        // TWO codes rather than one with a flag: the plural in the English is a `%s` here and would
        // be a different rule in another language, and "no attempts left" is a different
        // instruction rather than the same sentence with a zero in it. `left` travels for the
        // first, which is the second of the three coded errors carrying a number.
        if (left > 0) {
            cJSON *d = cJSON_CreateObject();
            if (d) {
                cJSON_AddNumberToObject(d, "left", left);
            }
            return send_error_full(req, 401, "pair.wrong", msg, d);
        }
        return send_error_code(req, 401, "pair.wrong.exhausted", msg);
    }

    // The cookie is what the photo grid needs: thumbnails are plain <img src> tags and cannot carry
    // a header. The token is in the body as well, for the same reason index.html keeps a copy of a
    // ?t= in localStorage -- some captive-portal mini-browsers do not store cookies, and this client
    // has just proved it read the panel.
    char cookie[TOKEN_COOKIE_BUF];
    set_token_cookie(req, token, cookie, sizeof(cookie));

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "token", token);
    const esp_err_t err = send_json(req, r);
    cJSON_Delete(r);
    return err;
}

// The page itself is served to anyone, deliberately. It has to be: a browser that has
// never paired needs somewhere to be told how to pair, and FR-2.2's captive portal needs
// this route to answer a request addressed to somebody else entirely. Nothing secret is
// in it -- every photograph, setting and status it displays comes from a route that does
// check.
//
// It is also where a browser picks up its cookie. The pairing QR (ticket 30) points at
// http://<addr>/?t=<token>, and one GET of that turns the token in the URL into a cookie
// the browser will send on its own from then on -- including on the <img> tags the photo
// grid uses, which cannot carry a header.
static esp_err_t send_index(httpd_req_t *req)
{
    char query[APP_AUTH_TOKEN_HEX_SIZE + 8];
    char val[APP_AUTH_TOKEN_HEX_SIZE];
    char cookie[TOKEN_COOKIE_BUF];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "t", val, sizeof(val)) == ESP_OK && app_auth_check(val)) {
        set_token_cookie(req, val, cookie, sizeof(cookie));
        // Ticket 66: this device has now paired with something, so the connect card is not drawn
        // at the next boot. A no-op after the first time.
        app_auth_mark_paired();
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    mark_activity();
    return httpd_resp_send(req, (const char *)asset_index_html_gz, asset_index_html_gz_len);
}

// Ticket 30 step 2, and the fix for the captive-portal defect ticket 30 step 1 found on a phone.
//
// **The defect:** this route is `/*`, so a phone's portal probe -- GET http://connectivitycheck.
// gstatic.com/generate_204 -- was answered with 200 and the page, under that Host. The page then
// makes relative API calls, which carry the same Host, and host_ok() refuses every one with 403.
// The user gets the UI and index.html's own "Cannot reach server" toast: the page arrives and is
// inert. host_ok()'s comment says the /* exclusion is what saves the portal, and it is necessary
// and NOT sufficient -- it covers the probe and the page, not the page's own fetch.
//
// **The fix is to send the phone to the real origin instead of serving the page at the wrong one.**
// A 302 to http://192.168.4.1/?t=<token> arrives at a Host host_ok() accepts, the query mints the
// cookie exactly as the pairing QR does, and every relative call after that is a Host we own. So
// nothing has to be relaxed in the guard.
//
// **Three conditions, and each of them is load-bearing** -- docs/agents/web-api.md's warning is
// that redirecting the probe with the token "hands the API token to anything joining an open AP":
//
//   1. `!host_ok(req)` -- only a request addressed to somebody else. A normal GET / is untouched.
//   2. `request_certainly_from_ap(req)` -- never the station link, so a DNS-rebinding attempt from
//      the house LAN gets today's behaviour rather than a token. Certainly, not probably: see that
//      function.
//   3. `board_wifi_ap_secured()` -- the access point actually carries WPA2 right now, so whoever
//      is on it knows a password that only the pairing screen gives out. This asks the RADIO and
//      not the build flag, which is why FRAME_AP_OPEN cannot half-apply and leave the token
//      exposed.
//
// Anything failing those falls through to the old behaviour, which is the page under a foreign
// Host -- a broken portal, and the state this shipped in.
static esp_err_t h_static_serve(httpd_req_t *req)
{
    if (!host_ok(req) && board_wifi_ap_secured() && request_certainly_from_ap(req)) {
        board_wifi_status_t st;
        board_wifi_status(&st);
        char token[APP_AUTH_TOKEN_HEX_SIZE];
        app_auth_token_hex(token, sizeof(token));

        char loc[96];
        snprintf(loc, sizeof(loc), "http://%s/?t=%s", st.ap_ip[0] ? st.ap_ip : "192.168.4.1",
                 token);
        // The Location is NOT logged: it carries the token whole and this console output ends up in
        // capture files. The Host that asked is, because that is the thing a portal run has to
        // check -- iOS probes captive.apple.com and Android connectivitycheck.gstatic.com, and
        // which one appeared says which phone was in the room.
        char host[64];
        if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
            host[0] = '\0';
        }
        printf("# portal redirect host=\"%s\" uri=%s -> http://%s/?t=<token>\n", host, req->uri,
               st.ap_ip[0] ? st.ap_ip : "192.168.4.1");
        fflush(stdout);

        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        return httpd_resp_send(req, NULL, 0);
    }
    return send_index(req);
}

// Everything unknown becomes the page. That is what makes a phone's captive-portal
// probe open the UI instead of showing "no internet" (FR-2.2).
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// The wildcard matcher, with one line added: "a request began, now".
//
// This is the hook the guard comment above says cannot refuse a request -- and it cannot, which is
// why it is used for a timestamp and nothing else. It is the only thing esp_http_server calls once
// per request before routing, so it sees the 401s, the 403s and the misses that fall through to
// not_found() as well as the routes that answer. Ticket 47 needs it because the heap sampler has
// to be able to say whether an HTTP request was in flight at the moment of a low, and
// send_file()'s whole-file read into PSRAM is the largest single thing this server ever holds.
//
// Called on the httpd task with no locks held; app_heapwatch_note_request() writes one word.
//
// `s_registering` is why this is not a one-liner: see the note at the registration loop.
static volatile bool s_registering;

static bool match_and_stamp(const char *uri_template, const char *uri_to_match, size_t match_upto)
{
    if (!s_registering) {
        app_heapwatch_note_request();
    }
    return httpd_uri_match_wildcard(uri_template, uri_to_match, match_upto);
}

// ---------------------------------------------------------------------- start

esp_err_t app_server_start(void)
{
    if (s_httpd) {
        return ESP_OK;
    }
    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
        s_scan_mutex = xSemaphoreCreateMutex();
        if (!s_state_mutex || !s_scan_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // 8, not the reference's 13. Each open socket costs internal RAM -- the scarce
    // resource here (ticket 21) -- and the page's own concurrency is a handful of
    // parallel thumbnail GETs, not thirteen. FR-3.4's requirement is really about
    // CONFIG_LWIP_MAX_SOCKETS being large enough for whatever this number is: httpd
    // refuses to start if it exceeds LWIP_MAX_SOCKETS - 3.
    cfg.max_open_sockets = 8;
    // 26 for the 25 routes below -- counted, not carried over: this comment has been stale
    // twice, once saying "22 for 21 routes" when there were 22, and once saying "23" after
    // /thumb/* made it 24, and /api/storage/rescan made it 25 on 2026-09-08. A slot is one httpd_uri_t pointer, so unlike max_open_sockets and
    // stack_size above and below it this one is not an internal-RAM decision. Past the limit
    // httpd_register_uri_handler() fails; the loop below returns that error, so the symptom
    // is a frame that boots with a panel, a slideshow and no web UI at all.
    // 32, and the route table below fills it exactly -- raised from 26 for ticket 60's config
    // routes, to 29 for its Sync now, to 30 on 2026-09-18 for /api/system/info, to 31 the same
    // day for ticket 66's /api/auth/pair, and to 32 on 2026-09-20 for ticket 68's
    // /api/panel/maintenance. A route past this number fails registration with one log line and
    // nothing else. COUNT THE TABLE rather than trusting this sentence; that is what the paragraph
    // above is about, and it is why ticket 68's status shares an existing route instead of adding a
    // second one. 33 on 2026-09-22 for ticket 09's /api/storage/usb, 34 the same day for ticket
    // 61's /api/system/ota.
    cfg.max_uri_handlers = 34;
    cfg.max_req_hdr_len = 1024;  // a browser's request headers, not 2 KB of them
    // 10 KB, not the reference's 20. The 20 KB was inherited from a firmware that
    // renders inside its handlers; here the deepest path is the multipart parser, which
    // is a 2 KB window plus cJSON. And it is not free: on 2026-09-03 httpd_start()
    // returned ESP_ERR_HTTPD_TASK -- no web UI at all -- because 20 KB of internal RAM
    // was not there to be had once the LED and button tasks existed (ticket 21).
    cfg.stack_size = 10240;
    cfg.uri_match_fn = match_and_stamp;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    // Verbatim from app_server.cpp:1361-1383, order included: "/" is last so the
    // wildcard cannot shadow an API route.
    static const httpd_uri_t routes[] = {
        {"/api/modes", HTTP_GET, h_get_modes, NULL},
        {"/api/wifi/scan", HTTP_GET, h_wifi_scan, NULL},
        {"/api/wifi/config", HTTP_POST, h_wifi_config, NULL},
        {"/api/wifi/status", HTTP_GET, h_wifi_status, NULL},
        {"/api/device/ready", HTTP_GET, h_device_ready, NULL},
        {"/api/wifi/disconnect", HTTP_POST, h_wifi_disconnect, NULL},
        {"/api/photos/list", HTTP_GET, h_photos_list, NULL},
        {"/api/photos/upload", HTTP_POST, h_photos_upload, NULL},
        {"/api/photos/delete", HTTP_DELETE, h_photos_delete, NULL},
        {"/api/storage", HTTP_GET, h_storage, NULL},
        {"/api/storage/rescan", HTTP_POST, h_storage_rescan, NULL},
        {"/api/battery", HTTP_GET, h_battery, NULL},
        {"/api/mode/mode_1/config", HTTP_GET, h_mode_cfg_get, NULL},
        {"/api/mode/mode_1/config", HTTP_POST, h_mode_cfg_set, NULL},
        {"/api/mode/switch", HTTP_POST, h_mode_switch, NULL},
        {"/data/*", HTTP_GET, h_data_serve, NULL},
        {"/thumb/*", HTTP_GET, h_thumb_serve, NULL},
        {"/api/photos/display", HTTP_POST, h_photos_display, NULL},
        {"/api/system/info", HTTP_GET, h_system_info, NULL},
        {"/api/system/reset", HTTP_POST, h_system_reset, NULL},
        {"/api/smb/config", HTTP_GET, h_smb_config_get, NULL},
        {"/api/smb/config", HTTP_POST, h_smb_config_set, NULL},
        {"/api/smb/sync", HTTP_POST, h_smb_sync, NULL},
        {"/api/smb/status", HTTP_GET, h_smb_status, NULL},
        {"/api/smb/test", HTTP_POST, h_smb_test, NULL},
        // Ticket 60.
        {"/api/gphotos/status", HTTP_GET, h_gphotos_status, NULL},
        {"/api/gphotos/config", HTTP_GET, h_gphotos_config_get, NULL},
        {"/api/gphotos/config", HTTP_POST, h_gphotos_config_set, NULL},
        {"/api/gphotos/sync", HTTP_POST, h_gphotos_sync, NULL},
        // Ticket 66. POST only -- a code in a query string is a code in every log between here and
        // the browser.
        {"/api/auth/pair", HTTP_POST, h_auth_pair, NULL},
        // Ticket 68. With the wildcard below it that is 32 entries against max_uri_handlers = 32, so
        // the table is full again. The course's STATUS is not a route: it rides
        // GET /api/system/info, precisely so that this needed one slot rather than two.
        {"/api/panel/maintenance", HTTP_POST, h_panel_maint, NULL},
        // Ticket 09. 33 entries with the wildcard, against max_uri_handlers = 33.
        {"/api/storage/usb", HTTP_POST, h_storage_usb, NULL},
        // Ticket 61. 34 entries with the wildcard, against max_uri_handlers = 34.
        {"/api/system/ota", HTTP_POST, h_system_ota, NULL},
        {"/*", HTTP_GET, h_static_serve, NULL},
    };

    // **httpd_register_uri_handler() calls uri_match_fn to check for a duplicate**, so registration
    // walks the matcher 25 times with no client anywhere -- and match_and_stamp() counted every one
    // of them as a request beginning. Measured 2026-09-11: a heap low 6 ms after "the last HTTP
    // request" at 36,617 ms, on a boot where nothing had ever connected, and again on every window
    // restart. That is precisely the moment ticket 47's instrument is trying to attribute, so the
    // pollution landed where it did the most harm. Suppressed rather than filtered inside the
    // matcher, which cannot tell the two callers apart.
    s_registering = true;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(s_httpd, &routes[i]);
        if (err != ESP_OK) {
            s_registering = false;
            ESP_LOGE(TAG, "register %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    s_registering = false;
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, not_found);

    ESP_LOGI(TAG, "http server on :80, %u routes, index.html %u bytes gzipped",
             (unsigned)(sizeof(routes) / sizeof(routes[0])), (unsigned)asset_index_html_gz_len);
    return ESP_OK;
}

esp_err_t app_server_stop(void)
{
    if (!s_httpd) {
        return ESP_OK;
    }
    const esp_err_t err = httpd_stop(s_httpd);
    s_httpd = NULL;
    return err;
}
