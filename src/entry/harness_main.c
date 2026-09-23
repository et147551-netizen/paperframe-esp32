// Measurement harness entry point.
//
// One CSV line per refresh on the USB serial console, so a sweep is a paste into a
// spreadsheet. Nothing here draws anything interesting on purpose: the instrument
// comes first, and every figure this project currently has is a prediction read off
// a document rather than a measurement.
//
// Report medians over at least ten runs with spread, never a single reading. The
// public FRS table this project is chasing is eight single-sample points from one
// unit, and removing that weakness is the entire reason this harness exists.
//
// It sweeps ARMS -- an FRS setting paired with a host-side sequencing mode. It swept
// FRS for issues/08; it sweeps stock-versus-BUSY sequencing for issues/09. Two things
// about the schedule, and neither changed between them:
//
//   * It is ROUND-ROBIN, not ten runs of one arm then ten of the next. Every refresh
//     warms the panel and refresh time is temperature-dependent, so walking the arms
//     in blocks confounds the variable under test with thermal drift in the same
//     direction as the effect. Round-robin gives every arm the same thermal history,
//     and an arm's trend across passes is then the drift control.
//   * The order WITHIN a pass is permuted from a fixed seed and printed, so position
//     in the pass is not confounded either, and the session is reproducible.

// Only one entry point may be linked; platformio.ini picks it with a build flag.
#ifdef BUILD_HARNESS

#include <stdio.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "epd_panel.h"
#include "epd_format.h"
#include "trace.h"

static const char *TAG = "harness";

// Sweep configuration. Reflashing to change a run count wastes the panel's 24-hour
// refresh budget as much as it wastes time.
//
// The swept variable is FRS again (issues/08), at the sequencing that is now the
// default. issues/08 closed 0x03/0x05/0x07/0x08/0x0F and left seven values open: the
// five below 0x03 that the linear model predicts are SLOWER than stock, and 0x39/0x3A,
// the explicit 100/200 Hz encodings that no measurement on this panel has touched. The
// 0x08 arm is carried as the end-of-block drift control and as the reference the chart
// scans are compared against.
//
// Everything else here -- passes, interval, seed, the permuted order -- is unchanged
// from the issues/09 session on purpose, so the two are comparable.
typedef struct {
    uint8_t frs;
    epd_seq_t seq;
} harness_arm_t;

// The colour arms, in the order their charts are drawn and scanned. Timing is not the
// question here -- these are duplicated so that every setting is compared SETTLED STATE
// TO SETTLED STATE, because the panel's own first draw of new content differs from its
// second by 4.7-6.6 LSB, which is larger than a real effect and looks exactly like one.
// The trailing 0x08 proves reversibility.
static const harness_arm_t HARNESS_ARMS[] = {
    {0x08, EPD_SEQ_BUSY}, // first draw, discarded as the settling excursion
    {0x08, EPD_SEQ_BUSY}, // the reference
    {0x06, EPD_SEQ_BUSY},
    {0x06, EPD_SEQ_BUSY},
    {0x01, EPD_SEQ_BUSY},
    {0x01, EPD_SEQ_BUSY},
    {0x08, EPD_SEQ_BUSY}, // back to stock: does the appearance return?
};
#define HARNESS_ARM_COUNT (sizeof(HARNESS_ARMS) / sizeof(HARNESS_ARMS[0]))

#define HARNESS_PASSES 1
#define HARNESS_COLOR EPD_COLOR_WHITE
#define HARNESS_SEED 0x5A17u

// Record whichever interval is used. The 180 s minimum usually quoted appears in
// neither panel manual -- it is Waveshare's, and unverified. Whether refresh time
// drifts under back-to-back cycling is itself a result worth having.
//
// 40 s, not the 180 s ticket 17's baseline was taken at: 50 refreshes at 180 s do not
// fit an hour. That makes this session's numbers comparable WITHIN itself and not
// directly against that baseline, which is a condition the write-up has to carry.
#define HARNESS_INTERVAL_MS 5000

