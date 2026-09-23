// SPI2 on the reTerminal E1002, shared by the panel (CS 10) and the microSD (CS 14).
//
// READ, NOT MEASURED: the sharing, the pins and the both-CS-high-before-bus-init order all come
// from refs/esp32-photoframe/components/board_hal/src/driver_seeedstudio_reterminal_e1002.c.
//
// The bus is shared on this board exactly as it is on the M5Paper Color, so ticket 43's seam 5
// resolves the same way: the exclusion below is load-bearing and board_storage.c's
// s_lock_took_bus is not dead weight here. Read board_spi.h for the lock order -- both rules
// there apply unchanged, and the second one is still the one that will actually be broken.

#include "board_spi.h"

#ifdef BOARD_RETERMINAL_E1002

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "spi";

static bool s_inited;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_owner;

esp_err_t board_spi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    // Created before the bus comes up, and eagerly rather than on first use: a lazy "create it
    // if it is NULL" is racy if two tasks first-call at once.
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // BOTH chip selects parked high before the bus exists. The reference does the same, and the
    // reason is the one ticket 03 found here: an undriven card CS leaves a fitted card free to
    // read the panel's frame transfer as its own traffic. The panel's own CS is driven by its
    // driver afterwards; parking it costs nothing and covers the window before epd_init().
    const gpio_config_t cs = {
        .pin_bit_mask = (1ULL << BOARD_SPI_PIN_SD_CS) | (1ULL << BOARD_EPD_PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&cs);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(BOARD_SPI_PIN_SD_CS, 1);
    gpio_set_level(BOARD_EPD_PIN_CS, 1);

    const spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SPI_PIN_MOSI,
        .miso_io_num = BOARD_SPI_PIN_MISO,
        .sclk_io_num = BOARD_SPI_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SPI_MAX_XFER,
    };
    err = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        // Someone else initialised it. That is the expected path for whichever of the panel and
        // the card comes second.
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

bool board_spi_is_inited(void)
{
    return s_inited;
}

esp_err_t board_spi_lock(TickType_t wait)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // A task that already holds the bus and asks again would wait for itself forever. Fail
    // loudly rather than hanging with no output.
    configASSERT(s_owner != xTaskGetCurrentTaskHandle());

    if (xSemaphoreTake(s_lock, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_owner = xTaskGetCurrentTaskHandle();
    return ESP_OK;
}

void board_spi_unlock(void)
{
    if (s_lock == NULL) {
        return;
    }
    s_owner = NULL;
    xSemaphoreGive(s_lock);
}

bool board_spi_held_by_me(void)
{
    return s_owner == xTaskGetCurrentTaskHandle();
}

#endif // BOARD_RETERMINAL_E1002
