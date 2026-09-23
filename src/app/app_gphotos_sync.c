#include "app_gphotos_sync.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_clock.h"
#include "app_gphotos_rules.h"
#include "app_heapwatch.h"
#include "app_settings.h"
#include "app_slideshow.h"
#include "app_smb_sync.h"
#include "board_storage.h"
#include "board_wifi.h"
#include "epd_image.h"
#include "gphotos_parse.h"
#include "smb_manifest.h"

static const char *TAG = "gphotos";

// Inside BOARD_STORAGE_MOUNT. Neither is one of photo_list's extensions, so neither appears in the
// slideshow -- the rule .smbidx and .smbdir rely on.
#define GPHOTOS_LIST_FILE ".gpdir"
#define GPHOTOS_INDEX_FILE ".gpidx"

// The list keeps KEYS only, ~121 bytes each in PSRAM. Well under SMB_CATALOG_MAX (4096), which
// bounds the slideshow's permutation, with room for an SMB catalogue beside it.
#define GPHOTOS_MAX_PHOTOS 1000
#define GPHOTOS_LIST_FILE_MAX ((long)GPHOTOS_MAX_PHOTOS * (GPHOTOS_KEY_MAX + GPHOTOS_ID_SIZE) + 64)

// Photographs kept on /data. SMB_SYNC_CACHE_FILES, for its reasons.
#define GPHOTOS_CACHE_FILES 100
#define GPHOTOS_ARENA (64 * 1024)
#define GPHOTOS_INDEX_FILE_MAX (64 * 1024)

// The CDN's 400x600 renditions measured 55,682-158,590 B, and 600x600 fits 59-178 KB. 1 MB is room.
#define GPHOTOS_PHOTO_MAX (1024 * 1024)
#define GPHOTOS_READ_CHUNK 4096

// 8 KB: a fetch task used 3,608-3,716 B, and store_photo() runs on the SMB window's task at the
// same size.
#define GPHOTOS_STACK 8192
#define GPHOTOS_PRIO 4

// After the SMB mirror's first run at 30 s, so the two first runs do not start together.
#define GPHOTOS_FIRST_DELAY_MS 90000
// Once a day, counted from the end of the last run (the owner's change of 2026-09-13). Manual
// reads come from the page's Sync now.
#ifndef GPHOTOS_PERIOD_MS
#define GPHOTOS_PERIOD_MS (24LL * 60 * 60 * 1000)
#endif

#define WORK_LOAD (1u << 0)
#define WORK_FETCH (1u << 1)
#define WORK_LIST (1u << 2)

static SemaphoreHandle_t s_mutex;
static volatile bool s_running;
static volatile uint8_t s_work;
static bool s_list_loaded;
static bool s_list_request;
static int64_t s_last_list_end_us = INT64_MIN;

// One list per album slot, under s_mutex. Replaced by swapping the pointer, never edited in place.
// `ids` names the pattern each key was found by, which is how its photograph's URL is built.
typedef struct {
    char (*keys)[GPHOTOS_KEY_MAX];
    char (*ids)[GPHOTOS_ID_SIZE];
    size_t n;
} list_t;
static list_t s_lists[APP_SETTINGS_GPHOTOS_SLOTS];

// The slideshow's ask, under s_mutex. Static rather than on a stack: the ask arrives on the main
// task, which has ~650 bytes to spare.
static bool s_want;
static char s_want_key[GPHOTOS_KEY_MAX];
static char s_want_id[GPHOTOS_ID_SIZE];
// The key being fetched. Only the task touches it, and there is one task at a time.
static char s_fetch_key[GPHOTOS_KEY_MAX];
static char s_fetch_id[GPHOTOS_ID_SIZE];

static app_gphotos_status_t s_status = {
    .last_run_ms = -1,
    .albums = {{.last_ok_ms = -1}, {.last_ok_ms = -1}, {.last_ok_ms = -1}, {.last_ok_ms = -1}},
};
// Static for the same reason as s_want_key: read from the application loop.
static board_wifi_status_t s_wifi;

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

const char *app_gphotos_result_str(app_gphotos_result_t r)
{
    switch (r) {
    case APP_GPHOTOS_NEVER:
        return "never";
    case APP_GPHOTOS_OK:
        return "ok";
    case APP_GPHOTOS_ERR_CONNECT:
        return "cannot reach the album";
    case APP_GPHOTOS_ERR_HTTP:
        return "the album refused the request";
    case APP_GPHOTOS_ERR_TRUNCATED:
        return "the album did not arrive whole";
    case APP_GPHOTOS_ERR_EMPTY:
        return "no photographs found in the album";
    case APP_GPHOTOS_ERR_NO_MEM:
        return "out of memory";
    }
    return "unknown";
}

// ------------------------------------------------------------------------------ HTTP

