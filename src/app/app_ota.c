#include "app_ota.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "app_display.h"
#include "board_storage.h"

static const char *TAG = "ota";

#if defined(BOARD_M5PAPER_COLOR)
#define OTA_BOARD "m5papercolor"
#elif defined(BOARD_RETERMINAL_E1002)
#define OTA_BOARD "reterminal_e1002"
#else
#error "no board selected"
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

#define IDENT_OFFSET \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static const esp_partition_t *s_target;
static esp_ota_handle_t s_handle;
static bool s_writing;
static uint32_t s_written;
static const char *s_last = "none";
static bool s_pending;

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

void app_ota_tick(bool httpd_ok, bool sta_connected)
{
    if (!s_pending) {
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
    if (s_writing) {
        app_ota_abort();
    }
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) {
        *why = s_last = "no OTA slot (the partition table predates ticket 61)";
        return ESP_ERR_NOT_FOUND;
    }
    if (size > s_target->size) {
        *why = s_last = "image larger than the slot";
        return ESP_ERR_INVALID_SIZE;
    }
    // Sequential: erasing the whole 4.9 MB slot up front would hold httpd's only worker for
    // seconds before the first byte is read.
    const esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        // ESP_ERR_OTA_ROLLBACK_INVALID_STATE here is the running image still on trial.
        *why = s_last = err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE
                            ? "the running image is still on trial"
                            : "esp_ota_begin failed";
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }
    s_writing = true;
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

void app_ota_get_status(app_ota_status_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    out->running = run ? run->label : "";
    out->state = running_state();
    out->board = s_ident.board;
    out->writing = s_writing;
    out->written = s_written;
    out->last_result = s_last;
}
