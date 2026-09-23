// ED2208-GCA driver: the reTerminal E1002's 7.3" Spectra 6 panel.
//
// WRITTEN WITHOUT THE PANEL. The command sequence, its order, the payloads, the frame chunking
// and the SPI clock are all transcribed from
// refs/esp32-photoframe/components/epaper_driver_ed2208_gca/src/driver_ed2208_gca.c (MIT, HEAD
// bf02982). It compiles and it has never driven glass. Ticket 63 is the bring-up.
//
// A COPY OF epd_el040ef1.c RATHER THAN AN #ifdef INSIDE IT, deliberately. The two share their
// plumbing -- manual CS across a command group, the BUSY interrupt with its timestamp taken in
// the ISR, the PSRAM frame bounced through one DMA-capable chunk -- and they differ in the
// refresh sequence's SHAPE, not in a constant:
//
//   EL040EF1:    reset, init, TRES, DTM+data, PON, [200ms], BTST2', [200ms], DRF, POF, [200ms]
//   ED2208-GCA:  reset, init (TRES inside), DTM+data, PON, DRF, POF, DSLP
//
// One file with both would be a chain of conditionals through the middle of the one function
// whose exactness is the whole point, and epd_el040ef1.c's sequence is the reference every
// figure in docs/measurements.md is compared against. The duplication is ~200 lines and
// is the cheaper mistake.
//
// NO TIMING FIGURE APPEARS IN THIS FILE. The timings struct is filled because callers read it,
// but nothing here asserts, expects or thresholds a duration.

#include "epd_panel.h"

#ifdef BOARD_RETERMINAL_E1002

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include "board.h"
#include "board_spi.h"
#include "epd_init_list_ed2208gca.h"

static const char *TAG = "epd";

// The frame is 192,000 bytes; this splits it into six transactions. NOT the reference's 128-byte
// chunks: that firmware copies through a 128-byte STACK buffer to keep PSRAM out of DMA, which
// is the same problem this project solved with one 32 KB DMA-capable heap buffer (ticket 21).
// 32 KB matches BOARD_SPI_MAX_XFER and the EL040EF1 path, so there is one answer here and not
// two. If a run finds the panel unhappy with long CS windows, this is the knob -- and that would
// be a finding, not a tuning.
#define EPD_XFER_CHUNK 32768

static spi_device_handle_t s_spi;
static uint8_t *s_frame;
static uint8_t *s_xfer;

// ------------------------------------------------------------------- BUSY capture

static volatile int64_t s_busy_release_us;
static TaskHandle_t s_busy_waiter;

static void IRAM_ATTR busy_isr(void *arg)
{
    (void)arg;
    s_busy_release_us = esp_timer_get_time();
    BaseType_t higher_woke = pdFALSE;
    if (s_busy_waiter != NULL) {
        vTaskNotifyGiveFromISR(s_busy_waiter, &higher_woke);
    }
    if (higher_woke) {
        portYIELD_FROM_ISR();
    }
}

bool epd_busy_is_idle(void)
{
    return gpio_get_level(BOARD_EPD_PIN_BUSY) != 0; // low = busy, high = idle
}

// Arm before issuing the command, so the edge cannot be missed in the gap between command and
// wait.
static void busy_arm(void)
{
    s_busy_waiter = xTaskGetCurrentTaskHandle();
    s_busy_release_us = 0;
    xTaskNotifyStateClear(NULL);
    gpio_intr_enable(BOARD_EPD_PIN_BUSY);
}

