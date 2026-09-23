// Hardware bring-up: prove the board end to end, and settle four open questions.
//
// No firmware from this project had ever run on this hardware before this file. Every
// number in docs/research/ is a prediction and every pin in docs/board-pinmap.md is a
// transcription from someone else's source. This is where that stops being true.
//
// Four things it answers that no amount of reading could:
//
//   1. Does the driver work at all -- PM1 rail, reset, init list, TRES, PON/DRF/POF.
//   2. Which palette index is which colour. The handoff document says 4=blue, 5=green;
//      the research document says that is wrong and it is 5=blue, 6=green. Both are
//      readings of documents. Six photographs decide it.
//   3. The RX8130's 7-bit address, 0x32 or 0x19 -- the datasheet is ambiguous.
//   4. Which I2C devices are actually on the bus.
//
// Prints "@@CAPTURE <label>" once a frame is on the glass and has settled, so
// tools/bringup_capture.py can photograph each colour without anyone timing it by hand.

// Only one entry point may be linked; platformio.ini picks it with a build flag.
#ifdef BUILD_BRINGUP

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "epd_panel.h"
#include "epd_format.h"
#include "trace.h"

static const char *TAG = "bringup";

// Long enough for the ink to stop moving and for the host to take a photograph. The
// panel is stable well before this; the margin is for the camera, not the physics.
#define SETTLE_MS 3000
#ifdef BOARD_RETERMINAL_E1002
// The E1002's panel is photographed with the flatbed rather than the webcam, and a scan of a
// 7.3" panel takes longer than the 8 s this used to allow -- the following refresh then lands in
// the scan's lower rows and reads exactly like a colour effect. docs/agents/hardware-runs.md has
// that account, from env:photo making the same mistake at 300 dpi.
#define DWELL_MS 20000
#else
#define DWELL_MS 8000
#endif

// Index 4 is orange: valid on the 7.3" part, not on this one. It is deliberately
// absent -- sending it is a defect, not an experiment (NFR-4).
static const struct {
    uint8_t index;
    const char *name;
} PALETTE[] = {
    {EPD_COLOR_WHITE, "white"},   {EPD_COLOR_BLACK, "black"},
    {EPD_COLOR_RED, "red"},       {EPD_COLOR_YELLOW, "yellow"},
    {EPD_COLOR_GREEN, "green"},   {EPD_COLOR_BLUE, "blue"},
};
#define PALETTE_COUNT (sizeof(PALETTE) / sizeof(PALETTE[0]))

static void report_boot(void)
{
    printf("\n# %s bring-up\n", BOARD_NAME);
    printf("# ESP-IDF %s\n", esp_get_idf_version());
    printf("# PSRAM total %u free %u\n",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    report_boot();
    trace_init();

    ESP_ERROR_CHECK(board_i2c_init());

    // Everything downstream assumes this bus is healthy. Probe it rather than inferring
    // health from one device happening to answer -- and this is what resolves the
    // RX8130 address ambiguity.
    ESP_ERROR_CHECK(board_i2c_scan());

    // The panel is dead until this rail is up.
    ESP_ERROR_CHECK(board_epd_power(true));
    bool powered = false;
    ESP_ERROR_CHECK(board_epd_power_state(&powered));
    printf("# PM1 EPD power readback: %s\n", powered ? "on" : "OFF");
    if (!powered) {
        ESP_LOGE(TAG, "EPD rail did not come up; stopping");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    board_power_t pwr = {0};
    if (board_power_read(&pwr) == ESP_OK) {
        printf("# power: vin=%d (%u mV) vinout=%d bat=%d (%u mV)\n",
               (int)pwr.vin_present, (unsigned)pwr.vin_mv, (int)pwr.vinout_present,
               (int)pwr.bat_present, (unsigned)pwr.vbat_mv);
        if (!pwr.vin_present) {
            printf("# WARNING: running on battery. The PM1 forcibly powers off below\n"
                   "#   BATT_LVP (2.50 V default) and nothing but the power button\n"
                   "#   brings this board back. Do not start a long unattended run.\n");
        }
    } else {
        printf("# power: PM1 did not answer\n");
    }

#ifdef BOARD_RETERMINAL_E1002
    // TICKET 63 ARM 5, AND IT CONTRADICTS WHAT THAT ARM WAS WRITTEN TO ASSUME. The reference
    // firmware's board header names no card-detect line, so board_card_present() answers "a card
    // may be fitted, I cannot tell". The V1.2 schematic (2026-09-17, .scratch/reterminal-e1002/)
    // names ESP_IO15/SD_DET, so the line exists on the PCB and only the third party's header omits
    // it. This prints the level with a card KNOWN to be fitted; the other half of the measurement
    // needs a hand at the slot, so nothing is wired to this pin until both readings exist.
    {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << 15,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cfg) == ESP_OK) {
            printf("# SD_DET (GPIO15, pull-up, card fitted): %d\n", gpio_get_level(15));
        }
    }
#endif

    float sht_temp = 0.0f, sht_rh = 0.0f;
    if (board_sht40_read(&sht_temp, &sht_rh) == ESP_OK) {
        printf("# SHT40 %.2f C %.1f %%RH\n", sht_temp, sht_rh);
    } else {
        printf("# SHT40 did not respond -- shared I2C bus may be unhealthy\n");
    }

    board_datetime_t now;
    if (board_rtc_read(&now) == ESP_OK) {
        printf("# RTC %04d-%02d-%02d %02d:%02d:%02d\n", now.year, now.month, now.day,
               now.hour, now.minute, now.second);
    } else {
        printf("# RTC did not read back a plausible time\n");
    }

    ESP_ERROR_CHECK(epd_init());
    printf("# BUSY at start: %s\n", epd_busy_is_idle() ? "idle (high)" : "busy (low)");

    int panel_temp = 0;
    if (epd_read_temperature(0, &panel_temp) == ESP_OK) {
        printf("# panel temperature %d C\n", panel_temp);
    } else {
        printf("# panel temperature unreadable (TSC did not respond)\n");
    }

    printf("%s\n", EPD_CSV_HEADER);

    for (unsigned i = 0; i < PALETTE_COUNT; i++) {
        const uint8_t index = PALETTE[i].index;
        const char *name = PALETTE[i].name;

        printf("# refreshing solid %s (index %u)\n", name, (unsigned)index);
        fflush(stdout);

        epd_timings_t t = {0};
        t.run = i + 1;
        t.temp_c = (epd_read_temperature(0, &panel_temp) == ESP_OK) ? panel_temp : -128;

        const esp_err_t err = epd_refresh_solid(index, EPD_FRS_STOCK, &t);
        if (err != ESP_OK) {
            printf("# %s FAILED: %s\n", name, esp_err_to_name(err));
            fflush(stdout);
            continue;
        }

        char line[192];
        epd_csv_row(line, sizeof(line), &t);
        printf("%s\n", line);

        vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

        // The host watches for this and takes the photograph. The label carries the
        // index so a mismatch between what we asked for and what appeared is visible
        // in the filename alone.
        printf("@@CAPTURE colour-%u-%s\n", (unsigned)index, name);
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(DWELL_MS));
    }

    printf("@@DONE\n");
    printf("# bring-up complete\n");
    fflush(stdout);
}

#endif // BUILD_BRINGUP
