#include "app_ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heapwatch.h"

#include "app_display.h"
#include "app_gphotos_sync.h"
#include "app_heapwatch.h"
#include "app_settings.h"
#include "board_storage.h"

static const char *TAG = "ota";

#if defined(BOARD_M5PAPER_COLOR)
#define OTA_BOARD "m5papercolor"
#elif defined(BOARD_RETERMINAL_E1002)
#define OTA_BOARD "reterminal_e1002"
#else
#error "no board selected"
#endif

// The pull's source: this board's asset on the public repository's latest release. `latest` skips
// drafts and prereleases, so publishing a release is the whole of shipping one. Built in rather
// than a platformio.ini flag because platformio_local.ini replaces `build_flags` for env:frame and a
// bench build would lose it silently. -DFRAME_OTA_URL='""' turns the pull off.
#ifndef FRAME_OTA_URL
#define FRAME_OTA_URL                                                                             \
    "https://github.com/et147551-netizen/paperframe-esp32/releases/latest/download/paperframe-" \
    OTA_BOARD ".bin"
#endif

#define IDENT_MAGIC "FRAMEID"

// The board identity every image carries. `.rodata_custom_desc` is the linker's slot for exactly
// this -- sections.ld places it straight after esp_app_desc_t at the start of the first DROM
// segment -- so it sits at a fixed offset in the image and can be read out of a slot that has only
// been written, never run.
typedef struct {
    char magic[8];
    char board[24];
} ota_ident_t;

__attribute__((section(".rodata_custom_desc"), used)) static const ota_ident_t s_ident = {
    IDENT_MAGIC,
    OTA_BOARD,
};

#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define IDENT_OFFSET (DESC_OFFSET + sizeof(esp_app_desc_t))
// What the pull reads before deciding whether to write anything.
#define PULL_HEAD (IDENT_OFFSET + sizeof(ota_ident_t))
#define PULL_CHUNK 4096
// Internal RAM: a task that writes flash may not have its stack in PSRAM.
#define PULL_STACK 8192
// After a pull that could not reach the server, try again sooner than a week: a site's router
// being down for an evening should not cost seven days.
#define PULL_RETRY_S 3600

static const esp_partition_t *s_target;
static esp_ota_handle_t s_handle;
static bool s_writing;
static uint32_t s_written;
static const char *s_last = "none";
static bool s_pending;

// s_writing and s_pulling are claimed under this, so a push and a pull cannot both hold the writer.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_pulling;
static TaskHandle_t s_pull_task;
static int64_t s_last_check_s = -1;
static bool s_last_check_failed;
static int s_last_http;
static const char *s_last_code = "none";

static bool read_ident(const esp_partition_t *p, ota_ident_t *out)
{
    return esp_partition_read(p, IDENT_OFFSET, out, sizeof(*out)) == ESP_OK &&
           memcmp(out->magic, IDENT_MAGIC, sizeof(IDENT_MAGIC)) == 0;
}

static const char *state_str(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending";
    case ESP_OTA_IMG_UNDEFINED:
        return "undefined";
    default:
        return "?";
    }
}

static const char *running_state(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        // Not seen on hardware: the bootloader writes a state on the first boot after a USB flash.
        return "undefined";
    }
    return state_str(st);
}

void app_ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    s_pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
                st == ESP_OTA_IMG_PENDING_VERIFY;
    // `ident=` checks the placement the whole board check rests on, against this very image: 0 means
    // .rodata_custom_desc did not land at IDENT_OFFSET, and every update would be refused.
    ota_ident_t mine;
    const bool ident_ok = run && read_ident(run, &mine) && strcmp(mine.board, s_ident.board) == 0;
    printf("# ota: running=%s state=%s board=%s ident=%d\n", run ? run->label : "?",
           running_state(), s_ident.board, ident_ok ? 1 : 0);
#if defined(FRAME_OTA_TEST_FAIL) && FRAME_OTA_TEST_FAIL == 1
    // Bench only: an image that panics while on trial, to watch the bootloader roll it back. It
    // runs normally once it is not on trial, so a USB flash of it is harmless.
    if (s_pending) {
        printf("# ota: FRAME_OTA_TEST_FAIL=1, panicking on trial\n");
        fflush(stdout);
        abort();
    }