// `ua` comes from the rules: a browser User-Agent, since a non-browser one is served a ~120 KB
// larger document for the same photographs (measured 2026-09-12).
static esp_http_client_handle_t client_for(const char *url, const char *ua)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .timeout_ms = 30000,
        .user_agent = ua,
        .keep_alive_enable = false,
    };
    return esp_http_client_init(&cfg);
}

// Opens `c` and follows redirects. A streaming open does not follow them by itself, and the short
// link a phone shares (photos.app.goo.gl) answers 302 -- measured as a real Location header.
static esp_err_t open_follow(esp_http_client_handle_t c, int *status)
{
    for (int hop = 0; hop < 5; hop++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            return err;
        }
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (*status == 301 || *status == 302 || *status == 303 || *status == 307 ||
            *status == 308) {
            esp_http_client_flush_response(c, NULL);
            if (esp_http_client_set_redirection(c) != ESP_OK) {
                return ESP_FAIL;
            }
            esp_http_client_close(c);
            continue;
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ----------------------------------------------------------------------- the album

typedef struct {
    char (*keys)[GPHOTOS_KEY_MAX];
    char (*ids)[GPHOTOS_ID_SIZE];
    size_t n;
    bool over_cap;
    const gphotos_rules_t *rules;  // what the read looks for; not owned
} album_t;

static bool album_alloc(album_t *a)
{
    a->keys = heap_caps_calloc(GPHOTOS_MAX_PHOTOS, GPHOTOS_KEY_MAX, MALLOC_CAP_SPIRAM);
    a->ids = heap_caps_calloc(GPHOTOS_MAX_PHOTOS, GPHOTOS_ID_SIZE, MALLOC_CAP_SPIRAM);
    return a->keys && a->ids;
}

static void album_free(album_t *a)
{
    heap_caps_free(a->keys);
    heap_caps_free(a->ids);
    a->keys = NULL;
    a->ids = NULL;
}

static bool album_has(const album_t *a, const char *key)
{
    for (size_t i = 0; i < a->n; i++) {
        if (strcmp(a->keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static void on_item(const char *key, size_t key_len, uint32_t w, uint32_t h, size_t pattern,
                    void *ctx)
{
    (void)w;
    (void)h;
    album_t *a = ctx;
    if (album_has(a, key)) {
        return;
    }
    if (a->n >= GPHOTOS_MAX_PHOTOS) {
        a->over_cap = true;
        return;
    }
    memcpy(a->keys[a->n], key, key_len + 1);
    memcpy(a->ids[a->n], a->rules->patterns[pattern].id, GPHOTOS_ID_SIZE);
    a->n++;
}

// WHETHER THE DOCUMENT ARRIVED WHOLE is asked of esp_http_client itself
// (esp_http_client_is_complete_data_received), not inferred from a read returning 0: a read that
// stops short can end with a clean 0, and the photographs sit in the document's last ~8 %.
static app_gphotos_result_t read_album(const char *url, album_t *a, int *status, uint32_t *bytes)
{
    *status = 0;
    *bytes = 0;
    gphotos_parse_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM);
    char *chunk = heap_caps_malloc(GPHOTOS_READ_CHUNK, MALLOC_CAP_SPIRAM);
    if (!p || !chunk) {
        heap_caps_free(p);
        heap_caps_free(chunk);
        return APP_GPHOTOS_ERR_NO_MEM;
    }
    gphotos_parse_init(p, a->rules, on_item, a);

    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, true);
    app_gphotos_result_t result = APP_GPHOTOS_ERR_CONNECT;
    esp_http_client_handle_t c = client_for(url, a->rules->ua);
    if (!c) {
        result = APP_GPHOTOS_ERR_NO_MEM;
    } else if (open_follow(c, status) != ESP_OK) {
        result = APP_GPHOTOS_ERR_CONNECT;
    } else if (*status != 200) {
        result = APP_GPHOTOS_ERR_HTTP;
    } else {
        int r;
        while ((r = esp_http_client_read(c, chunk, GPHOTOS_READ_CHUNK)) > 0) {
            gphotos_parse_feed(p, chunk, (size_t)r);
            *bytes += (uint32_t)r;
        }
        gphotos_parse_finish(p);
        if (r < 0 || !esp_http_client_is_complete_data_received(c)) {
            result = APP_GPHOTOS_ERR_TRUNCATED;
        } else {
            result = a->n > 0 ? APP_GPHOTOS_OK : APP_GPHOTOS_ERR_EMPTY;
        }
    }
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, false);
    heap_caps_free(p);
    heap_caps_free(chunk);
    return result;
}

static bool is_image(const uint8_t *buf, size_t len)
{
    switch (epd_image_sniff(buf, len)) {
    case EPD_IMAGE_JPEG:
    case EPD_IMAGE_PNG:
        return true;
    default:
        return false;
    }
}

// One photograph -- or a rule file -- into `buf`. A body that fills `buf` is treated as cut off, and
// one the client does not report complete as short -- a partial JPEG stored is a photograph the
// panel cannot draw.
static bool fetch_photo(const char *url, const char *ua, uint8_t *buf, size_t max, size_t *len,
                        int *status)
{
    *len = 0;
    *status = 0;
    bool ok = false;
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, true);
    esp_http_client_handle_t c = client_for(url, ua);
    if (c && open_follow(c, status) == ESP_OK && *status == 200) {
        const int64_t cl = esp_http_client_get_content_length(c);
        if (cl <= (int64_t)max) {
            int r;
            while ((r = esp_http_client_read(c, (char *)buf + *len, (int)(max - *len))) > 0) {
                *len += (size_t)r;
                if (*len >= max) {
                    break;
                }
            }
            ok = r >= 0 && *len > 0 && *len < max &&
                 esp_http_client_is_complete_data_received(c);
        }
    }
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, false);
    return ok;
}

// ------------------------------------------------------------------------ files

static void mount_path(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", BOARD_STORAGE_MOUNT, name);
}

// The whole file into PSRAM, or NULL. `*len` is its size.
static char *read_file(const char *name, long max, long *len)
{
    char path[64];
    mount_path(name, path, sizeof(path));
    *len = 0;
    char *buf = NULL;
    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        *len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (*len > 0 && *len <= max) {
            buf = heap_caps_malloc((size_t)*len, MALLOC_CAP_SPIRAM);
            if (buf && fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
                heap_caps_free(buf);
                buf = NULL;
            }
        }
        fclose(f);
    }
    board_storage_unlock();
    return buf;
}

