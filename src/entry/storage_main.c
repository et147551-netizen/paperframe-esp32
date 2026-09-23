// Tickets 03 and 08: /data mounted, scanned, and read from, on the bus the panel
// shares with the microSD.
//
// Two things it proves that nothing before it could:
//
//   * an image that came off a filesystem, not out of flash. Everything up to
//     ticket 07 drew src/app/assets_data.c;
//   * that a card read issued during a refresh waits for the panel instead of being
//     clocked into it. On internal flash the read never touches SPI2, so that arm is a
//     smoke test; with a card fitted it is the real thing, and the unguarded case is
//     the one proof that spi_device_acquire_bus() serialises what our mutex never sees.
//     Both were measured on 2026-09-04 and the case label says which medium was live.
//
// It also probes PM1 GPIO1 and GPIO4. Their card-detect roles were transcribed from the
// UserDemo and are now confirmed both ways -- cardless 2026-09-02, with a card
// 2026-09-04 -- so the four-condition table is a check rather than a question.

#ifdef BUILD_STORAGE

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "assets_data.h"
#include "board.h"
#include "board_spi.h"
#include "board_storage.h"
#include "epd_canvas.h"
#include "epd_dither.h"
#include "epd_panel.h"
#include "epd_format.h"
#include "epd_image.h"
#include "photo_list.h"
#include "trace.h"

static const char *TAG = "storage-main";

#define SETTLE_MS 3000
#define DWELL_MS 6000

#define SEED_PATH BOARD_STORAGE_MOUNT "/test-photo.jpg"
#define SCRATCH_PATH BOARD_STORAGE_MOUNT "/rw-check.txt"

static uint8_t *s_canvas_buf;
static uint8_t *s_packed;
static epd_canvas_t s_canvas;

static char s_name_arena[16384];
static photo_list_t s_list;

// Set by the reader task so the main task can report on it after the refresh.
static volatile int64_t s_read_start_us;
static volatile int64_t s_read_end_us;
static volatile size_t s_read_bytes;
static volatile int s_read_err;
static volatile bool s_read_done;

static void dither_canvas(void)
{
    const size_t row_bytes = EPD_WIDTH / 2;
    for (int32_t y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = epd_canvas_row(&s_canvas, y);
        uint8_t *dst = &s_packed[(size_t)y * row_bytes];
        epd_dither_row_quality(row, dst, EPD_WIDTH, (size_t)y,
                               EPD_DITHER_STRENGTH_QUALITY);
    }
}

// --------------------------------------------------------------------- PM1 probe
//
// THE ONE PLACE ABOVE src/board/ THAT REACHES INTO A BOARD ON PURPOSE, and it is guarded so it
// cannot happen by accident. env:storage is a measurement env for the M5Paper Color: this probe
// exists to settle ticket 08's card-detect polarity by driving three PM1 pins through four
// combinations and reading the raw GPIO_IN byte, which is not something board.h's
// board_card_present() can express and should not be. It is the same arrangement as
// epd_el040ef1.h's BUILD_TEMPPROBE hooks -- a board's or panel's own measurement entry point may
// see its internals; the application may not.
//
// On any other board this compiles to nothing and the probe simply does not run.
#ifdef BOARD_M5PAPER_COLOR
#include "board_pm1.h"

// Four conditions. With no card fitted only one of them could say anything -- rail on
// and detect armed reads GPIO1 high, its pull-up unopposed. With a card fitted
// (2026-09-04) the same two armed conditions read LOW, which settles the transcribed
// "low = present" polarity: 0x17/0x1F cardless against 0x15/0x1D with a card in.
static void probe_pm1_gpio(void)
{
    static const struct {
        bool power;
        bool det_en;
    } CONDITIONS[] = {
        {false, false}, {false, true}, {true, false}, {true, true},
    };

    printf("# pm1 gpio probe\n");
    for (size_t i = 0; i < sizeof(CONDITIONS) / sizeof(CONDITIONS[0]); i++) {
        board_pm1_gpio_output(PM1_GPIO_SD_POWER, CONDITIONS[i].power);
        board_pm1_gpio_output(PM1_GPIO_SD_DET_EN, CONDITIONS[i].det_en);
        board_pm1_gpio_input(PM1_GPIO_SD_DETECT, PM1_PULL_UP);
        vTaskDelay(pdMS_TO_TICKS(10));

        uint8_t bits = 0;
        const esp_err_t err = board_pm1_gpio_read_all(&bits);
        if (err != ESP_OK) {
            printf("# pm1 gpio_in read failed: %s\n", esp_err_to_name(err));
            return;
        }
        printf("# pm1 pwr=%d det_en=%d -> gpio_in=0x%02X (g1=%d)\n",
               (int)CONDITIONS[i].power, (int)CONDITIONS[i].det_en, bits,
               (bits >> PM1_GPIO_SD_DETECT) & 1);
    }
    fflush(stdout);
}

