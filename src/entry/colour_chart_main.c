// Ticket 20: a colour chart held on the glass for hours while the camera watches it.
//
// The question is not "what colour is this patch" -- a webcam cannot answer that, and
// tools/capture_panel.py says so in its own docstring. It is whether the appearance is
// STABLE: how long after a refresh the ink stops moving, and how much the same chart
// varies from one refresh to the next. Ticket 19 wants to calibrate the quantiser
// against measured appearance, and a palette calibrated against a moving target is
// calibrated against nothing.
//
// Three things this file does deliberately:
//
//   1. The chart is packed straight to 4bpp indices. No canvas, no dither, no decode --
//      a dithered chart would measure the dither instead of the ink.
//   2. Two variants, A and B, with the six subject patches permuted. A per-cell effect
//      (the room lighting one corner of the panel more) stays in the cell across the
//      two; a per-colour effect follows the colour. Nothing else separates them.
//   3. Panel temperature is NOT printed. This predates ticket 18's answer, when
//      epd_read_temperature() was reading a floating MISO line -- 0 C, then -1 C with
//      the room unchanged -- and the run would not write a fabricated temperature into
//      a second CSV. The read is fixed now (it was on the wrong pin); adding the column
//      here is a deliberate change to make against a fresh run, not a comment edit. The
//      SHT40 numbers below are AIR temperature, six centimetres away, and stay labelled
//      as such either way: the panel self-heats and the air does not.
//
// The run is long and unattended, so it refuses to start on battery. bringup_main.c only
// warns; here, six hours on a cell that the PM1 cuts at 2.50 V ends with a board that no
// software anywhere can restart (docs/agents/hardware-runs.md).

// Only one entry point may be linked; platformio.ini picks it with a build flag.
#ifdef BUILD_COLOURCHART

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

static const char *TAG = "chart";

#define CHART_COLS 2
#define CHART_ROWS 5

// Row 0 and row 4 are the in-frame reference pair. Normalising every patch against the
// white and black measured in the SAME photograph is what lets this run survive a room
// whose light changes over six hours; having the pair at both ends bounds how much the
// lighting varies across the panel itself.
//
// Row 4 is the MIRROR of row 0, and that matters. With white in column 0 at both ends
// and black in column 1 at both ends -- which is how the first run was laid out -- any
// brightness difference between the two columns is absorbed into the white-minus-black
// span and normalised away as if it were part of the scale. Mirroring makes each
// reference average over both columns, so a column gradient shows up in the subjects
// instead of hiding in the reference.
static const uint8_t CHART_A[CHART_COLS * CHART_ROWS] = {
    EPD_COLOR_WHITE, EPD_COLOR_BLACK,
    EPD_COLOR_RED,   EPD_COLOR_YELLOW,
    EPD_COLOR_GREEN, EPD_COLOR_BLUE,
    EPD_COLOR_WHITE, EPD_COLOR_BLACK,
    EPD_COLOR_BLACK, EPD_COLOR_WHITE,
};

// The same six subjects, in reversed order, so each one sits in a different cell and
// beside different neighbours.
static const uint8_t CHART_B[CHART_COLS * CHART_ROWS] = {
    EPD_COLOR_WHITE,  EPD_COLOR_BLACK,
    EPD_COLOR_BLACK,  EPD_COLOR_WHITE,
    EPD_COLOR_BLUE,   EPD_COLOR_GREEN,
    EPD_COLOR_YELLOW, EPD_COLOR_RED,
    EPD_COLOR_BLACK,  EPD_COLOR_WHITE,
};

#ifdef COLOURCHART_SMOKE
// Two minutes, one refresh: proves the firmware, the markers, the camera and the
// extractor before anything is asked to run for six hours.
#define CYCLE_COUNT 1
static const int SETTLE_SCHEDULE_S[] = {0, 15, 30};
static const int TAIL_SCHEDULE_S[] = {45};
#else
#define CYCLE_COUNT 6
// Seconds since the end of DRF. Dense early because that is where settling would be.
//
// t1800 was dropped after run 1: colour did not depend on time since the refresh
// anywhere in 0-900 s, and carrying every cycle out to half an hour cost 15 minutes a
// cycle to add a point that said the same thing. Six cycles now take about 1.7 h
// instead of 6.0 h.
static const int SETTLE_SCHEDULE_S[] = {0, 15, 30, 60, 120, 300, 900};