// TICKET 78: through `<name>.part` and a rename, which the SMB mirror's photograph and catalogue
// writers already did and this one did not. Both files it writes are parsed back and believed --
// `.gpidx` is the Google cache's OWNERSHIP RECORD, whose loss makes every file it fetched
// invisible to eviction, and `.gpdir<slot>` is the album list. A truncation of either fails the
// parse loudly *unless* it lands exactly on a record boundary, and then it parses as a shorter
// list with nothing to report it. board_storage_write_atomic() has the account and the sweep.
static bool write_file(const char *name, const char *buf, size_t n)
{
    char path[64];
    mount_path(name, path, sizeof(path));
    return board_storage_write_atomic(path, buf, n);
}

static void index_load(smb_manifest_t *m)
{
    long len = 0;
    char *buf = read_file(GPHOTOS_INDEX_FILE, GPHOTOS_INDEX_FILE_MAX, &len);
    smb_manifest_reset(m);
    // A damaged index is discarded rather than repaired, as .smbidx is: the mirror then owns
    // nothing, and the files it wrote before are left alone rather than deleted on the strength of
    // an index that could not be read.
    if (buf && !smb_manifest_parse(m, buf, (size_t)len)) {
        ESP_LOGW(TAG, "%s unparseable; starting with nothing owned", GPHOTOS_INDEX_FILE);
        smb_manifest_reset(m);
    }
    heap_caps_free(buf);
}

static void index_store(const smb_manifest_t *m)
{
    const size_t need = m->arena_used + smb_manifest_count(m) * 48 + 32;
    char *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte index", (unsigned)need);
        return;
    }
    const size_t n = smb_manifest_serialise(m, buf, need);
    if (n > 0) {
        write_file(GPHOTOS_INDEX_FILE, buf, n);
    }
    heap_caps_free(buf);
}

// ------------------------------------------------------------------------- the list

// Slot 0 keeps the single-album build's name, so its list survives the reflash that added slots.
static void list_name(size_t slot, char *out, size_t size)
{
    if (slot == 0) {
        snprintf(out, size, "%s", GPHOTOS_LIST_FILE);
    } else {
        snprintf(out, size, "%s%u", GPHOTOS_LIST_FILE, (unsigned)slot);
    }
}

// In any slot's list. Called with s_mutex NOT held.
static bool list_has(const char *key)
{
    bool found = false;
    lock();
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS && !found; s++) {
        for (size_t i = 0; s_lists[s].keys && i < s_lists[s].n && !found; i++) {
            found = strcmp(s_lists[s].keys[i], key) == 0;
        }
    }
    unlock();
    return found;
}