// POLL THE LEVEL, AND USE THE EDGE ONLY FOR THE TIMESTAMP. Measured 2026-09-17, the first run of
// this driver on real glass: the wait after epd_reset() timed out at 30 s with `level=1`, i.e. with
// BUSY already high, which is idle. An edge-only wait cannot come back from that -- there is no
// rising edge to see, because this panel never drove BUSY low for that step at all. Every refresh
// therefore failed before a single byte of frame data was sent, which is why the glass did not
// change.
//
// The reference driver polls (refs/esp32-photoframe/.../driver_ed2208_gca.c:144, `while
// (is_busy())` after a 10 ms settle) and never had the problem. The 10 ms settle is the part that
// looks like sloppiness and is not: BUSY is sampled AFTER the command has gone out, so a poll with
// no settle can read the line before the panel has had time to assert it, and then the driver runs
// ahead of a panel that is about to be busy.
//
// The ISR stays because it timestamps the release to microseconds, and `t_pon2drf_ms` -- ticket
// 63's question about whether this panel needs the EL040EF1's >80 ms floor -- would otherwise be
// quantised to the 10 ms poll.
static esp_err_t busy_wait(uint32_t timeout_ms, int64_t *release_us)
{
    vTaskDelay(pdMS_TO_TICKS(BUSY_SETTLE_MS));

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t out = ESP_OK;
    int64_t release = 0;

    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUSY_POLL_MS)) != 0) {
            release = s_busy_release_us; // the ISR saw the release; microsecond accurate
            break;
        }
        if (epd_busy_is_idle()) {
            // Either it was never busy, or the edge arrived before the wait was armed. Both mean
            // the panel is ready; only the timestamp is coarser.
            release = esp_timer_get_time();
            break;
        }
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "BUSY timeout after %u ms (level=%d)", (unsigned)timeout_ms,
                     gpio_get_level(BOARD_EPD_PIN_BUSY));
            out = ESP_ERR_TIMEOUT;
            break;
        }
    }

    gpio_intr_disable(BOARD_EPD_PIN_BUSY);
    s_busy_waiter = NULL;

    if (out == ESP_OK && release_us != NULL) {
        *release_us = release;
    }
    return out;
}

// ---------------------------------------------------------------------- SPI plumbing

// CS is held low across a whole command-plus-data group, as the reference does, so it is driven
// manually rather than by the SPI peripheral.
static inline void epd_cs(bool assert_low)
{
    gpio_set_level(BOARD_EPD_PIN_CS, assert_low ? 0 : 1);
}

static esp_err_t spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t epd_cmd(uint8_t cmd)
{
    gpio_set_level(BOARD_EPD_PIN_DC, 0); // DC low = command
    return spi_write(&cmd, 1);
}

static esp_err_t epd_cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = epd_cmd(cmd);
    if (err == ESP_OK && len > 0) {
        gpio_set_level(BOARD_EPD_PIN_DC, 1); // DC high = data
        err = spi_write(data, len);
    }
    return err;
}

// ---------------------------------------------------------------------------- init

