#include "epd_panel.h"

#ifdef BOARD_M5PAPER_COLOR

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include "board.h"     // the panel's pins and clock -- this BOARD's wiring, not the part's
#include "board_spi.h" // the bus, which belongs to neither of its two users
#include "epd_init_list_el040ef1.h"
#include "trace.h"

static const char *TAG = "epd";

// Chunk for the frame transfer. The frame is 120,000 bytes; this splits it into four
// transactions rather than one, which keeps max_transfer_sz modest. Note this is
// already different from M5GFX, which issues 600 separate 200-byte writes -- that
// chunking is an artefact of dithering row by row, not a bus requirement. issues/07
// compares the two directly.
#define EPD_XFER_CHUNK 32768

static spi_device_handle_t s_spi;
// The panel answers on its one data pin, SI0 = MOSI, not on MISO. That needs a
// second, half-duplex 3-wire handle on the same bus; writes stay on s_spi.
// Measured, ticket 18: see the comment on epd_read_temperature().
static spi_device_handle_t s_spi_read;
// The frame lives in PSRAM; s_xfer is the one DMA-capable chunk it is sent through.
// See the allocation in epd_init() for why.
static uint8_t *s_frame;
static uint8_t *s_xfer;

// BUSY since 2026-09-03: the glass was looked at and the arms are indistinguishable
// (1.9 LSB of 255 between settled refreshes, against 4.7 LSB of ordinary first-draw
// settling and a 0.66 LSB instrument floor). issues/09.
static epd_seq_t s_seq = EPD_SEQ_BUSY;

void epd_set_sequencing(epd_seq_t mode)
{
    s_seq = (mode == EPD_SEQ_BUSY) ? EPD_SEQ_BUSY : EPD_SEQ_STOCK;
}

epd_seq_t epd_get_sequencing(void)
{
    return s_seq;
}