// A six-colour chart per surviving arm at the end, photographed through the scanner.
// Solid white says nothing about whether the panel still renders colour, and for
// issues/09 this pair of scans is the only image evidence the run produces.
#define CHART_COLS 2
#define CHART_ROWS 5
static const uint8_t CHART[CHART_COLS * CHART_ROWS] = {
    EPD_COLOR_WHITE, EPD_COLOR_BLACK,
    EPD_COLOR_RED,   EPD_COLOR_YELLOW,
    EPD_COLOR_GREEN, EPD_COLOR_BLUE,
    EPD_COLOR_WHITE, EPD_COLOR_BLACK,
    EPD_COLOR_BLACK, EPD_COLOR_WHITE,
};

// Alive until an arm fails. A dropped arm is a result and is printed as one; it
// is not retried nine more times, and it is not silently missing from the CSV either.
static bool s_alive[HARNESS_ARM_COUNT];

static void report_boot(void)
{
    printf("\n# M5Paper Color EPD refresh harness\n");
    printf("# ESP-IDF %s\n", esp_get_idf_version());
    printf("# PSRAM total %u free %u\n",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("# passes=%d colour=%d interval_ms=%d seed=0x%04X\n", HARNESS_PASSES,
           HARNESS_COLOR, HARNESS_INTERVAL_MS, HARNESS_SEED);
    printf("# arms (frs/sequencing):");
    for (size_t i = 0; i < HARNESS_ARM_COUNT; i++) {
        printf(" 0x%02X/%s", HARNESS_ARMS[i].frs,
               epd_seq_name((uint8_t)HARNESS_ARMS[i].seq));
    }
    printf("\n");
    printf("# pon->drf floor %d ms, enforced in both arms\n",
           EPD_PON_TO_DRF_FLOOR_MS);
}

// Fisher-Yates over the index list, from a deterministic LCG. Not a good generator;
// it does not have to be. It has to be the SAME one next session, and printed.
static uint32_t s_rng;

static uint32_t next_random(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 16;
}

static void permute(uint8_t *order, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        order[i] = (uint8_t)i;
    }
    for (size_t i = n; i > 1; i--) {
        const size_t j = next_random() % i;
        const uint8_t tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }
}