#endif
}

// The automatic pull. Never while on trial (the tick returns before this), and never twice at once.
// A refusal -- a Google Photos run holding the network, say -- is simply retried at the next tick.
static void pull_schedule(bool sta_connected)
{
    if (!FRAME_OTA_URL[0] || !sta_connected || s_pulling || !app_settings_ota_auto()) {
        return;
    }
    const int64_t up_s = esp_timer_get_time() / 1000000;
    const int64_t due = s_last_check_s < 0 ? APP_OTA_FIRST_CHECK_S
                        : s_last_check_s + (s_last_check_failed ? PULL_RETRY_S : APP_OTA_EVERY_S);
    if (up_s < due) {
        return;
    }
    const char *why;
    if (app_ota_pull_start(&why) == ESP_OK) {
        printf("# ota: automatic check at %llds\n", (long long)up_s);
    }
}

void app_ota_tick(bool httpd_ok, bool sta_connected)
{
    if (!s_pending) {
        pull_schedule(sta_connected);
        return;
    }
    const int64_t up_s = esp_timer_get_time() / 1000000;
#if defined(FRAME_OTA_TEST_FAIL) && FRAME_OTA_TEST_FAIL == 2
    // Bench only: an image that runs but never earns its verdict, to exercise the give-up below.
    sta_connected = false;
#endif
    if (httpd_ok && sta_connected && up_s >= APP_OTA_VALID_AFTER_S) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        printf("# ota: marked valid after %llds: %s\n", (long long)up_s, esp_err_to_name(err));
        s_pending = false;
        return;
    }
    if (up_s >= APP_OTA_GIVE_UP_S) {
        // Reached only by an image that runs but never gets the web UI onto the network -- the one
        // failure a reset alone would not undo, because nothing resets.
        printf("# ota: no verdict after %llds (httpd=%d sta=%d); rolling back\n", (long long)up_s,
               httpd_ok ? 1 : 0, sta_connected ? 1 : 0);
        fflush(stdout);
        app_display_wait_idle(60000);
        board_storage_lock();
        esp_ota_mark_app_invalid_rollback_and_reboot();
        board_storage_unlock();  // only reached if there was nothing to roll back to
        s_pending = false;
    }
}

esp_err_t app_ota_begin(size_t size, const char **why)
{
    // Claimed before esp_ota_begin(), so the other transport sees the writer taken from here on. A
    // push is refused while a pull runs at all, not only while it writes: the pull decides to write
    // seconds after it starts. A push that finds s_writing set is replacing an abandoned push.
    const bool from_pull = s_pulling && xTaskGetCurrentTaskHandle() == s_pull_task;
    taskENTER_CRITICAL(&s_mux);
    const bool refuse = from_pull ? s_writing : s_pulling;
    const bool stale = !refuse && s_writing;
    if (!refuse) {
        s_writing = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (refuse) {
        *why = from_pull ? "an upload is being written" : "an update is being downloaded";
        return ESP_ERR_INVALID_STATE;
    }
    if (stale) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) {
        s_writing = false;
        *why = s_last = "no OTA slot (the partition table predates ticket 61)";
        return ESP_ERR_NOT_FOUND;
    }
    if (size > s_target->size) {
        s_writing = false;
        *why = s_last = "image larger than the slot";
        return ESP_ERR_INVALID_SIZE;
    }
    // Sequential: erasing the whole 4.9 MB slot up front would hold httpd's only worker for
    // seconds before the first byte is read.
    const esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        s_writing = false;
        // ESP_ERR_OTA_ROLLBACK_INVALID_STATE here is the running image still on trial.
        *why = s_last = err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE
                            ? "the running image is still on trial"
                            : "esp_ota_begin failed";
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }
    s_written = 0;
    printf("# ota: writing %u bytes to %s\n", (unsigned)size, s_target->label);
    return ESP_OK;
}