// The three stock sleeps. One helper rather than three inline conditionals so that
// "which delays did this build take" is one grep.
static void epd_stock_delay(uint32_t ms)
{
    if (s_seq == EPD_SEQ_STOCK) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

// ------------------------------------------------------------------- BUSY capture

// Recorded in the ISR rather than after the task wakes, so scheduler latency does not
// contaminate the measurement. Polling at 10 ms -- what both reference implementations
// do -- is far too coarse against the 80 ms threshold H1 tests.
static volatile int64_t s_busy_release_us;
static TaskHandle_t s_busy_waiter;

static void IRAM_ATTR busy_isr(void *arg)
{
    (void)arg;
    s_busy_release_us = esp_timer_get_time();
    // Emitted here, not after the task wakes, so an external observer sees the same
    // instant the timestamp records. This is the part of the path most likely to be
    // wrong, so it is the part worth exposing.
    trace_mark();
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

// Arm before issuing the command, so the edge cannot be missed in the gap between
// command and wait.
static void busy_arm(void)
{
    s_busy_waiter = xTaskGetCurrentTaskHandle();
    s_busy_release_us = 0;
    xTaskNotifyStateClear(NULL);
    gpio_intr_enable(BOARD_EPD_PIN_BUSY);
}

// Returns the ISR-captured release timestamp, not one taken after waking.
static esp_err_t busy_wait(uint32_t timeout_ms, int64_t *release_us)
{
    const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    gpio_intr_disable(BOARD_EPD_PIN_BUSY);
    s_busy_waiter = NULL;

    if (got == 0) {
        ESP_LOGE(TAG, "BUSY timeout after %u ms (level=%d)", (unsigned)timeout_ms,
                 gpio_get_level(BOARD_EPD_PIN_BUSY));
        return ESP_ERR_TIMEOUT;
    }
    if (release_us != NULL) {
        *release_us = s_busy_release_us;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------- SPI plumbing

// CS is held low across a whole command-plus-data group, as M5GFX does, so it is
// driven manually rather than by the SPI peripheral.
static inline void epd_cs(bool assert_low)
{
    gpio_set_level(BOARD_EPD_PIN_CS, assert_low ? 0 : 1);
}

// spi_device_acquire_bus() locks the bus to ONE device handle, and every
// transaction inside that window has to be that handle's. The panel is two handles
// -- writes at 4 MHz full duplex, reads half-duplex 3-wire -- so a command sent on
// s_spi cannot be followed by a read on s_spi_read while either holds the bus.
// Hence the device is a parameter here: a temperature read does its writes on the
// read handle too. Mixing them hung the board on the first call, 2026-09-03.
static esp_err_t spi_write_dev(spi_device_handle_t dev, const uint8_t *data,
                               size_t len)
{
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    return spi_device_polling_transmit(dev, &t);
}

static esp_err_t spi_write(const uint8_t *data, size_t len)
{
    return spi_write_dev(s_spi, data, len);
}

static esp_err_t epd_cmd_dev(spi_device_handle_t dev, uint8_t cmd)
{
    gpio_set_level(BOARD_EPD_PIN_DC, 0); // DC low = command
    return spi_write_dev(dev, &cmd, 1);
}

static esp_err_t epd_cmd_with_data_dev(spi_device_handle_t dev, uint8_t cmd,
                                       const uint8_t *data, size_t len)
{
    esp_err_t err = epd_cmd_dev(dev, cmd);
    if (err == ESP_OK && len > 0) {
        gpio_set_level(BOARD_EPD_PIN_DC, 1); // DC high = data
        err = spi_write_dev(dev, data, len);
    }
    return err;
}

static esp_err_t epd_cmd(uint8_t cmd)
{
    return epd_cmd_dev(s_spi, cmd);
}

static esp_err_t epd_data(const uint8_t *data, size_t len)
{
    gpio_set_level(BOARD_EPD_PIN_DC, 1); // DC high = data
    return spi_write(data, len);
}

static esp_err_t epd_cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = epd_cmd(cmd);
    if (err == ESP_OK && len > 0) {
        err = epd_data(data, len);
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

    // The bus belongs to board_spi, not to the panel -- the microSD is the other
    // device on it (issues/03). Idempotent, so it does not matter whether storage
    // brought the bus up first.
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

    // 1 MHz is M5GFX's freq_read: the panel is not obliged to drive its reply at
    // the 4 MHz write clock.
    const spi_device_interface_config_t read_dev = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = -1, // manual, same CS line as the write device
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
    };
    err = spi_bus_add_device(BOARD_SPI_HOST, &read_dev, &s_spi_read);
    if (err != ESP_OK) {
        return err;
    }

    // The frame goes in PSRAM and only one EPD_XFER_CHUNK of it is DMA-capable at a
    // time. This used to be 120,000 bytes of MALLOC_CAP_DMA, on the grounds that
    // routing it through the octal PSRAM would add a variable H2 was not asking
    // about. H2 is settled, and that buffer turned out to be the largest single
    // consumer of DMA-capable internal RAM on the board: the frame application idled
    // with dma_free 3167 and a 2048-byte largest block, and three HTTP responses took
    // it to 579/208 -- below what lwIP needs for an arriving pbuf, so the device
    // stopped answering ICMP and TCP alike while the console looked healthy
    // (issues/21). The transfer was already chunked, so the 120 KB was never needed.
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_frame == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the %d-byte frame", EPD_FRAME_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_xfer = heap_caps_malloc(EPD_XFER_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_xfer == NULL) {
        ESP_LOGE(TAG, "no DMA-capable RAM for the %d-byte transfer chunk",
                 EPD_XFER_CHUNK);
        heap_caps_free(s_frame);
        s_frame = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ------------------------------------------------------------------------- phases

// RST_N low >= 100 us, then ~20 ms before the first command (module manual, §13.1).
// The reference implementations use 2 ms / 200 ms, comfortably inside spec; the
// tighter numbers here are still well clear of the minimum and reset is not on the
// per-refresh critical path either way.
static void epd_reset(void)
{
    gpio_set_level(BOARD_EPD_PIN_RST, 0);
    ets_delay_us(200); // >= 100 us
    gpio_set_level(BOARD_EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20)); // ~20 ms, LUT_EN = 0 case (preflight issues/06)
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
            // The one byte the project is about. Everything else goes out verbatim.
            err = epd_cmd_with_data(cmd, &frs, 1);
        } else {
            err = epd_cmd_with_data(cmd, payload, len);
        }
        if (err != ESP_OK) {
            return err;
        }
        i += 2u + len;
    }

    // TRES is not in the list; M5GFX sends it separately after another BUSY wait.
    return epd_cmd_with_data(EPD_CMD_TRES, epd_tres_payload,
                             sizeof(epd_tres_payload));
}

static esp_err_t epd_send_frame(void)
{
    esp_err_t err = epd_cmd(EPD_CMD_DTM);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(BOARD_EPD_PIN_DC, 1);

    // The bounce through s_xfer is explicit rather than left to the driver. IDF's
    // setup_priv_desc (esp_driver_spi/src/gpspi/spi_master.c:1183-1194) does accept a
    // non-DMA tx_buffer -- by heap_caps_aligned_alloc()ing a transaction-sized DMA
    // buffer and freeing it again, which is a transient 32 KB allocation per chunk out
    // of exactly the pool this arrangement exists to protect. Note that the check it
    // makes, esp_ptr_dma_capable(), is an internal-SRAM address-range test, so a PSRAM
    // pointer always takes that path even though SOC_PSRAM_DMA_CAPABLE is 1 on the S3.
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

// The refresh proper, operating on whatever is already in s_frame.
//
// Both public entry points funnel through here, so the measurement harness and the
// frame application drive the panel through byte-identical code. If they were allowed
// to diverge, Phase 0 would stop measuring what the product actually does -- which is
// the only reason its numbers are worth anything.
static esp_err_t epd_refresh_current(uint8_t frs, epd_timings_t *t)
{
    esp_err_t err;
    int64_t release_us;

    // Mark 1: reset_begin. G5 frames the whole run.
    trace_run_begin();
    trace_mark();
    const int64_t t_start = trace_now_us();

    epd_reset();
    const int64_t t_reset_end = trace_now_us();
    trace_mark(); // 2: reset_end

    epd_cs(true);

    err = epd_send_init_list(frs);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_init_end = trace_now_us();
    trace_mark(); // 3: init_end == xfer_begin

    err = epd_send_frame();
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_xfer_end = trace_now_us();
    trace_mark(); // 4: xfer_end

    // PON. H1 lives here: if BUSY covers the whole >80 ms PON->DRF minimum, the
    // delay(200) below is redundant and issues/09 can drop it.
    busy_arm();
    trace_mark(); // 5: pon_cmd
    const int64_t t_pon = trace_now_us();
    err = epd_cmd(EPD_CMD_PON);
    if (err != ESP_OK) {
        goto fail;
    }
    // Mark 6 (pon_release) is emitted by the ISR.
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, &release_us);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_pon_release = release_us;
    // issues/06 settled H1: BUSY holds a median 130.9 ms against the 80 ms floor, so
    // this sleep is redundant under EPD_SEQ_BUSY. It is still the stock sequence.
    epd_stock_delay(200);

    // BTST2 again, with 0x27 as the last byte rather than the init list's 0x17.
    err = epd_cmd_with_data(EPD_CMD_BTST2, epd_btst2_refresh_payload,
                            sizeof(epd_btst2_refresh_payload));
    if (err != ESP_OK) {
        goto fail;
    }
    epd_stock_delay(200);

    // The panel's timing diagram specifies a >80 ms minimum between PON and DRF. Under
    // EPD_SEQ_STOCK the two sleeps above cover it many times over; under EPD_SEQ_BUSY
    // the BUSY wait is what covers it, and H1 is the evidence that it does. Enforce it
    // here regardless, so the constraint is a property of this function.
    //
    // A BUSY release earlier than the floor would refute H1 on this run. Say so; the
    // ticket asks for it to be logged rather than silently topped up.
    if (t_pon_release - t_pon < (int64_t)EPD_PON_TO_DRF_FLOOR_MS * 1000) {
        ESP_LOGW(TAG, "BUSY released %lld us after PON, below the %d ms floor",
                 (long long)(t_pon_release - t_pon), EPD_PON_TO_DRF_FLOOR_MS);
    }
    const int64_t since_pon_us = trace_now_us() - t_pon;
    if (since_pon_us < (int64_t)EPD_PON_TO_DRF_FLOOR_MS * 1000) {
        const int64_t shortfall_us = (int64_t)EPD_PON_TO_DRF_FLOOR_MS * 1000
                                     - since_pon_us;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((shortfall_us + 999) / 1000)));
    }

    // DRF. t_drf is the waveform time -- the number this whole project is about.
    busy_arm();
    trace_mark(); // 7: drf_cmd
    const int64_t t_drf = trace_now_us();
    const uint8_t zero = 0x00;
    err = epd_cmd_with_data(EPD_CMD_DRF, &zero, 1);
    if (err != ESP_OK) {
        goto fail;
    }
    // Mark 8 (drf_release) is emitted by the ISR.
    err = busy_wait(EPD_DRF_TIMEOUT_MS, &release_us);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_drf_release = release_us;

    busy_arm();
    trace_mark(); // 9: pof_cmd
    const int64_t t_pof = trace_now_us();
    err = epd_cmd_with_data(EPD_CMD_POF, &zero, 1);
    if (err != ESP_OK) {
        goto fail;
    }
    // Mark 10 (pof_release) is emitted by the ISR.
    err = busy_wait(EPD_BUSY_TIMEOUT_MS, &release_us);
    if (err != ESP_OK) {
        goto fail;
    }
    const int64_t t_pof_release = release_us;
    // The one sleep with no BUSY-backed justification either way: POF's own wait
    // returns at ~152 ms and nothing in the timing diagram says what this covers.
    // Dropping it is part of what issues/09 is measuring.
    epd_stock_delay(200);

    epd_cs(false);
    trace_mark(); // 11: run_end
    trace_run_end();

    t->frs = frs;
    t->seq = (uint8_t)s_seq;
    t->pon_to_drf_us = t_drf - t_pon;
    t->reset_us = t_reset_end - t_start;
    t->init_us = t_init_end - t_reset_end;
    t->xfer_us = t_xfer_end - t_init_end;
    t->pon_us = t_pon_release - t_pon;
    t->drf_us = t_drf_release - t_drf;
    t->pof_us = t_pof_release - t_pof;
    t->total_us = trace_now_us() - t_start;
    return ESP_OK;