// Takes ownership of a->keys. The 1000-entry scratch is copied into a block of exactly the size the
// album needs, so four slots idle at their photographs' keys rather than 256 KB each; if that block
// cannot be had, the scratch itself is kept.
static void list_install(size_t slot, album_t *a)
{
    const size_t n = a->n ? a->n : 1;
    char (*keys)[GPHOTOS_KEY_MAX] = heap_caps_malloc(n * GPHOTOS_KEY_MAX, MALLOC_CAP_SPIRAM);
    char (*ids)[GPHOTOS_ID_SIZE] = heap_caps_malloc(n * GPHOTOS_ID_SIZE, MALLOC_CAP_SPIRAM);
    if (keys) {
        memcpy(keys, a->keys, a->n * GPHOTOS_KEY_MAX);
        heap_caps_free(a->keys);
    } else {
        keys = a->keys;
    }
    if (ids) {
        memcpy(ids, a->ids, a->n * GPHOTOS_ID_SIZE);
        heap_caps_free(a->ids);
    } else {
        ids = a->ids;
    }
    lock();
    char (*old)[GPHOTOS_KEY_MAX] = s_lists[slot].keys;
    char (*old_ids)[GPHOTOS_ID_SIZE] = s_lists[slot].ids;
    s_lists[slot].keys = keys;
    s_lists[slot].ids = ids;
    s_lists[slot].n = a->n;
    s_status.albums[slot].items = (uint16_t)a->n;
    s_status.albums[slot].over_cap = a->over_cap;
    unlock();
    heap_caps_free(old);
    heap_caps_free(old_ids);
    a->keys = NULL;
    a->ids = NULL;
}

// A slot whose link was removed: its list goes, from PSRAM and from /data.
static void list_drop(size_t slot)
{
    lock();
    char (*old)[GPHOTOS_KEY_MAX] = s_lists[slot].keys;
    char (*old_ids)[GPHOTOS_ID_SIZE] = s_lists[slot].ids;
    s_lists[slot].keys = NULL;
    s_lists[slot].ids = NULL;
    s_lists[slot].n = 0;
    s_status.albums[slot].items = 0;
    s_status.albums[slot].over_cap = false;
    unlock();
    heap_caps_free(old);
    heap_caps_free(old_ids);

    char name[16];
    list_name(slot, name, sizeof(name));
    char path[64];
    mount_path(name, path, sizeof(path));
    board_storage_lock();
    board_storage_prepare_access();
    remove(path);
    board_storage_unlock();
}

// "#gpdir 2" then "<pattern id> <key>" per line. "#gpdir 1" -- one key per line, from before the
// rules were data -- is still read, under the built-in pattern's id. Any malformation discards the
// whole file, as .smbdir's parse does: the recovery is the next album read, which is cheap.
static void list_load(size_t slot)
{
    char name[16];
    list_name(slot, name, sizeof(name));
    long len = 0;
    char *buf = read_file(name, GPHOTOS_LIST_FILE_MAX, &len);
    if (!buf) {
        return;
    }
    album_t a = {0};
    static const char HEADER1[] = "#gpdir 1\n";
    static const char HEADER2[] = "#gpdir 2\n";
    const size_t header = sizeof(HEADER1) - 1;
    unsigned version = 0;
    if (len >= (long)header) {
        version = memcmp(buf, HEADER1, header) == 0   ? 1
                  : memcmp(buf, HEADER2, header) == 0 ? 2
                                                      : 0;
    }
    bool ok = album_alloc(&a) && version != 0;
    const char *p = buf + header;
    const char *end = buf + len;
    while (ok && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t k = (size_t)((nl ? nl : end) - p);
        if (k > 0 && p[k - 1] == '\r') {
            k--;
        }
        if (k > 0) {
            size_t off = 0;
            size_t key_len = 0;
            if (a.n >= GPHOTOS_MAX_PHOTOS ||
                !gphotos_dir_line(p, k, version, a.ids[a.n], &off, &key_len)) {
                ok = false;
                break;
            }
            memcpy(a.keys[a.n], p + off, key_len);
            a.keys[a.n][key_len] = '\0';
            a.n++;
        }
        p = nl ? nl + 1 : end;
    }
    heap_caps_free(buf);
    if (!ok) {
        ESP_LOGW(TAG, "%s unparseable; the list waits for the next album read", name);
        album_free(&a);
        return;
    }
    const size_t n = a.n;
    list_install(slot, &a);
    ESP_LOGI(TAG, "list: %u photographs from %s", (unsigned)n, name);
}

static void list_store(size_t slot, const album_t *a)
{
    const size_t need = 16 + a->n * (GPHOTOS_KEY_MAX + GPHOTOS_ID_SIZE);
    char *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM to store the list");
        return;
    }
    size_t at = (size_t)snprintf(buf, need, "#gpdir 2\n");
    for (size_t i = 0; i < a->n; i++) {
        at += (size_t)snprintf(buf + at, need - at, "%s %s\n", a->ids[i], a->keys[i]);
    }
    char name[16];
    list_name(slot, name, sizeof(name));
    write_file(name, buf, at);
    heap_caps_free(buf);
}