esp_err_t app_ota_write(const void *data, size_t len, const char **why)
{
    const esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        // ESP_ERR_OTA_VALIDATE_FAILED at the first chunk is a body that is not an app image.
        *why = s_last = err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a firmware image"
                                                           : "flash write failed";
        ESP_LOGE(TAG, "esp_ota_write at %u: %s", (unsigned)s_written, esp_err_to_name(err));
        app_ota_abort();
        return err;
    }
    s_written += (uint32_t)len;
    return ESP_OK;
}

esp_err_t app_ota_finish(const char **why)
{
    s_writing = false;
    // Verifies the image and, under CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, its signature.
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        *why = s_last = err == ESP_ERR_OTA_VALIDATE_FAILED ? "image or signature did not verify"
                                                           : "esp_ota_end failed";
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }
    ota_ident_t theirs;
    if (!read_ident(s_target, &theirs)) {
        *why = s_last = "the image carries no board identity";
        return ESP_ERR_INVALID_VERSION;
    }
    theirs.board[sizeof(theirs.board) - 1] = '\0';
    if (strcmp(theirs.board, s_ident.board) != 0) {
        *why = s_last = "the image is for the other board";
        ESP_LOGE(TAG, "image is for %s, this is %s", theirs.board, s_ident.board);
        return ESP_ERR_INVALID_VERSION;
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        *why = s_last = "esp_ota_set_boot_partition failed";
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }
    s_last = "ok";
    printf("# ota: %u bytes in %s verified; it boots next\n", (unsigned)s_written, s_target->label);
    return ESP_OK;
}

void app_ota_abort(void)
{
    if (s_writing) {
        esp_ota_abort(s_handle);
    }
    s_writing = false;
    s_handle = 0;
}

// A streaming open does not follow redirects by itself, and a release asset is two of them
// (github.com, then the asset host). The same loop as app_gphotos_sync.c's open_follow().
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

// Reads until `want` bytes are in `buf` or the body ends. The count read.
static size_t read_full(esp_http_client_handle_t c, uint8_t *buf, size_t want)
{
    size_t got = 0;
    while (got < want) {
        const int r = esp_http_client_read(c, (char *)buf + got, (int)(want - got));
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    return got;
}

static bool same_sha(const uint8_t *a, const esp_partition_t *p)
{
    esp_app_desc_t d;
    return p && esp_ota_get_partition_description(p, &d) == ESP_OK &&
           memcmp(a, d.app_elf_sha256, sizeof(d.app_elf_sha256)) == 0;
}

// Decides from the first PULL_HEAD bytes, before anything is written. NULL means "write it".
static const char *judge_head(const uint8_t *head, const char **why)
{
    esp_app_desc_t desc;
    ota_ident_t ident;
    memcpy(&desc, head + DESC_OFFSET, sizeof(desc));
    memcpy(&ident, head + IDENT_OFFSET, sizeof(ident));
    ident.board[sizeof(ident.board) - 1] = '\0';
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(ident.magic, IDENT_MAGIC, sizeof(IDENT_MAGIC)) != 0) {
        *why = "the release is not a frame firmware image";
        return "failed";
    }
    if (strcmp(ident.board, s_ident.board) != 0) {
        *why = "the release is for the other board";
        return "other_board";
    }
    if (memcmp(desc.app_elf_sha256, esp_app_get_description()->app_elf_sha256,
               sizeof(desc.app_elf_sha256)) == 0) {
        *why = "up to date";
        return "up_to_date";
    }
    // The slot a rolled-back image was in keeps it until the next write, so its description is
    // still there to compare -- which is what stops a bad release being installed every week.
    if (same_sha(desc.app_elf_sha256, esp_ota_get_last_invalid_partition())) {
        *why = "this release was rolled back before";
        return "rolled_back";
    }
    return NULL;
}