fail:
    epd_cs(false);
    // Deliberately no trailing trace_mark() here: the observer counts marks per run
    // and a short count is how an aborted refresh announces itself.
    trace_run_end();
    return err;
}

// Bus exclusion for a whole panel operation. Two layers, doing different jobs:
//
//   board_spi_lock()          application-level queueing point. Anything that wants
//                             the bus waits HERE, outside every timestamp.
//   spi_device_acquire_bus()  the correctness guarantee. epd_cs() holds the panel's
//                             CS low across the entire refresh, so the SPI driver
//                             must be told not to interleave the card's transactions
//                             -- and a card read reaches the bus through VFS, FATFS
//                             and sdspi_host without passing through our mutex.
//
// Neither is inside epd_refresh_current(). Lock-wait folded into a timed span would
// reach Phase 0 as panel behaviour, and issues/03 says so explicitly.
static esp_err_t epd_bus_take_dev(spi_device_handle_t dev)
{
    esp_err_t err = board_spi_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    err = spi_device_acquire_bus(dev, portMAX_DELAY);
    if (err != ESP_OK) {
        board_spi_unlock();
    }
    return err;
}

static void epd_bus_give_dev(spi_device_handle_t dev)
{
    spi_device_release_bus(dev);
    board_spi_unlock();
}

