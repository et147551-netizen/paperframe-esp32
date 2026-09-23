#include "board_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "diskio_sdmmc.h"
#include "diskio_wl.h"
#include "ff.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

#include "board.h"
#include "board_spi.h"

static const char *TAG = "storage";

// The `storage` partition from partitions.csv: 6 MB of FAT over wear levelling at
// 0xA00000.
#define FLASH_PARTITION_LABEL "storage"

// Open files across the whole application. The reference uses 5.
#define STORAGE_MAX_FILES 5

// The frequency ladder from hal_storage.cpp:298-310. Cards that refuse 20 MHz on a
// shared bus are common, and the retry is cheap compared to falling back to a 6 MB
// volume when a 32 GB one was fitted.
static const int SD_FREQ_KHZ[] = {20000, 10000, 4000};
#define SD_FREQ_COUNT (sizeof(SD_FREQ_KHZ) / sizeof(SD_FREQ_KHZ[0]))
#define SD_ATTEMPTS_PER_FREQ 2

static struct {
    board_storage_media_t media;
    bool storage_created;
    tinyusb_msc_storage_handle_t storage_hdl;

    wl_handle_t wl_handle;

    sdmmc_card_t *sd_card;
    sdspi_dev_handle_t sd_spi_handle;
    bool sd_host_init;
    bool sd_spi_attached;
} s_ctx = {
    .media = BOARD_STORAGE_MEDIA_FLASH,
    .wl_handle = WL_INVALID_HANDLE,
    .sd_spi_handle = -1,
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_lock_owner;
// Whether the current holder also took the SPI bus. Decided at lock time from the
// active medium and remembered, so unlock cannot make a different decision if the
// medium changes underneath -- only one task holds the lock, so one flag is enough.
static bool s_lock_took_bus;

// ------------------------------------------------------------------------- lock

static esp_err_t lock_create(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

void board_storage_lock(void)
{
    if (s_lock == NULL) {
        return;
    }
    configASSERT(s_lock_owner != xTaskGetCurrentTaskHandle());
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_lock_owner = xTaskGetCurrentTaskHandle();

    // Taken second, never first: a refresh takes only the bus lock, and that ordering
    // is what keeps the two from deadlocking against each other.
    //
    // Only for the card. The microSD is SDSPI on the panel's own bus, so a card access
    // during a refresh would clock into the panel -- that exclusion is what ticket 03
    // exists for. The internal FAT volume is on the main flash controller and never
    // touches SPI2, so taking the bus for it only means the application waits out a
    // 15.6 s refresh for nothing: measured on 2026-09-02, GET /api/photos/list issued
    // during a refresh timed out at 10 s, which is NFR-5's "the UI must stay responsive
    // across a refresh" failing. The SD branch keeps the old behaviour and is still
    // unverified against a fitted card (ticket 08).
    s_lock_took_bus = (s_ctx.media == BOARD_STORAGE_MEDIA_SD);
    if (s_lock_took_bus) {
        board_spi_lock(portMAX_DELAY);
    }
}

void board_storage_unlock(void)
{
    if (s_lock == NULL) {
        return;
    }
    if (s_lock_took_bus) {
        board_spi_unlock();
        s_lock_took_bus = false;
    }
    s_lock_owner = NULL;
    xSemaphoreGive(s_lock);
}

void board_storage_prepare_access(void)
{
    // Ticket 09 hands the volume to a host only in USB drive mode, which is a boot of its own
    // with none of this function's callers running (board_usb_msc.h). So this still reclaims
    // nothing in practice; it stays because it is the guard should that ever change.
    if (!s_ctx.storage_created) {
        return;
    }
    tinyusb_msc_mount_point_t cur;
    if (tinyusb_msc_get_storage_mount_point(s_ctx.storage_hdl, &cur) == ESP_OK &&
        cur != TINYUSB_MSC_STORAGE_MOUNT_APP) {
        tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl,
                                            TINYUSB_MSC_STORAGE_MOUNT_APP);
    }
}

esp_err_t board_storage_hand_to_usb(void)
{
    if (!s_ctx.storage_created) {
        return ESP_ERR_INVALID_STATE;
    }
    return tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
}

bool board_storage_host_owns(void)
{
    tinyusb_msc_mount_point_t cur;
    return s_ctx.storage_created &&
           tinyusb_msc_get_storage_mount_point(s_ctx.storage_hdl, &cur) == ESP_OK &&
           cur == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

// ---------------------------------------------------------------- flash medium

static esp_err_t flash_medium_open(void)
{
    if (s_ctx.wl_handle != WL_INVALID_HANDLE) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        FLASH_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "no '%s' FAT partition -- check the table actually reached the "
                      "flash, not just sdkconfig", FLASH_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(part, &s_ctx.wl_handle);
}

static void flash_medium_close(void)
{
    if (s_ctx.wl_handle != WL_INVALID_HANDLE) {
        wl_unmount(s_ctx.wl_handle);
        s_ctx.wl_handle = WL_INVALID_HANDLE;
    }
}

// ------------------------------------------------------------------- SD medium

// The card's rail and its detect switch are the BOARD's, not this file's: board_card_power()
// and board_card_present() moved to src/board/<board>/ during the layering pass, which is
// what leaves everything in here generic ESP-IDF -- the mount, the frequency ladder, the
// medium switch and the lock.

static void sd_medium_close(void)
{
    if (s_ctx.sd_spi_attached) {
        sdspi_host_remove_device(s_ctx.sd_spi_handle);
        s_ctx.sd_spi_attached = false;
    }
    if (s_ctx.sd_host_init) {
        sdspi_host_deinit();
        s_ctx.sd_host_init = false;
    }
    if (s_ctx.sd_card != NULL) {
        free(s_ctx.sd_card);
        s_ctx.sd_card = NULL;
    }
    // The rail stays up. Cutting it may also unpower the detect switch, which would
    // make a later removal or insertion invisible. Powering it down is ticket 15's
    // business. That is a GUESS about the schematic; a DMM on TF_3V3_L3B settles it.
}

// **The card's chip select is ours, not sdspi_host's, and it goes low BEFORE the driver clocks
// anything.** sdspi_host_start_command() waits for "not busy" with CS high and then sends the
// command as the first bits after CS falls. A 32 GB card on the M5Paper Color misses the start
// of a command that begins on the first clock after CS falls: it answers CMD0 and then calls
// CMD8, CMD55 and ACMD41 illegal, off-byte, and IDF reads its first byte as a CRC error --
// ESP_ERR_INVALID_CRC at every frequency. One 0xFF clocked with CS low first cures it at once,
// and holding CS across the whole transaction does exactly that, because the driver's busy poll
// then runs with the card selected. A 1 ms delay after CS falls does NOT cure it, so it is clocks
// the card wants, not time. The raw-response probe and its runs: ticket 08, 2026-09-22.
//
// Holding CS across the driver's spi_device_acquire_bus() is only safe while nothing else can
// clock the bus, so this takes board_spi_lock() for any caller that does not hold it already --
// otherwise an access that bypassed board_storage_lock() would have the card selected through a
// panel transfer.
static esp_err_t sd_do_transaction(int slot, sdmmc_command_t *cmd)
{
    const bool take = !board_spi_held_by_me();
    if (take) {
        board_spi_lock(portMAX_DELAY);
    }
    gpio_set_level(BOARD_SPI_PIN_SD_CS, 0);
    const esp_err_t err = sdspi_host_do_transaction(slot, cmd);
    gpio_set_level(BOARD_SPI_PIN_SD_CS, 1);
    if (take) {
        board_spi_unlock();
    }
    return err;
}

static esp_err_t sd_medium_open(void)
{
    esp_err_t err = board_spi_init();
    if (err != ESP_OK) {
        return err;
    }
    err = board_card_power(true);
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.do_transaction = sd_do_transaction;
    slot.host_id = BOARD_SPI_HOST;
    // board_spi_init() parked this pin as an output, high; sd_do_transaction() drives it.
    slot.gpio_cs = SDSPI_SLOT_NO_CS;

    s_ctx.sd_card = calloc(1, sizeof(sdmmc_card_t));
    if (s_ctx.sd_card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = sdspi_host_init();
    if (err != ESP_OK) {
        goto fail;
    }
    s_ctx.sd_host_init = true;

    err = sdspi_host_init_device(&slot, &s_ctx.sd_spi_handle);
    if (err != ESP_OK) {
        goto fail;
    }
    s_ctx.sd_spi_attached = true;
    host.slot = s_ctx.sd_spi_handle;

    for (size_t f = 0; f < SD_FREQ_COUNT; f++) {
        host.max_freq_khz = SD_FREQ_KHZ[f];
        for (int attempt = 0; attempt < SD_ATTEMPTS_PER_FREQ; attempt++) {
            const esp_err_t ierr = sdmmc_card_init(&host, s_ctx.sd_card);
            if (ierr == ESP_OK) {
                ESP_LOGI(TAG, "card init OK @ %d kHz", SD_FREQ_KHZ[f]);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "card init @ %d kHz attempt %d: %s", SD_FREQ_KHZ[f], attempt,
                     esp_err_to_name(ierr));
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    ESP_LOGW(TAG, "card init failed at every frequency");
    err = ESP_ERR_NOT_FOUND;

fail:
    sd_medium_close();
    return err;
}

// --------------------------------------------------------------------- mounting

// Set when the last attempt to use a card found no filesystem this build can read, and cleared by
// the next attempt that does. Read by GET /api/storage so the page can say "reformat" rather than
// only "using internal memory".
static bool s_sd_unreadable;

// FatFs's FS_* type of the volume on `media`'s drive, 0 when it has none. The caller holds the
// storage lock. f_getfree() is what mounts a lazily registered volume, so this is also the check
// that a filesystem is there at all.
static int fat_type_locked(board_storage_media_t media)
{
    const BYTE pdrv = (media == BOARD_STORAGE_MEDIA_SD) ? ff_diskio_get_pdrv_card(s_ctx.sd_card)
                                                         : ff_diskio_get_pdrv_wl(s_ctx.wl_handle);
    if (pdrv == 0xFF) {
        return 0;
    }
    const char drv[3] = {(char)('0' + pdrv), ':', '\0'};
    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    const FRESULT fr = f_getfree(drv, &free_clusters, &fs);
    return (fr == FR_OK && fs != NULL) ? (int)fs->fs_type : 0;
}

static esp_err_t storage_create(board_storage_media_t media)
{
    tinyusb_msc_storage_config_t cfg = {
        .fat_fs = {
            .base_path = BOARD_STORAGE_MOUNT,
            .config = {
                .max_files = STORAGE_MAX_FILES,
                .allocation_unit_size = 0,
            },
            // Formatting is allowed on our own partition and forbidden on a card.
            //
            // Careful with the first half. This was written expecting a virgin
            // partition -- flashing an app at 0x10000 never touches 0xA00000, so
            // nothing this firmware has done could have created a filesystem there.
            // On hardware it mounted with no format at all, because the FACTORY
            // firmware's volume is still present, imaged001.png..imaged004.png and
            // all. Those are the only real photographs on the device; the copies in
            // .scratch/digital-frame/fixtures/ are the backup. So this flag is not
            // "format the empty partition", it is "if the volume is ever lost, we may
            // rebuild it" -- and doing so destroys them.
            //
            // A user's card is different in kind: reformatting someone's photos
            // because the FAT looked odd is not a recoverable mistake.
            .do_not_format = (media == BOARD_STORAGE_MEDIA_SD),
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };

    esp_err_t err;
    if (media == BOARD_STORAGE_MEDIA_FLASH) {
        err = flash_medium_open();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wl_mount: %s", esp_err_to_name(err));
            return err;
        }
        cfg.medium.wl_handle = s_ctx.wl_handle;
        err = tinyusb_msc_new_storage_spiflash(&cfg, &s_ctx.storage_hdl);
        if (err != ESP_OK) {
            flash_medium_close();
            return err;
        }
    } else {
        err = sd_medium_open();
        if (err != ESP_OK) {
            return err;
        }
        cfg.medium.card = s_ctx.sd_card;
        err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_ctx.storage_hdl);
        if (err != ESP_OK) {
            sd_medium_close();
            return err;
        }
        // **tinyusb_msc reports SUCCESS for a card with no filesystem it may read** when
        // do_not_format is set (tinyusb_msc.c msc_storage_mount(): FR_NO_FILESYSTEM ->
        // "Mount failed and do not format is set" -> ESP_OK). So an exFAT, NTFS or blank card
        // used to be "mounted" as SD with nothing behind /data: zero capacity, an empty album,
        // every write failing, and no fallback to flash because nothing had failed. exFAT is
        // simply absent from this FatFs build (ffconf.h FF_FS_EXFAT 0, with no Kconfig for it).
        // Reading the type is what tells the two apart, and failing here hands the card to
        // board_storage_select()/_ensure()'s existing fallback.
        s_sd_unreadable = (fat_type_locked(media) == 0);
        if (s_sd_unreadable) {
            ESP_LOGE(TAG, "SD card has no FAT filesystem this build can read (exFAT/NTFS/blank?); "
                          "format it as FAT32");
            tinyusb_msc_delete_storage(s_ctx.storage_hdl);
            s_ctx.storage_hdl = NULL;
            sd_medium_close();
            return ESP_ERR_NOT_FOUND;
        }
    }

    s_ctx.storage_created = true;
    s_ctx.media = media;
    return ESP_OK;
}

static void storage_destroy(void)
{
    if (s_ctx.storage_created) {
        tinyusb_msc_delete_storage(s_ctx.storage_hdl);
        s_ctx.storage_created = false;
        s_ctx.storage_hdl = NULL;
    }
    if (s_ctx.media == BOARD_STORAGE_MEDIA_FLASH) {
        flash_medium_close();
    } else {
        sd_medium_close();
    }
}

// The public entry points take the lock; the _locked helpers assume it is held, so
// that init-then-switch does not try to take a non-recursive mutex twice.
static esp_err_t storage_switch_locked(board_storage_media_t media)
{
    if (s_ctx.storage_created && s_ctx.media == media) {
        return ESP_OK;
    }
    storage_destroy();
    return storage_create(media);
}

esp_err_t board_storage_init(board_storage_media_t media)
{
    esp_err_t err = lock_create();
    if (err != ESP_OK) {
        return err;
    }

    board_storage_lock();
    // Mounting or unmounting a card drives SPI2 -- the frequency ladder, sdspi attach
    // and detach -- so this path needs the bus even when the lock did not take it for
    // the medium that happens to be active. Either end of the switch being SD is
    // enough: tearing an SD volume down talks to the card too.
    if (!s_lock_took_bus &&
        (media == BOARD_STORAGE_MEDIA_SD || s_ctx.media == BOARD_STORAGE_MEDIA_SD)) {
        board_spi_lock(portMAX_DELAY);
        s_lock_took_bus = true;
    }
    err = storage_switch_locked(media);
    board_storage_unlock();
    return err;
}

esp_err_t board_storage_switch(board_storage_media_t media)
{
    return board_storage_init(media);
}

board_storage_media_t board_storage_get_media(void)
{
    return s_ctx.media;
}

bool board_storage_is_mounted(void)
{
    return s_ctx.storage_created;
}

esp_err_t board_storage_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_ctx.storage_created) {
        return ESP_ERR_INVALID_STATE;
    }
    board_storage_lock();
    board_storage_prepare_access();
    const esp_err_t err =
        esp_vfs_fat_info(BOARD_STORAGE_MOUNT, total_bytes, free_bytes);
    board_storage_unlock();
    return err;
}

// **A FAT16 card works until its ROOT DIRECTORY fills, and that is not its capacity.** FAT16's
// root is a fixed table of 512 entries, and this firmware keeps every photograph in the root under
// a long name, which costs ceil(len/13) + 1 entries each: a mirrored `album_v24_<13 digits>.jpg`
// is 4. Until 2026-09-23 its `.thm` and `.mta` sidecars were there too, 4 more each, which is how
// the root filled at 73; they are in `/data/.thumbs/` now (app_smb_sync.h), so by the same arithmetic ~120, not yet
// run. FatFs
// f_rename() needs fresh entries for the new name, so the failure surfaces as the upload
// handler's `rename fail` (HTTP 500) with gigabytes free. FAT32's root is an ordinary cluster
// chain and has no such limit.
//
// **Windows formats a card of 2 GB or less as FAT (FAT16) by default**, so an ordinary reformat
// produces exactly this. Seen on the E1002's 2 GB card on 2026-09-22: 73 photographs plus
// thumbnails were 506 entries, uploads failed, and deleting ONE unrelated photograph let exactly
// two more through (docs/defect-log.md). Nothing here can lift the limit -- the card is
// never formatted by this firmware (storage_create()) -- so the page warns and the user
// reformats: `format X: /FS:FAT32 /A:4096` gives ~485 k clusters on 2 GB, well above FAT32's
// 65,525-cluster floor. Moving the photographs into a subdirectory would also lift it, and
// touches the listing, both mirrors and the thumbnail paths.
//
// The type is read rather than inferred from the capacity: FatFs decides it by cluster count, and
// a 2 GB card can be either.
int board_storage_fat_type(void)
{
    if (!s_ctx.storage_created) {
        return 0;
    }
    board_storage_lock();
    board_storage_prepare_access();
    const int type = fat_type_locked(s_ctx.media);
    board_storage_unlock();
    return type;
}

bool board_storage_card_unreadable(void)
{
    return s_sd_unreadable;
}

// -------------------------------------------------- media selection (FR-4.1/4.2)

esp_err_t board_storage_select(board_storage_watch_t *w)
{
    if (w == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    w->sd_fallback_locked = false;
    w->last_card_present = board_card_present();

    // Detect is advisory, the attempt is authoritative. With the detect line
    // unverified, gating on it risks either ignoring a fitted card forever or
    // retrying an absent one forever; trying and falling back is correct either way,
    // and costs about three seconds once, at boot.
    esp_err_t err = board_storage_init(BOARD_STORAGE_MEDIA_SD);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    w->sd_fallback_locked = true;
    ESP_LOGW(TAG, "SD unavailable (%s); using internal flash", esp_err_to_name(err));
    return board_storage_init(BOARD_STORAGE_MEDIA_FLASH);
}

bool board_storage_ensure(board_storage_watch_t *w)
{
    if (w == NULL) {
        return false;
    }
    const bool present = board_card_present();

    // A reseat clears the lock; nothing else does. That is the whole point of the
    // flag -- without it the frequency ladder runs on every pass and the UI stalls
    // for three seconds at a time.
    if (!present) {
        w->last_card_present = false;
        w->sd_fallback_locked = false;
    } else if (!w->last_card_present) {
        w->last_card_present = true;
        w->sd_fallback_locked = false;
    }

    const board_storage_media_t target =
        (present && !w->sd_fallback_locked) ? BOARD_STORAGE_MEDIA_SD
                                            : BOARD_STORAGE_MEDIA_FLASH;
    if (board_storage_get_media() == target && board_storage_is_mounted()) {
        return false;
    }

    if (board_storage_switch(target) == ESP_OK) {
        return true;
    }
    if (target == BOARD_STORAGE_MEDIA_FLASH) {
        ESP_LOGE(TAG, "flash switch failed");
        return false;
    }

    w->sd_fallback_locked = true;
    ESP_LOGW(TAG, "SD switch failed; falling back to flash");
    if (board_storage_get_media() != BOARD_STORAGE_MEDIA_FLASH ||
        !board_storage_is_mounted()) {
        if (board_storage_switch(BOARD_STORAGE_MEDIA_FLASH) != ESP_OK) {
            ESP_LOGE(TAG, "flash fallback failed");
            return false;
        }
    }
    return true;
}

void board_storage_rescan_request(board_storage_watch_t *w)
{
    if (w != NULL) {
        w->sd_fallback_locked = false;
        w->last_card_present = false;
    }
}

// ----------------------------------------------------------- atomic record write

// TICKET 78. Four files on this volume are written whole, parsed back and BELIEVED: the
// photograph itself, `.smbdir`, `.smbidx` and `.gpidx`/`.gpdir<n>`. Two of the four already
// wrote through a `.part` and two did not, and the two that did not were the two OWNERSHIP
// records -- the one file whose loss cannot be recovered by re-reading anything.
//
// `fopen(path, "wb")` truncates at open, so writing at the real name leaves a prefix there for
// the whole duration of the write. For `.smbidx` that is ~63 bytes per cached file, and the
// prefix has two ways to be read back: a cut inside a record line fails the parse, which
// manifest_load() logs, and a cut exactly on a record boundary PARSES, silently, as an index of
// a smaller cache. `test/test_smb_manifest`'s truncation sweep measures the split -- 40 of 2,531
// cut points are the silent kind for a 40-record index.
//
// So the real name only ever holds the old file, nothing, or the new file whole. Absent is a
// state every reader here already handles loudly; a parseable prefix is not.
//
// Takes the storage lock for the whole sequence, so a caller must NOT already hold it: the lock
// is not recursive and board_storage_lock() asserts on re-entry by the same task.
bool board_storage_write_atomic(const char *path, const void *buf, size_t len)
{
    if (path == NULL || buf == NULL) {
        return false;
    }
    char part[BOARD_STORAGE_PATH_MAX];
    const int pn = snprintf(part, sizeof(part), "%s.part", path);
    if (pn <= 0 || (size_t)pn >= sizeof(part)) {
        ESP_LOGE(TAG, "%s: no room for a .part name", path);
        return false;
    }

    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(part, "wb");
    size_t wrote = 0;
    if (f != NULL) {
        wrote = fwrite(buf, 1, len, f);
        fclose(f);
    }
    bool renamed = false;
    if (f != NULL && wrote == len) {
        // FATFS f_rename fails when the destination exists, so the old file goes first. The gap
        // where NEITHER name holds the file is the exposure this function still has, and it is
        // two directory operations wide instead of a whole data write.
        unlink(path);
        renamed = (rename(part, path) == 0);
    }
    if (f != NULL && !renamed) {
        unlink(part);
    }
    board_storage_unlock();

    // Three distinguishable failures, because on this bench the console is how they are told
    // apart -- and an out-of-space write fails as a short one, which is the common case.
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s for write failed", part);
    } else if (wrote != len) {
        ESP_LOGE(TAG, "short write %s: %u of %u", part, (unsigned)wrote, (unsigned)len);
    } else if (!renamed) {
        ESP_LOGE(TAG, "rename %s -> %s failed", part, path);
    }
    return renamed;
}

// -------------------------------------------------------------- directory scan

esp_err_t board_storage_scan(photo_list_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.storage_created) {
        return ESP_ERR_INVALID_STATE;
    }

    photo_list_reset(out);

    board_storage_lock();
    board_storage_prepare_access();

    DIR *dir = opendir(BOARD_STORAGE_MOUNT);
    if (dir == NULL) {
        board_storage_unlock();
        return ESP_FAIL;
    }

    const struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_DIR) {
            continue; // /data only, no recursion
        }
        if (!photo_list_add(out, ent->d_name) && out->truncated) {
            break; // 500 reached; the reference stops reading here too
        }
    }
    closedir(dir);
    board_storage_unlock();

    photo_list_sort(out);
    if (out->truncated) {
        ESP_LOGW(TAG, "image list capped at %d", PHOTO_LIST_MAX);
    }
    if (out->arena_full) {
        ESP_LOGW(TAG, "image list truncated: name arena full");
    }
    return ESP_OK;
}