static void pull_task(void *arg)
{
    (void)arg;
    s_pull_task = xTaskGetCurrentTaskHandle();
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, true);

    const char *why = "the update server could not be reached";
    const char *code = "net_error";
    int status = 0;
    bool install = false;

    // PSRAM: the task only reads into it and hands it to esp_ota_write(), which copies.
    uint8_t *buf = heap_caps_malloc(PULL_CHUNK, MALLOC_CAP_SPIRAM);
    // tx at 2 KB: the asset host's signed URL is several hundred bytes and goes in the request line.
    const esp_http_client_config_t cfg = {
        .url = FRAME_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .timeout_ms = 30000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = buf ? esp_http_client_init(&cfg) : NULL;
    if (!c) {
        why = "no memory for the download";
        code = "failed";
    } else if (open_follow(c, &status) == ESP_OK) {
        const int64_t len = esp_http_client_get_content_length(c);
        if (status == 404) {
            why = "no release found";
            code = "no_release";
        } else if (status != 200) {
            why = "the update server answered an error";
            code = "http_error";
        } else if (len < (int64_t)PULL_HEAD) {
            why = "the release has no usable length";
            code = "failed";
        } else {
            size_t got = read_full(c, buf, PULL_HEAD);
            if (got < PULL_HEAD) {
                why = "the download stopped";
            } else if ((code = judge_head(buf, &why)) == NULL) {
                printf("# ota: pull %lld bytes from HTTP %d\n", (long long)len, status);
                if (app_ota_begin((size_t)len, &why) != ESP_OK) {
                    code = "refused";
                } else {
                    esp_err_t err = app_ota_write(buf, got, &why);
                    size_t total = got;
                    while (err == ESP_OK && total < (size_t)len) {
                        const size_t want = (size_t)len - total < PULL_CHUNK
                                                ? (size_t)len - total : PULL_CHUNK;
                        got = read_full(c, buf, want);
                        if (got == 0) {
                            break;
                        }
                        err = app_ota_write(buf, got, &why);
                        total += got;
                    }
                    if (err != ESP_OK) {
                        code = "failed";  // app_ota_write() has aborted and said why
                    } else if (total != (size_t)len) {
                        app_ota_abort();
                        why = "the download stopped";
                        code = "net_error";
                    } else if (app_ota_finish(&why) != ESP_OK) {
                        code = "failed";
                    } else {
                        why = "verified; restarting into it";
                        code = "installing";
                        install = true;
                    }
                }
            }
        }
        esp_http_client_close(c);
    }
    if (c) {
        esp_http_client_cleanup(c);
    }
    heap_caps_free(buf);
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, false);

    s_last_http = status;
    s_last_code = code;
    s_last = install ? "ok" : why;
    s_last_check_s = esp_timer_get_time() / 1000000;
    s_last_check_failed = strcmp(code, "net_error") == 0 || strcmp(code, "http_error") == 0;
    printf("# ota: pull done http=%d code=%s: %s\n", status, code, why);
    fflush(stdout);

    if (install) {
        // The same order as the push: no restart under a refresh or a volume write.
        app_display_wait_idle(60000);
        board_storage_lock();
        esp_restart();
    }
    s_pulling = false;
    vTaskDelete(NULL);
}

esp_err_t app_ota_pull_start(const char **why)
{
    if (!FRAME_OTA_URL[0]) {
        *why = "this build has no update address";
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_pending) {
        *why = "the running image is still on trial";
        return ESP_ERR_INVALID_STATE;
    }
    // One TLS session at a time: two beside httpd is what ticket 60 §3b found failing.
    app_gphotos_status_t gp;
    app_gphotos_sync_get_status(&gp);
    if (gp.running) {
        *why = "Google Photos is using the network; try again shortly";
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_mux);
    const bool busy = s_pulling || s_writing;
    if (!busy) {
        s_pulling = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (busy) {
        *why = "an update is already in progress";
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(pull_task, "ota_pull", PULL_STACK, NULL, 4, NULL) != pdPASS) {
        s_pulling = false;
        *why = "no memory to start the download";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_ota_writing(void)
{
    return s_writing;
}

void app_ota_get_status(app_ota_status_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    out->running = run ? run->label : "";
    out->state = running_state();
    out->board = s_ident.board;
    out->writing = s_writing;
    out->written = s_written;
    out->last_result = s_last;
    out->url_set = FRAME_OTA_URL[0] != '\0';
    out->checking = s_pulling;
    out->last_check_s = s_last_check_s;
    out->last_http = s_last_http;
    out->last_code = s_last_code;
}
