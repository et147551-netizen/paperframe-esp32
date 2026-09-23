// SPI2_HOST, shared by the EPD panel (CS 44) and the microSD (CS 47).
//
// The bus used to be initialised inside epd_init(), which was correct while the
// measurement harness was the only client and the card was deliberately never
// mounted. The frame reads its images off that card, so the bus needs an owner that
// is neither of its two users. See .scratch/digital-frame/issues/03.
//
// Both users call board_spi_init(); it is idempotent, so neither has to know whether
// the other went first.

#ifndef BOARD_SPI_H
#define BOARD_SPI_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/spi_common.h"

// BOARD_SPI_HOST, the three bus pins, BOARD_SPI_PIN_SD_CS and BOARD_SPI_MAX_XFER are the
// board's wiring and live in its pin header. Both boards this project supports happen to
// share the panel and the card on SPI2 -- read
// refs/esp32-photoframe/components/board_hal/include/board_seeedstudio_reterminal_e1002.h,
// which drives both CS pins high before spi_bus_initialize() exactly as this does -- so the
// exclusion below is load-bearing on both and must not be simplified away on either.
#include "board.h"

// One spi_bus_initialize() for SPI2_HOST, plus the SD CS park. Safe to call more than
// once and from either client; ESP_ERR_INVALID_STATE from an already-initialised bus
// is folded to ESP_OK, as refs/M5PaperColor-UserDemo/main/hal/storage/hal_storage.cpp
// does in bus_spi_init_once().
esp_err_t board_spi_init(void);

bool board_spi_is_inited(void);

// Bus exclusion, held across a whole panel refresh -- 15.6 s of it (NFR-5).
//
// This is the coarse, application-level half of the exclusion. The other half is
// spi_device_acquire_bus() inside the driver, and the two are not redundant: a read
// from a file on the card reaches the bus through VFS -> FATFS -> sdspi_host without
// passing through any mutex this project owns, so only the SPI driver's own bus lock
// can stop it being clocked into a panel that is holding CS low. The mutex is what
// gives an application-level access a named place to queue instead of stalling deep
// inside FATFS while holding its file lock.
//
// LOCK ORDER. board_storage_lock() takes this one internally, so a filesystem access
// holds both and a refresh holds only this. Two rules follow, and the second is the
// one that will actually be broken:
//
//   1. Never take board_spi_lock() and then board_storage_lock().
//   2. Never call epd_refresh_*() while holding board_storage_lock(). Decode the
//      image, release the storage lock, then refresh. Holding it across the refresh
//      means asking for this mutex twice from one task.
//
// Both mutexes are non-reentrant and assert on self-deadlock rather than blocking. A
// silent hang on this board is indistinguishable from a device that was never powered
// on, which has already cost this project an hour once (issues/17).
esp_err_t board_spi_lock(TickType_t wait);
void board_spi_unlock(void);

// Whether the calling task holds board_spi_lock(). For board_storage.c's card transactions,
// which are reached both from under board_storage_lock() and from outside it.
bool board_spi_held_by_me(void);

#endif // BOARD_SPI_H
