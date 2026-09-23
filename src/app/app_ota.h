// Firmware updates without USB: the writer, the board check and the rollback. Ticket 61.
//
// The operator's choices of 2026-09-22, which this file is the whole of:
//
//   * AN IMAGE IS SIGNED with ESP-IDF's own RSA-3072 scheme (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT)
//     and esp_ota_end() refuses one that does not verify. The trusted key is the one the RUNNING
//     image carries, so a USB-flashed image must be signed too -- tools/sign_firmware.py does it at
//     build time. No eFuse is burned; USB still flashes anything.
//   * IT MUST NAME THIS BOARD. Both boards' images are signed by the same key, so the signature says
//     nothing about which board an image is for. Each image carries a board identity right after its
//     esp_app_desc_t, and one naming the other board is refused before the boot slot changes.
//   * NO VERSION ORDER. Any signed image for this board is accepted, older ones included: the
//     operator's answer to "a bad release" is to publish the previous one, and fw_version is not
//     monotonic anyway (docs/agents/build-system.md).
//   * A NEW IMAGE IS ON TRIAL (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) until app_ota_tick() sees the
//     web server up and the station on the network, APP_OTA_VALID_AFTER_S into the boot. A reset
//     before that boots the previous slot; no verdict by APP_OTA_GIVE_UP_S rolls back on purpose.
//
// Push only for now: POST /api/system/ota streams the body into begin/write/finish. The pull from a
// URL is ticket 61's next step and will feed the same three calls.
//
// Every call except app_ota_get_status() is from ONE task at a time: the writer from httpd's only
// worker, the boot check and the tick from main.

#ifndef APP_OTA_H
#define APP_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_OTA_VALID_AFTER_S 120
#define APP_OTA_GIVE_UP_S 600

typedef struct {
    const char *running;     // the running slot's label, "" if unknown
    // "valid", "pending" or "?". A USB flash reads "valid" too: with no `factory`, the bootloader
    // writes ota_0 into the blank otadata as VALID on the first boot (set_actual_ota_seq()).
    const char *state;
    const char *board;       // this image's board identity
    bool writing;
    uint32_t written;        // bytes of the image being written, or of the last one
    const char *last_result; // "none", "ok", or why the last attempt failed; a literal
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

void app_ota_get_status(app_ota_status_t *out);

#endif // APP_OTA_H