void app_main(void)
{
    report_boot();
    trace_init();

    ESP_ERROR_CHECK(board_i2c_init());

    // Scan before trusting any single device. Also resolves the RX8130CE's address,
    // which the datasheet states ambiguously (0x32 as an 8-bit write byte would be
    // 0x19 as a 7-bit address).
    ESP_ERROR_CHECK(board_i2c_scan());

    // The panel is dead until this rail comes up. Confirm the write landed rather
    // than assuming it -- and the first time, check EPD_3V3_L3B with a DMM too.
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

    float sht_temp = 0.0f, sht_rh = 0.0f;
    if (board_sht40_read(&sht_temp, &sht_rh) == ESP_OK) {
        printf("# SHT40 %.2f C %.1f %%RH\n", sht_temp, sht_rh);
    } else {
        printf("# SHT40 did not respond -- shared I2C bus may be unhealthy\n");
    }

    ESP_ERROR_CHECK(epd_init());
    printf("# BUSY at start: %s\n", epd_busy_is_idle() ? "idle (high)" : "busy (low)");

    // Session start, for the drift tripwire below. Not for precision -- the RTC has
    // one-second resolution and is no more accurate than the crystal esp_timer
    // already runs on. It is here to catch a gross failure, e.g. the main crystal
    // not starting and the SoC falling back to an RC oscillator, which would put
    // esp_timer out by percent rather than ppm.
    int64_t rtc_start = 0;
    const bool rtc_ok = (board_rtc_seconds(&rtc_start) == ESP_OK);
    const int64_t timer_start = esp_timer_get_time();
    if (!rtc_ok) {
        printf("# RTC unavailable: no wall clock, and the esp_timer drift check is\n"
               "#   skipped. Fall back to timing the session by hand.\n");
    }

    for (size_t i = 0; i < HARNESS_ARM_COUNT; i++) {
        s_alive[i] = true;
    }
    s_rng = HARNESS_SEED;

    printf("%s\n", EPD_CSV_HEADER);
    fflush(stdout);

    uint32_t run = 0;
    for (int pass = 1; pass <= HARNESS_PASSES; pass++) {
        uint8_t order[HARNESS_ARM_COUNT];
        permute(order, HARNESS_ARM_COUNT);

        printf("# pass %d order:", pass);
        for (size_t i = 0; i < HARNESS_ARM_COUNT; i++) {
            printf(" 0x%02X/%s", HARNESS_ARMS[order[i]].frs,
                   epd_seq_name((uint8_t)HARNESS_ARMS[order[i]].seq));
        }
        printf("\n");

        for (size_t i = 0; i < HARNESS_ARM_COUNT; i++) {
            const uint8_t slot = order[i];
            if (!s_alive[slot]) {
                continue;
            }
            const uint8_t frs = HARNESS_ARMS[slot].frs;
            epd_set_sequencing(HARNESS_ARMS[slot].seq);

            epd_timings_t t = {0};
            t.run = ++run;
            t.frs = frs;

            // Wall clock as a comment, so the fixed CSV schema is preserved -- the
            // column list is pre-registered and analyse.py skips '#' lines. Provenance
            // for a long sweep: correlate with ambient logs, reconstruct run order.
            board_datetime_t now;
            if (board_rtc_read(&now) == ESP_OK) {
                printf("# run %u pass %d frs 0x%02X seq %s at "
                       "%04d-%02d-%02d %02d:%02d:%02d\n",
                       (unsigned)run, pass, frs,
                       epd_seq_name((uint8_t)HARNESS_ARMS[slot].seq), now.year,
                       now.month, now.day, now.hour, now.minute, now.second);
            }

            // Supply per run, as a comment so the pre-registered CSV schema is
            // untouched. A sweep can run for the better part of an hour; if the supply
            // changed partway through, that is a condition the timings were taken under
            // and it belongs in the record.
            board_power_t run_pwr = {0};
            if (board_power_read(&run_pwr) == ESP_OK) {
                printf("# run %u power vin=%d %u mV bat %u mV\n", (unsigned)run,
                       (int)run_pwr.vin_present, (unsigned)run_pwr.vin_mv,
                       (unsigned)run_pwr.vbat_mv);
            }

            // Panel temperature per run, so the sweep can be separated from thermal
            // drift at analysis time rather than assumed away.
            int panel_temp = 0;
            t.temp_c = (epd_read_temperature(0, &panel_temp) == ESP_OK) ? panel_temp
                                                                       : -128;

            const esp_err_t err = epd_refresh_solid(HARNESS_COLOR, frs, &t);
            if (err != ESP_OK) {
                // A BUSY timeout under the tightened sequence is an answer, not an
                // accident. Drop the arm rather than spending the remaining passes on
                // it -- and a dropped arm is printed, not silently missing.
                s_alive[slot] = false;
                printf("# arm 0x%02X/%s dropped at pass %d: %s\n", frs,
                       epd_seq_name((uint8_t)HARNESS_ARMS[slot].seq), pass,
                       esp_err_to_name(err));
                fflush(stdout);
                continue;
            }

            char line[192];
            epd_csv_row(line, sizeof(line), &t);
            printf("%s\n", line);
            fflush(stdout);

            const bool last = (pass == HARNESS_PASSES) && (i + 1 == HARNESS_ARM_COUNT);
            if (!last) {
                vTaskDelay(pdMS_TO_TICKS(HARNESS_INTERVAL_MS));
            }
        }
    }

    // One chart per surviving arm. The timings above are all solid white, which
    // cannot show a waveform that has stopped resolving colour.
    uint8_t *packed = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (packed == NULL) {
        printf("# chart skipped: frame allocation failed -- is PSRAM enabled?\n");
    } else if (epd_pack_chart(packed, EPD_FRAME_BYTES, CHART, CHART_COLS, CHART_ROWS)
               != EPD_FRAME_BYTES) {
        printf("# chart skipped: the packer refused the layout\n");
    } else {
        printf("# chart grid=%dx%d\n", CHART_COLS, CHART_ROWS);
        for (int i = 0; i < CHART_COLS * CHART_ROWS; i++) {
            printf("# layout r%dc%d index=%u\n", i / CHART_COLS, i % CHART_COLS,
                   (unsigned)CHART[i]);
        }
        for (size_t slot = 0; slot < HARNESS_ARM_COUNT; slot++) {
            const uint8_t frs = HARNESS_ARMS[slot].frs;
            const char *seq = epd_seq_name((uint8_t)HARNESS_ARMS[slot].seq);
            if (!s_alive[slot]) {
                printf("# chart 0x%02X/%s skipped: arm was dropped\n", frs, seq);
                continue;
            }
            epd_set_sequencing(HARNESS_ARMS[slot].seq);

            epd_timings_t t = {0};
            t.run = ++run;
            t.frs = frs;
            int panel_temp = 0;
            t.temp_c = (epd_read_temperature(0, &panel_temp) == ESP_OK) ? panel_temp
                                                                       : -128;

            const esp_err_t err = epd_refresh_frame(packed, frs, &t);
            if (err != ESP_OK) {
                printf("# chart 0x%02X/%s FAILED: %s\n", frs, seq,
                       esp_err_to_name(err));
                fflush(stdout);
                continue;
            }
            char line[192];
            epd_csv_row(line, sizeof(line), &t);
            printf("%s\n", line);
            // The capture marker, and then a settle wait: ticket 20 measured the ink
            // still moving for tens of seconds after DRF returns, and the scan takes
            // its own time on top.
            //
            // These two scans are the whole image-quality half of issues/09: a timing
            // win means nothing until the same chart under both sequences has been
            // looked at. If the flatbed is asleep the capture fails per marker while
            // the run carries on, so check WIA sees it before trusting the run for
            // images (docs/agents/hardware-runs.md).
            printf("@@CAPTURE frs-0x%02X-%s\n", frs, seq);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(15000));
        }
        heap_caps_free(packed);
    }

    // Gross-error tripwire. Both clocks are good to tens of ppm, so a healthy pair
    // agrees far inside 1 %; the threshold is deliberately loose because this is
    // looking for a wrong clock source, not for drift.
    int64_t rtc_end = 0;
    if (rtc_ok && board_rtc_seconds(&rtc_end) == ESP_OK) {
        int64_t rtc_elapsed = rtc_end - rtc_start;
        if (rtc_elapsed < 0) {
            rtc_elapsed += 24 * 3600; // crossed midnight
        }
        const double timer_elapsed =
            (double)(esp_timer_get_time() - timer_start) / 1e6;
        if (rtc_elapsed >= 60) { // below a minute the 1 s resolution dominates
            const double err = (timer_elapsed - (double)rtc_elapsed) /
                               (double)rtc_elapsed;
            printf("# esp_timer %.1f s vs RTC %lld s -> %+.3f %%\n", timer_elapsed,
                   (long long)rtc_elapsed, 100.0 * err);
            if (err > 0.01 || err < -0.01) {
                printf("# WARNING: >1%% disagreement. esp_timer is probably NOT on\n"
                       "#   the crystal. Every interval in this session is suspect.\n");
            }
        } else {
            printf("# session too short (%lld s) for a meaningful drift check\n",
                   (long long)rtc_elapsed);
        }
    }

    printf("# done\n");
}

#endif // BUILD_HARNESS