esp_err_t epd_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_EPD_PIN_DC) | (1ULL << BOARD_EPD_PIN_CS) |
                        (1ULL << BOARD_EPD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK) {
        return err;
    }
    epd_cs(false);
    gpio_set_level(BOARD_EPD_PIN_RST, 1);

    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << BOARD_EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE, // rising = release. low is busy.
    };
    err = gpio_config(&busy);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // already installed is fine
        return err;
    }
    err = gpio_isr_handler_add(BOARD_EPD_PIN_BUSY, busy_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    gpio_intr_disable(BOARD_EPD_PIN_BUSY);

    // The bus belongs to board_spi, not to the panel -- the microSD is the other device on it,
    // on this board as on the M5Paper Color. Idempotent.
    err = board_spi_init();
    if (err != ESP_OK) {
        return err;
    }

    const spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_EPD_SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = -1, // driven manually, see epd_cs()
        .queue_size = 1,
    };
    err = spi_bus_add_device(BOARD_SPI_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    // There is no read handle. The EL040EF1 needs a second, half-duplex 3-wire device because
    // its temperature reply comes back on MOSI (ticket 18); nothing is read from this panel.

    // The frame in PSRAM, one DMA-capable chunk to send it through. See ticket 21 for why the
    // whole frame is not DMA-capable: a 120 KB MALLOC_CAP_DMA buffer was the largest single
    // consumer of internal DMA RAM on the M5Paper Color and took dma_largest below what lwIP
    // needs for an arriving pbuf, which stopped the device answering ICMP and TCP with a
    // perfectly healthy console. This panel's frame is 192,000 bytes, so the same buffer would
    // be 1.6x worse.
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_frame == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the %d-byte frame", EPD_FRAME_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_xfer = heap_caps_malloc(EPD_XFER_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_xfer == NULL) {
        ESP_LOGE(TAG, "no DMA-capable RAM for the %d-byte transfer chunk", EPD_XFER_CHUNK);
        heap_caps_free(s_frame);
        s_frame = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ------------------------------------------------------------------------- phases

// The reference's reset is 50 ms high, 20 ms low, 50 ms high. Reproduced rather than tightened:
// the EL040EF1 path uses 200 us / 20 ms against its module manual's stated minimum, and there is
// no equivalent document for this panel to be confident against.
static void epd_reset(void)
{
    gpio_set_level(BOARD_EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t epd_send_init_list(uint8_t frs)
{
    size_t i = 0;
    while (i < sizeof(epd_init_list) && epd_init_list[i] != EPD_INIT_END) {
        const uint8_t cmd = epd_init_list[i];
        const uint8_t len = epd_init_list[i + 1];
        const uint8_t *payload = &epd_init_list[i + 2];

        esp_err_t err;
        if (cmd == EPD_CMD_PLL) {
            // The waveform rate, overridable by the caller. Everything else goes out verbatim,
            // including TRES, which is part of this panel's list.
            err = epd_cmd_with_data(cmd, &frs, 1);
        } else {
            err = epd_cmd_with_data(cmd, payload, len);
        }
        if (err != ESP_OK) {
            return err;
        }
        i += 2u + len;
    }
    return ESP_OK;
}

static esp_err_t epd_send_frame(void)
{
    esp_err_t err = epd_cmd(EPD_CMD_DTM);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(BOARD_EPD_PIN_DC, 1);

    for (size_t sent = 0; sent < EPD_FRAME_BYTES;) {
        size_t chunk = EPD_FRAME_BYTES - sent;
        if (chunk > EPD_XFER_CHUNK) {
            chunk = EPD_XFER_CHUNK;
        }
        memcpy(s_xfer, s_frame + sent, chunk);
        err = spi_write(s_xfer, chunk);
        if (err != ESP_OK) {
            return err;
        }
        sent += chunk;
    }
    return ESP_OK;
}

// The refresh, operating on whatever is already in s_frame. Both public entry points funnel
// through here so they drive the panel through identical code.
static esp_err_t epd_refresh_current(uint8_t frs, epd_timings_t *t)
{
    esp_err_t err;
    int64_t release_us;
    const int64_t t_start = esp_timer_get_time();

    epd_reset();
    // The reference waits on BUSY after the reset. The panel is left DEEP ASLEEP by the previous
    // refresh's DSLP, so this wait is how the controller says it has come back -- it is not
    // optional here the way it would be on a panel that stays awake.
    busy_arm();
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, NULL);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t t_reset_end = esp_timer_get_time();

    epd_cs(true);

    err = epd_send_init_list(frs);
    if (err != ESP_OK) {
        goto fail;
    }
    busy_arm();
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_init_end = esp_timer_get_time();

    err = epd_send_frame();
    if (err != ESP_OK) {
        goto fail;
    }
    busy_arm();
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_xfer_end = esp_timer_get_time();

    // PON. No fixed delay after it and NO BTST2 RESEND before DRF -- that resend is the
    // EL040EF1's, and this panel's reference does not do it. Adding one "for symmetry" would be
    // sending a booster payload nothing has ever sent to this part.
    busy_arm();
    const int64_t t_pon = esp_timer_get_time();
    err = epd_cmd(EPD_CMD_PON);
    if (err != ESP_OK) {
        goto fail;
    }
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, &release_us);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_pon_release = release_us;

    // The EL040EF1 enforces a >80 ms PON-to-DRF floor from its timing diagram. NO SUCH FLOOR IS
    // KNOWN FOR THIS PANEL: the reference goes straight from the PON BUSY release to DRF, and
    // there is no datasheet here to say whether that is inside spec or merely working. So
    // nothing is enforced and nothing is claimed -- the interval is recorded in
    // t->pon_to_drf_us, which is what a run reads. Ticket 63 should look at it before anyone
    // adds a delay or removes one.
    busy_arm();
    const int64_t t_drf = esp_timer_get_time();
    const uint8_t zero = 0x00;
    err = epd_cmd_with_data(EPD_CMD_DRF, &zero, 1);
    if (err != ESP_OK) {
        goto fail;
    }
    err = busy_wait(EPD_DRF_TIMEOUT_MS, &release_us);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_drf_release = release_us;

    busy_arm();
    const int64_t t_pof = esp_timer_get_time();
    err = epd_cmd_with_data(EPD_CMD_POF, &zero, 1);
    if (err != ESP_OK) {
        goto fail;
    }
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, &release_us);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_pof_release = release_us;

    // DSLP after every refresh, which is the reference's shape and not an idle-power addition of
    // ours. epd_reset() at the top of the next refresh is what brings the controller back; see
    // epd_init_list_ed2208gca.h on why nothing may read a register between refreshes here.
    const uint8_t dslp = EPD_DSLP_PAYLOAD;
    err = epd_cmd_with_data(EPD_CMD_DSLP, &dslp, 1);
    if (err != ESP_OK) {
        goto fail;
    }

    epd_cs(false);

    t->frs = frs;
    t->seq = 0; // one sequence on this panel; see epd_ed2208gca.h
    t->pon_to_drf_us = t_drf - t_pon;
    t->reset_us = t_reset_end - t_start;
    t->init_us = t_init_end - t_reset_end;
    t->xfer_us = t_xfer_end - t_init_end;
    t->pon_us = t_pon_release - t_pon;
    t->drf_us = t_drf_release - t_drf;
    t->pof_us = t_pof_release - t_pof;
    t->total_us = esp_timer_get_time() - t_start;
    return ESP_OK;

fail:
    epd_cs(false);
    return err;
}