// BENCH FLAG, never in a shipping build: -DGPHOTOS_BENCH_ONLY_SLOT=<0..3> makes the slideshow select
// from that one slot (and app_catalog.c drops the SMB part), so a small album's photographs are
// fetched and drawn within minutes rather than after hundreds of advances. Lists, reads and
// deletion still cover every slot.
#ifdef GPHOTOS_BENCH_ONLY_SLOT
#define SLOT_SELECTABLE(s) ((s) == (size_t)GPHOTOS_BENCH_ONLY_SLOT)
#else
#define SLOT_SELECTABLE(s) true
#endif

// Total selectable photographs over the slots, and entry `i` of that concatenation. Both under
// s_mutex.
static size_t total_locked(void)
{
    size_t n = 0;
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
        if (!SLOT_SELECTABLE(s)) {
            continue;
        }
        n += s_lists[s].keys ? s_lists[s].n : 0;
    }
    return n;
}

static const char *key_at_locked(size_t i, const char **id)
{
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
        if (!SLOT_SELECTABLE(s)) {
            continue;
        }
        const size_t n = s_lists[s].keys ? s_lists[s].n : 0;
        if (i < n) {
            if (id) {
                *id = s_lists[s].ids[i];
            }
            return s_lists[s].keys[i];
        }
        i -= n;
    }
    return NULL;
}

// ------------------------------------------------------------------------- the work

static void publish(const app_gphotos_status_t *st)
{
    // Fields others write under the lock while the task runs are kept, not overwritten from the
    // task's copy.
    lock();
    const app_gphotos_status_t cur = s_status;
    s_status = *st;
    s_status.enabled = cur.enabled;
    s_status.want_pending = cur.want_pending;
    for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
        s_status.albums[s].items = cur.albums[s].items;
        s_status.albums[s].over_cap = cur.albums[s].over_cap;
    }
    unlock();
}

// Oldest first, never what is on the panel, what the settle window is about to draw, or the
// photograph that just arrived.
static uint16_t evict_over_cap(smb_manifest_t *m, const char *just_fetched)
{
    if (smb_manifest_count(m) <= GPHOTOS_CACHE_FILES) {
        return 0;
    }
    app_slideshow_state_t ss;
    app_slideshow_get_state(&ss);
    uint16_t dropped = 0;
    size_t i = 0;
    while (smb_manifest_count(m) > GPHOTOS_CACHE_FILES && i < smb_manifest_count(m)) {
        const char *local = smb_manifest_local(m, i);
        const bool keep = strcmp(local, just_fetched) == 0 ||
                          (ss.current_name[0] && strcmp(local, ss.current_name) == 0) ||
                          (ss.pending_name[0] && strcmp(local, ss.pending_name) == 0);
        if (keep) {
            i++;
            continue;
        }
        app_smb_sync_delete_photo(local);
        smb_manifest_remove_at(m, i);
        dropped++;
    }
    return dropped;
}

static void do_fetch(app_gphotos_status_t *st, smb_manifest_t *m, uint8_t *photo, char *url,
                     size_t url_size, const char *ua)
{
    lock();
    const bool have = s_want;
    if (have) {
        memcpy(s_fetch_key, s_want_key, sizeof(s_fetch_key));
        memcpy(s_fetch_id, s_want_id, sizeof(s_fetch_id));
        s_want = false;
        s_status.want_pending = 0;
    }
    unlock();
    if (!have) {
        return;
    }

    char local[SMB_MANIFEST_NAME_SIZE];
    if (!gphotos_local_name(s_fetch_key, strlen(s_fetch_key), local, sizeof(local))) {
        st->failed++;
        return;
    }
    // A key found by a pattern the rules in force no longer have: its list predates the rules, and
    // the album's next whole read replaces it.
    if (!app_gphotos_rules_photo_url(s_fetch_id, s_fetch_key, SMB_RESIZE_FIT_EDGE, url, url_size)) {
        ESP_LOGW(TAG, "%s: no pattern \"%s\" in the rules in force", local, s_fetch_id);
        st->failed++;
        return;
    }
    size_t len = 0;
    int status = 0;
    const int64_t t0 = esp_timer_get_time();
    bool ok = fetch_photo(url, ua, photo, GPHOTOS_PHOTO_MAX, &len, &status);
    if (ok && !is_image(photo, len)) {
        ESP_LOGW(TAG, "%s: %u B that are not a JPEG or PNG", local, (unsigned)len);
        ok = false;
    }
    if (ok) {
        ok = app_smb_sync_store_photo(local, photo, len) == ESP_OK;
    }
    if (!ok) {
        ESP_LOGW(TAG, "fetch %s failed: http %d, %u B", local, status, (unsigned)len);
        st->failed++;
        return;
    }

    index_load(m);
    // Replace, not append -- app_smb_sync.c's reason: a photograph evicted and wanted again would
    // otherwise get a second record, and evicting one would unlink the file the other still owns.
    // It also keeps record order equal to fetch order, which is what eviction reads.
    const int had = smb_manifest_find(m, s_fetch_key);
    if (had >= 0) {
        smb_manifest_remove_at(m, (size_t)had);
    }
    if (!smb_manifest_add(m, local, s_fetch_key, len, 0)) {
        ESP_LOGE(TAG, "%s written but the index refused it", local);
    }
    const uint16_t evicted = evict_over_cap(m, local);
    index_store(m);
    st->fetched++;
    st->evicted = (uint16_t)(st->evicted + evicted);
    st->owned = (uint16_t)smb_manifest_count(m);
    app_slideshow_invalidate();
    ESP_LOGI(TAG, "fetched %s for the slideshow (%u B, %lld ms), %u kept, %u evicted", local,
             (unsigned)len, (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)st->owned, (unsigned)evicted);
}

