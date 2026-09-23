// Google Photos as a second photograph source: the album's LIST is mirrored, and its photographs
// are fetched one at a time shortly before the slideshow shows them.
//
// Ticket .scratch/digital-frame/issues/60, FR-10; the Google Photos mirror plan. The owner's
// ruling of 2026-09-13 replaced a whole-album mirror with this: "ダウンロードの全体ミラーを止め、
// SMBと同じく都度。但しリスト作成レベルのみミラー可能" -- fetch on demand like the SMB share, and mirror
// no more than the list.
//
// So there are two jobs, on one transient task:
//
//   * THE LISTS. Once a day, or when the page asks, each configured album's HTML is read and its
//     photographs' keys are kept in PSRAM and in /data/.gpdir (slot 0) or .gpdirN (slots 1-3),
//     which are loaded at boot so the slideshow can choose before the network is up. A read that
//     is not whole, or finds nothing, replaces nothing. The slideshow selects uniformly over the
//     photographs of every album together (the owner's choice, 2026-09-13).
//   * ONE PHOTOGRAPH. The slideshow's catalogue epoch (through app_catalog) declares the album
//     entry it is about to want; this fetches that one photograph, stores it through the SMB
//     mirror's store path, and keeps at most GPHOTOS_CACHE_FILES of them on /data, oldest first.
//
// Four things are load-bearing:
//
//   * .gpidx IS THE OWNERSHIP RECORD of the photographs on /data, in smb_manifest's format. A file
//     it does not name is never touched.
//   * THE WANT COPIES THE KEY, not the index: the list can be replaced between an ask and the
//     fetch, and an index would then name some other photograph.
//   * httpd IS NOT STOPPED. A TLS fetch ran alongside httpd and back-to-back uploads, 32 of 32, at
//     CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y (ticket 60 §3c).
//   * THE ALBUM LINK AND EVERY PHOTO URL ARE BEARER CAPABILITIES and are never logged. Local names
//     are hashes; the log carries those and counts.

#ifndef APP_GPHOTOS_SYNC_H
#define APP_GPHOTOS_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_settings.h"

typedef enum {
    APP_GPHOTOS_NEVER = 0,     // no album read has finished since boot
    APP_GPHOTOS_OK,
    APP_GPHOTOS_ERR_CONNECT,   // the album could not be opened or read: DNS, TLS, the network
    APP_GPHOTOS_ERR_HTTP,      // it answered with a status other than 200 -- 404 is an album that
                               // is account-shared rather than link-shared (FR-10.1)
    APP_GPHOTOS_ERR_TRUNCATED, // the document did not arrive whole, so the list was kept
    APP_GPHOTOS_ERR_EMPTY,     // a whole document and no photograph in it: the markup changed
    APP_GPHOTOS_ERR_NO_MEM,
} app_gphotos_result_t;

// One album slot's part of FR-10.4: its last ALBUM READ.
typedef struct {
    app_gphotos_result_t last_result;
    int last_http_status;
    int64_t last_ok_ms;    // uptime when this slot's last whole read ended, -1 never
    uint32_t album_bytes;  // document size of the last read
    uint16_t items;        // photographs in this slot's list
    bool over_cap;         // the album holds more than the list keeps
    // A NEW link failed and the last good one was put back (the owner's rule of 2026-09-13):
    // why, and the HTTP status. Cleared by the next save or the next whole read.
    bool reverted;
    app_gphotos_result_t reverted_result;
    int reverted_http;
} app_gphotos_album_status_t;

// FR-10.4's material. The counters are since boot.
typedef struct {
    bool enabled;
    bool running;
    int64_t last_run_ms;   // uptime when the last album run ended, -1 before the first
    uint16_t owned;        // photographs kept on /data (.gpidx)
    uint8_t want_pending;  // a photograph the slideshow has asked for and not yet been given
    uint16_t fetched;      // since boot
    uint16_t failed;       // since boot
    uint16_t deleted;      // since boot: kept photographs whose key left every album
    uint16_t evicted;      // since boot: kept photographs dropped to stay at the cache size
    app_gphotos_album_status_t albums[APP_SETTINGS_GPHOTOS_SLOTS];
} app_gphotos_status_t;

// From the application loop. Never blocks.
void app_gphotos_sync_tick(void);

// Re-read every album at the next tick, whatever the schedule says. A save and the page's Sync now.
void app_gphotos_sync_request(void);

void app_gphotos_sync_get_status(app_gphotos_status_t *out);
const char *app_gphotos_result_str(app_gphotos_result_t r);

// The selector's side, through app_catalog. Same contract as app_smb_sync_catalog_*: no I/O, and
// `want` carries only entries found absent from /data. Called from the main task.
size_t app_gphotos_sync_count(void);
bool app_gphotos_sync_local(size_t i, char *local, size_t local_size);
void app_gphotos_sync_want(const uint16_t *index, size_t n);

#endif // APP_GPHOTOS_SYNC_H