// Bus exclusion for a whole panel operation, in two layers doing different jobs -- the
// application-level queueing point and the SPI driver's own guarantee. board_spi.h has the
// account; both apply here because this board also shares SPI2 with the card.
static esp_err_t epd_bus_take(void)
{
    esp_err_t err = board_spi_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        board_spi_unlock();
    }
    return err;
}

static void epd_bus_give(void)
{
    spi_device_release_bus(s_spi);
    board_spi_unlock();
}

esp_err_t epd_refresh_solid(uint8_t color, uint8_t frs, epd_timings_t *t)
{
    if (t == NULL || s_frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (epd_pack_solid(s_frame, EPD_FRAME_BYTES, color) == 0) {
        ESP_LOGE(TAG, "colour index %u is not valid on this panel", (unsigned)color);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = epd_bus_take();
    if (err != ESP_OK) {
        return err;
    }
    err = epd_refresh_current(frs, t);
    epd_bus_give();
    return err;
}

esp_err_t epd_refresh_frame(const uint8_t *packed, uint8_t frs, epd_timings_t *t)
{
    if (t == NULL || s_frame == NULL || packed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // Copied rather than sent in place, so the caller's buffer is not held across the whole
    // refresh. Both ends are PSRAM; the DMA-capable one is s_xfer, a chunk at a time.
    memcpy(s_frame, packed, EPD_FRAME_BYTES);

    esp_err_t err = epd_bus_take();
    if (err != ESP_OK) {
        return err;
    }
    err = epd_refresh_current(frs, t);
    epd_bus_give();
    return err;
}

esp_err_t epd_read_temperature(int offset_steps, int *temp_c)
{
    (void)offset_steps;
    (void)temp_c;
    // NOT SUPPORTED, and that is honest rather than lazy. The EL040EF1's read path was
    // established by ticket 18 across five variants and three panel states, and its answer --
    // the reply arrives on MOSI, not MISO -- is a fact about that board's wiring. Nothing
    // establishes it here, and this panel is deep asleep between refreshes anyway.
    //
    // Callers must tell this apart from a reading of 0 C. That distinction is exactly what
    // ticket 18 cost: a floating line returned 0x00 and 0xFF, which decoded to the entirely
    // plausible 0 C and -1 C, and both looked like measurements. The board's SHT4x
    // (board_sht40_read) is the ambient reading that is available here.
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // BOARD_RETERMINAL_E1002