// An uploaded rule file, then the one at the rule URL: either may put a newer set on trial for the
// album reads that follow. The URL is a capability and is not logged.
static void check_rules(char *url, size_t url_size, const char *ua)
{
    app_gphotos_rules_consider_upload();
    if (!app_gphotos_rules_get_url(url, url_size, NULL)) {
        return;
    }
    char *buf = heap_caps_malloc(GPHOTOS_RULES_FILE_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }
    size_t len = 0;
    int status = 0;
    if (fetch_photo(url, ua, (uint8_t *)buf, GPHOTOS_RULES_FILE_MAX + 1, &len, &status)) {
        app_gphotos_rules_consider(buf, len, APP_GPRULES_SRC_URL, status);
    } else {
        app_gphotos_rules_fetch_failed(status);
    }
    heap_caps_free(buf);
}

// Every slot in turn, one TLS session at a time.
static void do_list(app_gphotos_status_t *st, smb_manifest_t *m, char *url, size_t url_size,
                    const char *ua)
{
    char *good = heap_caps_malloc(APP_SETTINGS_GPHOTOS_ALBUM_SIZE, MALLOC_CAP_SPIRAM);
    char *cur = heap_caps_malloc(APP_SETTINGS_GPHOTOS_ALBUM_SIZE, MALLOC_CAP_SPIRAM);
    bool any_listed = false;

    check_rules(url, url_size, ua);
    gphotos_rules_t *rules = heap_caps_malloc(sizeof(*rules), MALLOC_CAP_SPIRAM);
    const bool trial = rules && app_gphotos_rules_for_read(rules);
    // For the verdict on a set on trial: did any album arrive whole, and did any hold a photograph.
    bool any_whole = false;
    bool any_ok = false;

    for (size_t slot = 0; slot < APP_SETTINGS_GPHOTOS_SLOTS; slot++) {
        app_gphotos_album_status_t *as = &st->albums[slot];
        app_settings_gphotos_album(slot, url, url_size);
        if (url[0] == '\0') {
            lock();
            const bool had = s_lists[slot].keys != NULL;
            unlock();
            if (had) {
                list_drop(slot);
                ESP_LOGI(TAG, "album %u: link removed, its list dropped", (unsigned)slot);
            }
            *as = (app_gphotos_album_status_t){.last_ok_ms = -1};
            continue;
        }

        album_t a = {.rules = rules};
        app_gphotos_result_t result = APP_GPHOTOS_ERR_NO_MEM;
        as->last_http_status = 0;
        as->album_bytes = 0;
        if (rules && album_alloc(&a)) {
            result = read_album(url, &a, &as->last_http_status, &as->album_bytes);
        }
        ESP_LOGI(TAG, "album %u: %s, http %d, %u B, %u photographs%s%s", (unsigned)slot,
                 app_gphotos_result_str(result), as->last_http_status,
                 (unsigned)as->album_bytes, (unsigned)a.n,
                 a.over_cap ? " (more than the list keeps)" : "",
                 trial ? " (rules on trial)" : "");
        any_whole = any_whole || result == APP_GPHOTOS_OK || result == APP_GPHOTOS_ERR_EMPTY;
        any_ok = any_ok || result == APP_GPHOTOS_OK;
        if (result == APP_GPHOTOS_OK) {
            list_store(slot, &a);
            list_install(slot, &a);
        }
        album_free(&a);

        // THE OWNER'S RULE OF 2026-09-13, per slot: a NEW link that cannot be read, or holds
        // nothing, is not kept. The last link that read whole with photographs in it is put back
        // and the failure is reported, so the frame goes on showing the album it was showing and
        // the page says why the change did not take. The list needs nothing: it is only ever
        // replaced by a whole read, so it is still the good link's.
        //
        // Only when the link that failed is still the stored one -- a newer edit wins -- and is not
        // the good one: a good album that fails on a daily re-read is a failure to report, not a
        // change to undo. Out of memory is the device's problem, not the link's, so it never
        // reverts.
        if (good && cur && result != APP_GPHOTOS_ERR_NO_MEM) {
            app_settings_gphotos_good(slot, good, APP_SETTINGS_GPHOTOS_ALBUM_SIZE);
            app_settings_gphotos_album(slot, cur, APP_SETTINGS_GPHOTOS_ALBUM_SIZE);
            if (result == APP_GPHOTOS_OK) {
                as->reverted = false;
                if (strcmp(good, url) != 0) {
                    app_settings_set_gphotos_good(slot, url);
                }
            } else if (good[0] && strcmp(url, good) != 0 && strcmp(cur, url) == 0) {
                app_settings_set_gphotos_album(slot, good);
                as->reverted = true;
                as->reverted_result = result;
                as->reverted_http = as->last_http_status;
                ESP_LOGW(TAG,
                         "album %u: the new link failed (%s, http %d); the previous link is back",
                         (unsigned)slot, app_gphotos_result_str(result), as->last_http_status);
            }
        }

        as->last_result = result;
        if (result == APP_GPHOTOS_OK) {
            as->last_ok_ms = esp_timer_get_time() / 1000;
        }
        lock();
        const bool listed = s_lists[slot].keys != NULL;
        unlock();
        any_listed = any_listed || listed;
    }
    heap_caps_free(good);
    heap_caps_free(cur);
    if (trial) {
        app_gphotos_rules_verdict(any_whole, any_ok);
    }
    heap_caps_free(rules);

    // Photographs kept on /data whose key is in NO album's list go now, and every list came from a
    // WHOLE read. A slot whose read failed keeps its previous list, so its photographs stay.
    //
    // A configured slot holding NO list at all does not block this. Its photographs cannot be on
    // /data through this build's selector, which only asks for keys that are in a list; and
    // blocking on it meant one empty album -- 200 with no payload, which never reverts when there
    // is no earlier good link -- stopped deletion for every other album for as long as it stayed
    // configured (found on the bench 2026-09-13, three albums: 167, 4, empty). The one case it
    // costs is a list FILE lost while its photographs are kept: they are deleted and fetched again
    // when shown. At least one list is still required, so a run where every read failed from boot
    // deletes nothing.
    if (any_listed) {
        index_load(m);
        uint16_t deleted = 0;
        for (size_t i = smb_manifest_count(m); i-- > 0;) {
            if (!list_has(smb_manifest_path(m, i))) {
                app_smb_sync_delete_photo(smb_manifest_local(m, i));
                smb_manifest_remove_at(m, i);
                deleted++;
            }
        }
        if (deleted > 0) {
            index_store(m);
            app_slideshow_invalidate();
        }
        st->deleted = (uint16_t)(st->deleted + deleted);
        st->owned = (uint16_t)smb_manifest_count(m);
    }

    st->last_run_ms = esp_timer_get_time() / 1000;
    s_last_list_end_us = esp_timer_get_time();
}

