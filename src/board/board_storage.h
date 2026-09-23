// /data, on either the internal wear-levelled FAT volume or a microSD card.
//
// Ticket .scratch/digital-frame/issues/08; satisfies FR-4.1 through FR-4.3, FR-4.5
// and FR-4.6. The shape follows
// refs/M5PaperColor-UserDemo/main/hal/storage/hal_storage.{h,cpp}, because tickets
// 09, 11 and 13 are written against that contract.
//
// The card is SDSPI on the panel's bus (board_spi.h), so every access here has to be
// excluded from a refresh. board_storage_lock() takes board_spi_lock() for you; do
// not take them the other way round.

#ifndef BOARD_STORAGE_H
#define BOARD_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "photo_list.h"

#define BOARD_STORAGE_MOUNT "/data"

typedef enum {
    BOARD_STORAGE_MEDIA_FLASH = 0, // internal 6 MB FAT over wear levelling
    BOARD_STORAGE_MEDIA_SD,        // microSD, SDSPI on SPI2
} board_storage_media_t;

esp_err_t board_storage_init(board_storage_media_t media);
esp_err_t board_storage_switch(board_storage_media_t media);
board_storage_media_t board_storage_get_media(void);
bool board_storage_is_mounted(void);

// Serialises the web server, the slideshow and a refresh against each other. Takes
// board_spi_lock() internally, so a card read cannot begin while the panel holds the
// bus.
//
// Neither mutex is recursive. In particular: do NOT call epd_refresh_*() while
// holding this. Read the file, unlock, then refresh -- a refresh takes the bus lock
// itself, and asking for it twice from one task asserts. See board_spi.h.
void board_storage_lock(void);
void board_storage_unlock(void);

// Reclaims the volume from the USB host before an application-side access
// (hal_storage.h:59-69 is the discipline being copied). Ticket 09's USB drive mode is a boot
// of its own in which none of the callers run, so in practice there is never anything to
// reclaim -- but reclaiming would yank the drive from a host, so do not start calling this
// from USB mode.
void board_storage_prepare_access(void);

// USB drive mode only (board_usb_msc.c): unmounts /data from the application and offers the
// medium to the host, and reports whether the host still has it -- an eject from the host puts
// it back to the application, which is how that mode learns it is over.
esp_err_t board_storage_hand_to_usb(void);
bool board_storage_host_owns(void);

// GET /api/storage needs both of these, plus board_storage_get_media().
esp_err_t board_storage_usage(uint64_t *total_bytes, uint64_t *free_bytes);

// The mounted volume's FAT type as FatFs numbers it -- 1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT -- or 0
// when nothing is mounted or it cannot be read. See board_storage.c for why a card cares.
int board_storage_fat_type(void);

// True when the last card tried had no filesystem this build can read -- exFAT, NTFS or blank --
// and the frame fell back to internal flash because of it. Cleared when a card mounts.
bool board_storage_card_unreadable(void);

// Writes `len` bytes to `path` through `<path>.part` and a rename, so `path` is only ever the
// old file, absent, or the new file WHOLE. Ticket 78: four files on this volume are parsed back
// and believed, and a `.smbidx` truncated exactly on a record boundary parses as a smaller
// index with no warning anywhere. Every one of the four goes through here now.
//
// Takes the storage lock, so the caller must not hold it. Returns true only when the rename
// completed; the three ways it can fail are logged distinguishably at the call.
//
// A leftover `.part` is harmless and bounded at one per file: `local_set_build()` unlinks any
// it finds in the mount root, `photo_list_is_image()` keeps the suffix out of the slideshow and
// the album, and the next write truncates it anyway.
#define BOARD_STORAGE_PATH_MAX 224
bool board_storage_write_atomic(const char *path, const void *buf, size_t len);

// --------------------------------------------------------------- card plumbing
//
// board_card_power() and board_card_present() are in board.h -- the rail and the detect
// switch are wired per board. What they promise here is unchanged: the detect is ADVISORY,
// nothing in this module branches on it, because the mount attempt is the authoritative test
// and falling back is correct either way (issues/08).

// --------------------------------------------------- media selection (FR-4.1/4.2)
//
// Ported from local_photo_slideshow.cpp:39-92. The fallback lock is the load-bearing
// part: a card that failed to initialise is not retried until it is physically
// reseated, or the device re-runs the frequency ladder on every loop and the UI
// stalls for three seconds at a time.
typedef struct {
    bool last_card_present;
    bool sd_fallback_locked;
} board_storage_watch_t;

esp_err_t board_storage_select(board_storage_watch_t *w);

// Returns true if the medium changed, in which case the CALLER resets its displayed
// image index (FR-4.2). The reference does that write itself; the index lives in
// RX8130 RAM and belongs to ticket 13, so it is not this module's to touch.
bool board_storage_ensure(board_storage_watch_t *w);

// Clears the fallback lock by hand, for a button or POST /api/storage/rescan -- the
// way out for a user whose detect line does not work.
void board_storage_rescan_request(board_storage_watch_t *w);

// ------------------------------------------------------------ directory (FR-4.6)

// Scans BOARD_STORAGE_MOUNT, non-recursively, filtering and sorting per photo_list.
// Takes the lock and the USB handover itself.
esp_err_t board_storage_scan(photo_list_t *out);

#endif // BOARD_STORAGE_H
