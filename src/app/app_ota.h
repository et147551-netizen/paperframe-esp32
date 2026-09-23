// Firmware updates without USB: the writer, the board check and the rollback. Ticket 61.
//
// The owner's choices of 2026-09-22, which this file is the whole of:
//
//   * AN IMAGE IS SIGNED with ESP-IDF's own RSA-3072 scheme (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT)
//     and esp_ota_end() refuses one that does not verify. The trusted key is the one the RUNNING
//     image carries, so a USB-flashed image must be signed too -- tools/sign_firmware.py does it at
//     build time. No eFuse is burned; USB still flashes anything.
//   * IT MUST NAME THIS BOARD. Both boards' images are signed by the same key, so the signature says
//     nothing about which board an image is for. Each image carries a board identity right after its
//     esp_app_desc_t, and one naming the other board is refused before the boot slot changes.
//   * NO VERSION ORDER. Any signed image for this board is accepted, older ones included: the
//     owner's answer to "a bad release" is to publish the previous one, and fw_version is not
//     monotonic anyway (docs/build-system.md).
//   * A NEW IMAGE IS ON TRIAL (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) until app_ota_tick() sees the
//     web server up and the station on the network, APP_OTA_VALID_AFTER_S into the boot. A reset
//     before that boots the previous slot; no verdict by APP_OTA_GIVE_UP_S rolls back on purpose.
//
//   * TWO TRANSPORTS, ONE WRITER. POST /api/system/ota streams an upload into begin/write/finish;
//     the PULL fetches FRAME_OTA_URL -- a GitHub "latest release" asset for this board -- into the
//     same three calls, from a one-shot task. It runs APP_OTA_FIRST_CHECK_S after boot and every
//     APP_OTA_EVERY_S after that, or on demand (POST /api/system/ota/check). "Is there anything to
//     fetch" is the release's app_elf_sha256 against the running image's, read from the first bytes
//     of the download before anything is written; one that was already rolled back is skipped, so a
//     bad release is not re-installed every week.
//
// The boot check and the tick are main's; the push is httpd's only worker; the pull is its own task.
// A push and a pull never write at once: each refuses while the other holds the writer.

#ifndef APP_OTA_H
#define APP_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_OTA_VALID_AFTER_S 120
#define APP_OTA_GIVE_UP_S 600

#ifndef APP_OTA_FIRST_CHECK_S
#define APP_OTA_FIRST_CHECK_S 600
#endif
#ifndef APP_OTA_EVERY_S
#define APP_OTA_EVERY_S (7 * 24 * 3600)
#endif

typedef struct {
    const char *running;     // the running slot's label, "" if unknown
    // "valid", "pending" or "?". A USB flash reads "valid" too: with no `factory`, the bootloader
    // writes ota_0 into the blank otadata as VALID on the first boot (set_actual_ota_seq()).
    const char *state;
    const char *board;       // this image's board identity
    bool writing;
    uint32_t written;        // bytes of the image being written, or of the last one
    const char *last_result; // "none", "ok", or why the last attempt failed; a literal
    bool url_set;            // the pull has a URL; false disables it
    bool checking;           // a pull is running
    int64_t last_check_s;    // uptime when the last pull ended, -1 before the first
    int last_http;           // that pull's HTTP status, 0 if it got none
    // That pull's outcome as a stable word the page translates: "none", "up_to_date", "installing",
    // "no_release", "http_error", "net_error", "other_board", "rolled_back", "refused", "failed".
    const char *last_code;
} app_ota_status_t;

// Main, once at boot: reads whether this image is on trial and prints the line that says so.
void app_ota_boot_check(void);

// Main, every 10 s: marks a trial image valid, or rolls it back.
void app_ota_tick(bool httpd_ok, bool sta_connected);

// httpd. `size` is the whole image. *why is a literal on failure.
esp_err_t app_ota_begin(size_t size, const char **why);
esp_err_t app_ota_write(const void *data, size_t len, const char **why);
// Checks the signature and the board, then makes the new slot the boot slot. Nothing restarts.
esp_err_t app_ota_finish(const char **why);
void app_ota_abort(void);

// Starts a pull now. ESP_OK when the task started; otherwise *why says why not (a literal): no URL,
// the image on trial, a write or a pull already running, a mirror holding the network, no memory.
esp_err_t app_ota_pull_start(const char **why);

// True while an image is being written by either transport. The slideshow holds its advance on it.
bool app_ota_writing(void);

void app_ota_get_status(app_ota_status_t *out);

#endif // APP_OTA_H