static void run_task(void *arg)
{
    (void)arg;
    const uint8_t work = s_work;
    app_gphotos_status_t st;
    lock();
    st = s_status;
    unlock();
    st.running = true;
    publish(&st);
    // Before a list is read or a photograph fetched: both go by the rules in force.
    app_gphotos_rules_load();

    if (work & WORK_LOAD) {
        for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
            list_load(s);
        }
        s_list_loaded = true;
        smb_manifest_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_SPIRAM);
        char *arena = heap_caps_malloc(GPHOTOS_ARENA, MALLOC_CAP_SPIRAM);
        if (m && arena && smb_manifest_init(m, arena, GPHOTOS_ARENA)) {
            index_load(m);
            st.owned = (uint16_t)smb_manifest_count(m);
        }
        heap_caps_free(m);
        heap_caps_free(arena);
    }

    if (work & (WORK_FETCH | WORK_LIST)) {
        const size_t url_size = APP_SETTINGS_GPHOTOS_ALBUM_SIZE + 64;
        char *url = heap_caps_malloc(url_size, MALLOC_CAP_SPIRAM);
        smb_manifest_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_SPIRAM);
        char *arena = heap_caps_malloc(GPHOTOS_ARENA, MALLOC_CAP_SPIRAM);
        uint8_t *photo = (work & WORK_FETCH) ? heap_caps_malloc(GPHOTOS_PHOTO_MAX, MALLOC_CAP_SPIRAM)
                                             : NULL;
        char *ua = heap_caps_malloc(GPHOTOS_UA_SIZE, MALLOC_CAP_SPIRAM);
        const bool ready = url && ua && m && arena && smb_manifest_init(m, arena, GPHOTOS_ARENA);
        if (ua) {
            app_gphotos_rules_ua(ua, GPHOTOS_UA_SIZE);
        }
        // The fetch first: the slideshow is waiting on it, and the list can wait an advance.
        if (ready && (work & WORK_FETCH) && photo) {
            do_fetch(&st, m, photo, url, url_size, ua);
        }
        if (ready && (work & WORK_LIST)) {
            do_list(&st, m, url, url_size, ua);
        }
        heap_caps_free(ua);
        heap_caps_free(url);
        heap_caps_free(m);
        heap_caps_free(arena);
        heap_caps_free(photo);
    }

    st.running = false;
    publish(&st);
    lock();
    const size_t listed = total_locked();
    unlock();
    ESP_LOGI(TAG, "task: work=%u, %u listed, %u kept, stack %u free", (unsigned)work,
             (unsigned)listed, (unsigned)st.owned,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_running = false;
    vTaskDelete(NULL);
}