// Three and a half hours on one unchanged image, appended to the final cycle. OFF by
// default, and run 1 is why: over that hold everything moved by 1-6 raw 8-bit units,
// which is the size of this camera's quantisation and of its response to the chart
// layout changing, and the second half of the hold added wobble rather than resolution.
// The question -- whether the ink keeps moving for hours -- is real and unanswered; it
// belongs to an instrument that can resolve it, not to another 3.5 h of this one.
#ifdef COLOURCHART_TAIL
static const int TAIL_SCHEDULE_S[] = {3600, 5400, 7200, 9000, 10800, 12600};
#else
#define TAIL_DISABLED 1
#endif
#endif

#define SETTLE_COUNT ((int)(sizeof(SETTLE_SCHEDULE_S) / sizeof(SETTLE_SCHEDULE_S[0])))
#define TAIL_COUNT ((int)(sizeof(TAIL_SCHEDULE_S) / sizeof(TAIL_SCHEDULE_S[0])))

// A marker is fire-and-forget: the firmware prints it and carries on while the host
// spends a few seconds per photograph. Every gap in the schedule above is wide enough to
// absorb that except the last one, which is followed immediately by the next refresh --
// and the first run put four photographs of a panel mid-waveform into the data, which
// normalise to nonsense because the white and black references are momentarily the same
// colour. Three shots take about 8 s; this is that with margin.
#define CAPTURE_DRAIN_MS 15000

static uint8_t *s_packed;

// Everything the analysis needs to explain a reading, printed immediately before the
// marker so it sits next to the photograph in the log. Raw numbers, no verdicts: a
// derived boolean in first-cut instrumentation has already cost this project a re-run
// (issues/03), and here a re-run is six hours.
static void print_conditions(int cycle, char variant, int requested_s, int64_t elapsed_us)
{
    float air_c = 0.0f, rh = 0.0f;
    const bool sht_ok = (board_sht40_read(&air_c, &rh) == ESP_OK);

    board_power_t pwr = {0};
    const bool pwr_ok = (board_power_read(&pwr) == ESP_OK);

    printf("# sample cycle=%d variant=%c t_req_s=%d t_actual_ms=%lld uptime_ms=%lld",
           cycle, variant, requested_s, (long long)(elapsed_us / 1000),
           (long long)(esp_timer_get_time() / 1000));
    if (sht_ok) {
        printf(" air_c=%.2f rh=%.1f", (double)air_c, (double)rh);
    } else {
        printf(" air_c=NA rh=NA");
    }
    if (pwr_ok) {
        printf(" vin=%d vbat_mv=%u", (int)pwr.vin_present, (unsigned)pwr.vbat_mv);
    } else {
        printf(" vin=NA vbat_mv=NA");
    }
    printf("\n");
}

// Waits until `target_s` after t0 and then asks the host for a photograph. Sleeping to
// an absolute deadline rather than between markers keeps the camera's own few seconds
// from accumulating across a schedule that runs for hours.
static void capture_at(int cycle, char variant, int64_t t0_us, int target_s)
{
    const int64_t deadline_us = t0_us + (int64_t)target_s * 1000000;
    const int64_t now_us = esp_timer_get_time();
    if (deadline_us > now_us) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((deadline_us - now_us) / 1000)));
    }

    const int64_t elapsed_us = esp_timer_get_time() - t0_us;
    print_conditions(cycle, variant, target_s, elapsed_us);
    printf("@@CAPTURE chart-%c-c%d-t%d\n", variant, cycle, target_s);
    fflush(stdout);
}