static esp_err_t epd_bus_take(void)
{
    return epd_bus_take_dev(s_spi);
}

static void epd_bus_give(void)
{
    epd_bus_give_dev(s_spi);
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

    // Copied rather than sent in place: epd_refresh_current() operates on whatever is
    // in s_frame, so the caller's buffer is not held across the fifteen seconds of a
    // refresh. Both ends are PSRAM now -- the DMA-capable one is s_xfer, a chunk at a
    // time inside epd_send_frame(). This copy sits before epd_bus_take(), so it is
    // outside the t_total window and has never been measured; do not repeat the "about
    // a millisecond" that used to be asserted here. The one copy that IS measured is
    // s_xfer's, and 120 KB across the octal PSRAM turned out to be 4.46 ms rather than
    // the 1 ms predicted for it -- still 0.03 % of a refresh, but not the guess.
    memcpy(s_frame, packed, EPD_FRAME_BYTES);

    esp_err_t err = epd_bus_take();
    if (err != ESP_OK) {
        return err;
    }
    err = epd_refresh_current(frs, t);
    epd_bus_give();
    return err;
}

// Ticket 18. The reply arrives on the panel's one data pin, SI0 = MOSI, so this
// reads on the half-duplex 3-wire handle. Reading MISO -- which is what this did
// until 2026-09-03 -- returns whatever GPIO14 floats to, and the two values it
// floated to, 0x00 and 0xFF, decode to 0 C and -1 C. Both looked like measurements
// and neither was one.
//
// Measured, not inferred: with the same commands on the same bus, TSC read 0x23 and
// FLG read 0x0B on MOSI while both read 0xFF on MISO, across three panel states.
esp_err_t epd_read_temperature(int offset_steps, int *temp_c)
{
    uint8_t tse;
    if (!epd_tse_encode(offset_steps, &tse)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Short, but still a transaction on a bus the microSD also uses, and it holds CS
    // low across three of them. Acquired on the read device, because that is the one
    // whose transactions must not be interleaved with the card's here.
    esp_err_t err = epd_bus_take_dev(s_spi_read);
    if (err != ESP_OK) {
        return err;
    }

    // Twice, and the second one is the answer. TSE starts a conversion and TSC
    // returns the PREVIOUS one with the current offset applied -- so a single read
    // is always one conversion behind, and after an idle gap it reports the panel's
    // temperature from before the gap while looking perfectly current. Measured both
    // ways, ticket 18: with reads flowing the lag is invisible, and across a 60 s gap
    // the first read came back at the pre-gap value while the second was 4 C away.
    // Back-to-back is enough; no delay is needed between them.
    WORD_ALIGNED_ATTR uint8_t raw[4] = {0};
    epd_cs(true);
    for (int attempt = 0; attempt < 2 && err == ESP_OK; attempt++) {
        err = epd_cmd_with_data_dev(s_spi_read, EPD_CMD_TSE, &tse, 1);
        if (err == ESP_OK) {
            err = epd_cmd_dev(s_spi_read, EPD_CMD_TSC);
        }
        if (err == ESP_OK) {
            spi_transaction_t t = {0};
            t.length = 0; // half duplex: the whole transaction is the reply
            t.rxlength = 8;
            t.rx_buffer = raw;
            gpio_set_level(BOARD_EPD_PIN_DC, 1);
            err = spi_device_polling_transmit(s_spi_read, &t);
        }
    }
    epd_cs(false);
    // The panel does not let go of SI0 (= MOSI, the line it answers on) when CS is
    // deasserted, so the microSD sees a contended data line and every sdspi command
    // times out: measured on 2026-09-04, a card that initialised at 20 MHz before this
    // function ran would not initialise at any frequency after it, and neither a
    // full-duplex transaction on the panel's other device nor a card rail cycle brought
    // it back -- a panel reset did. Ticket 08 has the run.
    epd_reset();
    epd_bus_give_dev(s_spi_read);

    if (err == ESP_OK && temp_c != NULL) {
        *temp_c = epd_tsc_decode(raw[0]);
    }
    return err;
}

// ------------------------------------------------------- ticket 18 probe hooks
//
// env:tempprobe only. See the header for what each variant is testing.
#ifdef BUILD_TEMPPROBE

// `dev` decides which pin the reply is expected on: s_spi is full duplex and
// listens to MISO, s_spi_read is half duplex and listens to MOSI.
static esp_err_t probe_read(spi_device_handle_t dev, uint8_t *out, size_t n)
{
    WORD_ALIGNED_ATTR uint8_t rx[4] = {0};
    spi_transaction_t t = {0};
    if (dev == s_spi_read) {
        t.length = 0; // half duplex: nothing to send, rxlength is the whole of it
    } else {
        t.length = n * 8;
    }
    t.rxlength = n * 8;
    t.rx_buffer = rx;
    const esp_err_t err = spi_device_polling_transmit(dev, &t);
    memcpy(out, rx, n);
    return err;
}

// One command-plus-read group, CS held low across it exactly as the refresh path
// does. `tse` is written first unless `write_tse` is false.
static esp_err_t probe_group(spi_device_handle_t dev, uint8_t cmd, bool write_tse,
                             uint8_t tse, uint8_t *out)
{
    epd_cs(true);
    esp_err_t err = ESP_OK;
    if (write_tse) {
        err = epd_cmd_with_data_dev(dev, EPD_CMD_TSE, &tse, 1);
    }
    if (err == ESP_OK) {
        err = epd_cmd_dev(dev, cmd);
    }
    if (err == ESP_OK) {
        gpio_set_level(BOARD_EPD_PIN_DC, 1); // data phase
        err = probe_read(dev, out, 2);
    }
    epd_cs(false);
    return err;
}

esp_err_t epd_probe_reset(void)
{
    esp_err_t err = board_spi_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    epd_reset();
    board_spi_unlock();
    return ESP_OK;
}

esp_err_t epd_probe_configure(uint8_t frs)
{
    esp_err_t err = board_spi_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    epd_reset();
    epd_cs(true);
    err = epd_send_init_list(frs);
    epd_cs(false);
    board_spi_unlock();
    return err;
}

// How long after TSE does TSC carry the new conversion? The thermal answer is
// confounded -- a stale byte and a fresh one look identical on a panel that is not
// changing -- so this steps TSE's own TO[3:0] offset field instead, which moves the
// reading by a known number of degrees on demand.
esp_err_t epd_probe_offset_step(int offset_steps, uint32_t settle_ms, uint8_t *out)
{
    uint8_t tse;
    if (out == NULL || !epd_tse_encode(offset_steps, &tse)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = epd_bus_take_dev(s_spi_read);
    if (err != ESP_OK) {
        return err;
    }

    epd_cs(true);
    err = epd_cmd_with_data_dev(s_spi_read, EPD_CMD_TSE, &tse, 1);
    if (err == ESP_OK && settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
    }
    if (err == ESP_OK) {
        err = epd_cmd_dev(s_spi_read, EPD_CMD_TSC);
    }
    if (err == ESP_OK) {
        gpio_set_level(BOARD_EPD_PIN_DC, 1);
        err = probe_read(s_spi_read, out, 2);
    }
    epd_cs(false);
    epd_bus_give_dev(s_spi_read);
    return err;
}

esp_err_t epd_probe_sample(int offset_steps, epd_probe_sample_t *out)
{
    uint8_t tse;
    if (out == NULL || !epd_tse_encode(offset_steps, &tse)) {
        return ESP_ERR_INVALID_ARG;
    }
    // board_spi_lock() alone, not epd_bus_take(): spi_device_acquire_bus() locks
    // the bus to one device handle, and this deliberately uses two.
    esp_err_t err = board_spi_lock(portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));

    err = probe_group(s_spi, EPD_CMD_TSC, true, tse, out->tsc_miso);
    if (err == ESP_OK) {
        err = probe_group(s_spi_read, EPD_CMD_TSC, true, tse, out->tsc_3wire);
    }
    if (err == ESP_OK) {
        err = probe_group(s_spi_read, EPD_CMD_FLG, false, tse, out->flg_3wire);
    }
    if (err == ESP_OK) {
        err = probe_group(s_spi_read, EPD_CMD_TSC, false, tse, out->tsc_3wire_no_tse);
    }
    if (err == ESP_OK) {
        err = probe_group(s_spi, EPD_CMD_FLG, false, tse, out->flg_miso);
    }

    board_spi_unlock();
    return err;
}

#endif // BUILD_TEMPPROBE

#endif // BOARD_M5PAPER_COLOR
