#include "board_spi.h"

#ifdef BOARD_M5PAPER_COLOR

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

    // Created before the bus comes up, and eagerly rather than on first use. A lazy
    // "create it if it is NULL" is racy if two tasks first-call at once; the
    // UserDemo gets away with that only because its init is single-threaded.
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Park the card's chip select high before the bus exists, so it is deselected for
    // every transaction the panel makes, including the ones during epd_init().
    const gpio_config_t sd_cs = {
        .pin_bit_mask = 1ULL << BOARD_SPI_PIN_SD_CS,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&sd_cs);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(BOARD_SPI_PIN_SD_CS, 1);

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
        // Someone else initialised it. That is the expected path for whichever of the
        // panel and the card comes second.
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
    // A task that already holds the bus and asks again would wait for itself forever.
    // Fail loudly here rather than hanging with no output, which on this board looks
    // exactly like a device that was never powered on.
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

#endif // BOARD_M5PAPER_COLOR