void app_main(void)
{
    printf("\n# M5Paper Color colour chart -- issues/20\n");
    printf("# ESP-IDF %s\n", esp_get_idf_version());
    printf("# PSRAM total %u free %u\n",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    trace_init();
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_epd_power(true));

    board_power_t pwr = {0};
    const esp_err_t pwr_err = board_power_read(&pwr);
    if (pwr_err == ESP_OK) {
        printf("# power: vin=%d (%u mV) vinout=%d bat=%d (%u mV)\n",
               (int)pwr.vin_present, (unsigned)pwr.vin_mv, (int)pwr.vinout_present,
               (int)pwr.bat_present, (unsigned)pwr.vbat_mv);
    } else {
        printf("# power: PM1 did not answer (%s)\n", esp_err_to_name(pwr_err));
    }

    // A stop, not a warning. Below BATT_LVP the PM1 powers the board off and only the
    // power button brings it back -- and the point of this run is that nobody is there.
    if (pwr_err != ESP_OK || !pwr.vin_present) {
        printf("# REFUSING TO RUN: this is a multi-hour unattended run and USB power is\n"
               "#   not confirmed. The PM1 cuts the board off below BATT_LVP and nothing\n"
               "#   but the power button restarts it. See docs/agents/hardware-runs.md.\n");
        printf("@@DONE\n");
        fflush(stdout);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(epd_init());

    s_packed = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (s_packed == NULL) {
        ESP_LOGE(TAG, "frame allocation failed -- is PSRAM enabled?");
        return;
    }

    // The layout, machine-readable, so the extractor names patches by cell and reads
    // what was actually sent to each one out of this log rather than a second copy of
    // the table that can drift from this one.
    printf("# chart grid=%dx%d\n", CHART_COLS, CHART_ROWS);
    for (int i = 0; i < CHART_COLS * CHART_ROWS; i++) {
        printf("# layout A r%dc%d index=%u\n", i / CHART_COLS, i % CHART_COLS,
               (unsigned)CHART_A[i]);
    }
    for (int i = 0; i < CHART_COLS * CHART_ROWS; i++) {
        printf("# layout B r%dc%d index=%u\n", i / CHART_COLS, i % CHART_COLS,
               (unsigned)CHART_B[i]);
    }
    printf("%s\n", EPD_CSV_HEADER);
    fflush(stdout);

    for (int cycle = 1; cycle <= CYCLE_COUNT; cycle++) {
        const bool is_a = (cycle % 2) == 1;
        const char variant = is_a ? 'A' : 'B';
        const uint8_t *patches = is_a ? CHART_A : CHART_B;

        if (epd_pack_chart(s_packed, EPD_FRAME_BYTES, patches, CHART_COLS, CHART_ROWS)
            != EPD_FRAME_BYTES) {
            ESP_LOGE(TAG, "chart %c was refused by the packer", variant);
            break;
        }

        epd_timings_t t = {0};
        t.run = (uint32_t)cycle;
        // temp_c stays at the sentinel: see the header comment. The panel's own sensor
        // is not readable on this board yet (issues/18) and the CSV must not carry a
        // number the panel never supplied.
        t.temp_c = -128;

        const esp_err_t err = epd_refresh_frame(s_packed, EPD_FRS_STOCK, &t);
        const int64_t t0_us = esp_timer_get_time();
        if (err != ESP_OK) {
            printf("# cycle %d (%c) refresh FAILED: %s\n", cycle, variant,
                   esp_err_to_name(err));
            fflush(stdout);
            continue;
        }

        char line[192];
        epd_csv_row(line, sizeof(line), &t);
        printf("%s\n", line);
        fflush(stdout);

        for (int i = 0; i < SETTLE_COUNT; i++) {
            capture_at(cycle, variant, t0_us, SETTLE_SCHEDULE_S[i]);
        }
#ifndef TAIL_DISABLED
        if (cycle == CYCLE_COUNT) {
            for (int i = 0; i < TAIL_COUNT; i++) {
                capture_at(cycle, variant, t0_us, TAIL_SCHEDULE_S[i]);
            }
        }
#endif

        // Let the host finish photographing this chart before the next refresh starts
        // repainting it underneath the camera.
        vTaskDelay(pdMS_TO_TICKS(CAPTURE_DRAIN_MS));
    }

    printf("@@DONE\n");
    printf("# colour chart run complete\n");
    fflush(stdout);
}

#endif // BUILD_COLOURCHART