#else // not BOARD_M5PAPER_COLOR

static void probe_pm1_gpio(void)
{
    printf("# pm1 gpio probe: not this board\n");
}

#endif

// ------------------------------------------------------------------ filesystem

static esp_err_t write_file(const char *path, const uint8_t *data, size_t len)
{
    board_storage_lock();
    board_storage_prepare_access();
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        board_storage_unlock();
        return ESP_FAIL;
    }
    const size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    board_storage_unlock();
    return (wrote == len) ? ESP_OK : ESP_FAIL;
}

// Reads a whole file, returning the byte count and a cheap sum so a corrupted read is
// distinguishable from a short one. `lock` is false for the unguarded concurrency
// variant, which deliberately goes round the application mutex.
static size_t read_file(const char *path, bool lock, uint32_t *sum, int *err)
{
    if (lock) {
        board_storage_lock();
        board_storage_prepare_access();
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (lock) {
            board_storage_unlock();
        }
        *err = 1;
        return 0;
    }

    uint8_t buf[512];
    size_t total = 0;
    uint32_t acc = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            acc = acc * 31u + buf[i];
        }
        total += n;
    }
    const int ferr = ferror(f);
    fclose(f);
    if (lock) {
        board_storage_unlock();
    }

    *sum = acc;
    *err = ferr;
    return total;
}

// --------------------------------------------------------- concurrency reader

typedef struct {
    bool lock;
    uint32_t expect_sum;
    size_t expect_bytes;
} reader_args_t;

static void reader_task(void *arg)
{
    const reader_args_t *a = (const reader_args_t *)arg;

    // 1500 ms puts the read squarely inside DRF, which is ~14.4 s of the 15.6 s
    // refresh -- not in the 240 ms transfer, where it would be easy to miss.
    vTaskDelay(pdMS_TO_TICKS(1500));

    uint32_t sum = 0;
    int err = 0;
    s_read_start_us = esp_timer_get_time();
    const size_t got = read_file(SEED_PATH, a->lock, &sum, &err);
    s_read_end_us = esp_timer_get_time();

    s_read_bytes = got;
    s_read_err = err;
    if (got != a->expect_bytes || sum != a->expect_sum) {
        s_read_err = 2; // wrong content, which is the failure that matters most
    }
    s_read_done = true;
    vTaskDelete(NULL);
}

static void concurrency_case(const char *label, bool lock, uint32_t expect_sum,
                             size_t expect_bytes)
{
    static reader_args_t args;
    args.lock = lock;
    args.expect_sum = expect_sum;
    args.expect_bytes = expect_bytes;

    s_read_done = false;
    s_read_err = 0;
    s_read_bytes = 0;

    if (xTaskCreate(reader_task, "reader", 4096, &args, 5, NULL) != pdPASS) {
        printf("# %s: could not start reader task\n", label);
        return;
    }

    epd_timings_t t = {0};
    const int64_t refresh_start = esp_timer_get_time();
    const esp_err_t err = epd_refresh_solid(EPD_COLOR_WHITE, EPD_FRS_STOCK, &t);
    const int64_t refresh_end = esp_timer_get_time();

    while (!s_read_done) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Raw offsets from the start of the refresh rather than a derived verdict: the
    // question is whether the read overlapped the refresh, and four numbers answer it
    // without anyone having to trust an expression they cannot see.
    printf("%s,refresh_err=%d,read_err=%d,read_bytes=%u,"
           "refresh_end_ms=%.1f,read_start_ms=%.1f,read_end_ms=%.1f,"
           "read_wait_ms=%.1f,drf_ms=%.1f\n",
           label, (int)err, s_read_err, (unsigned)s_read_bytes,
           (double)(refresh_end - refresh_start) / 1000.0,
           (double)(s_read_start_us - refresh_start) / 1000.0,
           (double)(s_read_end_us - refresh_start) / 1000.0,
           (double)(s_read_end_us - s_read_start_us) / 1000.0,
           (double)t.drf_us / 1000.0);
    fflush(stdout);
}