void app_gphotos_sync_tick(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return;
        }
        app_gphotos_rules_init();
#ifdef GPHOTOS_BENCH_ONLY_SLOT
        printf("# BENCH gphotos: slideshow selects from album slot %d only, SMB excluded\n",
               (int)GPHOTOS_BENCH_ONLY_SLOT);
#endif
    }
    const bool enabled = app_settings_gphotos_enabled();
    lock();
    s_status.enabled = enabled;
    unlock();
    if (!enabled) {
        s_list_request = false;
        return;
    }
    if (s_running) {
        return;
    }
    // The SMB mirror's rule, for its reason: with the card out /data is the 6 MB internal volume.
    if (board_storage_get_media() != BOARD_STORAGE_MEDIA_SD) {
        return;
    }

    uint8_t work = s_list_loaded ? 0 : WORK_LOAD;
    board_wifi_status(&s_wifi);
    if (s_wifi.sta_connected) {
        lock();
        const bool want = s_want;
        unlock();
        if (want) {
            work |= WORK_FETCH;
        }
        const int64_t now = esp_timer_get_time();
        bool due;
        if (s_list_request) {
            due = true;
        } else if (s_last_list_end_us == INT64_MIN) {
            due = now >= (int64_t)GPHOTOS_FIRST_DELAY_MS * 1000;
        } else {
            // Held outside active hours like the SMB mirror's periodic run (ticket 42).
            due = (now - s_last_list_end_us) >= (int64_t)GPHOTOS_PERIOD_MS * 1000 &&
                  app_clock_schedule_active();
        }
        if (due) {
            work |= WORK_LIST;
        }
    }
    if (work == 0) {
        return;
    }

    const bool was_requested = s_list_request;
    if (work & WORK_LIST) {
        s_list_request = false;
    }
    s_work = work;
    s_running = true;
    if (xTaskCreate(run_task, "gphotos", GPHOTOS_STACK, NULL, GPHOTOS_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no internal RAM for the task; retrying at the next tick");
        s_list_request = was_requested;
        s_running = false;
    }
}

void app_gphotos_sync_request(void)
{
    // A save starts a new attempt, so a previous revert no longer describes what is being tried.
    if (s_mutex) {
        lock();
        for (size_t s = 0; s < APP_SETTINGS_GPHOTOS_SLOTS; s++) {
            s_status.albums[s].reverted = false;
        }
        unlock();
    }
    s_list_request = true;
}

void app_gphotos_sync_get_status(app_gphotos_status_t *out)
{
    if (!s_mutex) {
        *out = s_status;
        return;
    }
    lock();
    *out = s_status;
    out->running = s_running;
    unlock();
}

// ------------------------------------------------------------ the selector's side

size_t app_gphotos_sync_count(void)
{
    if (!s_mutex || !app_settings_gphotos_enabled()) {
        return 0;
    }
    lock();
    const size_t n = total_locked();
    unlock();
    return n;
}

// Hashed under the lock, straight off the list: copying a 256-byte key out first would put it on
// the main task's stack.
bool app_gphotos_sync_local(size_t i, char *local, size_t local_size)
{
    if (!s_mutex || !local || local_size == 0) {
        return false;
    }
    lock();
    const char *key = key_at_locked(i, NULL);
    const bool ok = key && gphotos_local_name(key, strlen(key), local, local_size);
    unlock();
    return ok;
}

void app_gphotos_sync_want(const uint16_t *index, size_t n)
{
    if (!s_mutex) {
        return;
    }
    lock();
    const char *id = NULL;
    const char *key = (index && n > 0) ? key_at_locked(index[0], &id) : NULL;
    if (key) {
        memcpy(s_want_key, key, sizeof(s_want_key));
        memcpy(s_want_id, id, sizeof(s_want_id));
        s_want = true;
    } else {
        s_want = false;
    }
    s_status.want_pending = s_want ? 1 : 0;
    unlock();
}