// ----------------------------------------------------------------------- main

void app_main(void)
{
    // Printed before anything TinyUSB-linked runs. If the console ever goes quiet
    // after adding esp_tinyusb, the absence of this line is the diagnosis.
    printf("\n# M5Paper Color storage\n");
    printf("# ESP-IDF %s\n", esp_get_idf_version());
    printf("# console alive over USB-Serial-JTAG\n");
    fflush(stdout);

    trace_init();
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_i2c_scan());

    ESP_ERROR_CHECK(board_epd_power(true));
    bool powered = false;
    ESP_ERROR_CHECK(board_epd_power_state(&powered));
    printf("# epd rail readback: %s\n", powered ? "on" : "OFF");

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
    vTaskDelay(pdMS_TO_TICKS(50));

    probe_pm1_gpio();
    printf("# card_present=%d\n", (int)board_card_present());

    // Ticket 03's literal form: the caller brings the bus up, epd_init() only adds
    // its device. board_spi_init() is idempotent, so the three older entry points can
    // stay source-identical and keep the harness baseline comparable.
    ESP_ERROR_CHECK(board_spi_init());
    ESP_ERROR_CHECK(epd_init());

    // Free evidence for ticket 18. GPIO47 was an undriven input in every build before
    // this one; board_spi_init() now parks it high, so if a card that believed itself
    // selected was driving the shared MISO, this number changes.
    int panel_c = -999;
    const esp_err_t terr = epd_read_temperature(0, &panel_c);
    printf("# panel temperature: %d C (err=%s) [issues/18]\n", panel_c,
           esp_err_to_name(terr));

    s_canvas_buf = heap_caps_malloc(epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT),
                                    MALLOC_CAP_SPIRAM);
    s_packed = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (s_canvas_buf == NULL || s_packed == NULL) {
        ESP_LOGE(TAG, "canvas/frame allocation failed -- is PSRAM enabled?");
        return;
    }
    if (!epd_canvas_init(&s_canvas, s_canvas_buf,
                         epd_canvas_bytes(EPD_WIDTH, EPD_HEIGHT), EPD_WIDTH,
                         EPD_HEIGHT)) {
        ESP_LOGE(TAG, "canvas init refused the buffer");
        return;
    }

    // ---- mount. Expect the SD attempt to fail through the frequency ladder and fall
    // back, because no card is fitted.
    board_storage_watch_t watch = {0};
    const int64_t select_start = esp_timer_get_time();
    const esp_err_t serr = board_storage_select(&watch);
    const int64_t select_end = esp_timer_get_time();
    if (serr != ESP_OK) {
        printf("# mount FAILED: %s\n", esp_err_to_name(serr));
        return;
    }
    printf("# media=%s fallback_locked=%d select_ms=%.0f\n",
           board_storage_get_media() == BOARD_STORAGE_MEDIA_SD ? "sd" : "flash",
           (int)watch.sd_fallback_locked,
           (double)(select_end - select_start) / 1000.0);

    uint64_t total = 0, freeb = 0;
    if (board_storage_usage(&total, &freeb) == ESP_OK) {
        printf("# /data total=%llu free=%llu\n", (unsigned long long)total,
               (unsigned long long)freeb);
    }
    fflush(stdout);

    // ---- write, read back, delete.
    static const uint8_t probe[] = "storage round trip";
    if (write_file(SCRATCH_PATH, probe, sizeof(probe)) != ESP_OK) {
        printf("# rw-check: write FAILED\n");
    } else {
        uint32_t sum = 0;
        int rerr = 0;
        const size_t got = read_file(SCRATCH_PATH, true, &sum, &rerr);
        printf("# rw-check: wrote %u read %u err %d\n", (unsigned)sizeof(probe),
               (unsigned)got, rerr);
        board_storage_lock();
        board_storage_prepare_access();
        remove(SCRATCH_PATH);
        board_storage_unlock();
    }

    // ---- seed one real image, so there is something to display and to read during a
    // refresh. With no card and no USB MSC yet, this is the only way files get here.
    if (write_file(SEED_PATH, asset_test_photo_jpg, asset_test_photo_jpg_len) !=
        ESP_OK) {
        printf("# seed: write FAILED\n");
        return;
    }
    printf("# seeded %s (%u bytes)\n", SEED_PATH,
           (unsigned)asset_test_photo_jpg_len);

    // ---- scan.
    photo_list_init(&s_list, s_name_arena, sizeof(s_name_arena));
    const esp_err_t scan_err = board_storage_scan(&s_list);
    printf("# scan err=%s count=%u\n", esp_err_to_name(scan_err),
           (unsigned)photo_list_count(&s_list));
    for (size_t i = 0; i < photo_list_count(&s_list); i++) {
        printf("#   [%u] %s\n", (unsigned)i, photo_list_at(&s_list, i));
    }
    fflush(stdout);

    // ---- the milestone: an image off the filesystem, onto the glass.
    epd_image_reader_storage_t rstore;
    epd_image_reader_t *reader = NULL;
    esp_err_t err = epd_image_open_file(&rstore, SEED_PATH, &reader);
    if (err != ESP_OK) {
        printf("# open %s failed: %s\n", SEED_PATH, esp_err_to_name(err));
        return;
    }

    const int64_t d0 = esp_timer_get_time();
    err = epd_image_draw(reader, &s_canvas);
    const int64_t d1 = esp_timer_get_time();
    epd_image_close(reader);
    if (err != ESP_OK) {
        printf("# decode failed: %s\n", esp_err_to_name(err));
        return;
    }
    dither_canvas();

    epd_timings_t t = {0};
    t.run = 1;
    err = epd_refresh_frame(s_packed, EPD_FRS_STOCK, &t);
    printf("from-file,decode_ms=%.1f,xfer_ms=%.1f,drf_ms=%.1f,total_ms=%.1f,err=%s\n",
           (double)(d1 - d0) / 1000.0, (double)t.xfer_us / 1000.0,
           (double)t.drf_us / 1000.0, (double)t.total_us / 1000.0,
           esp_err_to_name(err));
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    printf("@@CAPTURE from-file\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(DWELL_MS));

    // ---- concurrency. Establish the expected content first, with nothing else
    // running, so a mismatch later means the read was corrupted rather than that the
    // file was never right.
    uint32_t expect_sum = 0;
    int expect_err = 0;
    const size_t expect_bytes = read_file(SEED_PATH, true, &expect_sum, &expect_err);
    printf("# baseline read: %u bytes err=%d\n", (unsigned)expect_bytes, expect_err);

    printf("case,refresh_err,read_err,read_bytes,read_wait_ms,read_after_refresh,drf_ms\n");
    concurrency_case("guarded", true, expect_sum, expect_bytes);
    // Run the guarded case twice: a lock that is taken but never given passes once.
    concurrency_case("guarded-2", true, expect_sum, expect_bytes);
    // Unguarded goes round the mutex, leaving only spi_device_acquire_bus(). On
    // internal flash it never reaches SPI2 and proves nothing about serialisation; on a
    // card it is the one real proof, and the log says which medium was mounted.
    concurrency_case(board_storage_get_media() == BOARD_STORAGE_MEDIA_SD
                         ? "unguarded-sd"
                         : "unguarded-flash-only",
                     false, expect_sum, expect_bytes);

    printf("@@DONE\n");
    printf("# storage complete\n");
    fflush(stdout);
}

#endif // BUILD_STORAGE
