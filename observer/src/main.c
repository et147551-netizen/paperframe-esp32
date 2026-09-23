// Trace observer -- M5Capsule as a two-channel timing capture.
//
// Independently measures the phase boundaries the M5Paper Color harness reports, so
// that its esp_timer numbers are checked against something with its own crystal
// rather than trusted. This is the cross-check .scratch/epd-refresh-optimization/
// issues/05 asks for, done without buying a logic analyser.
//
// WHY AN INTERRUPT AND NOT THE RMT PERIPHERAL. RMT looks like the right tool -- it
// captures edge timings in hardware with no CPU jitter -- but its symbol duration
// field is 15 bits, so at the finest useful tick one interval tops out around 100 ms.
// The DRF phase is ~18.5 s. Spanning that needs a tick so coarse it would throw away
// the resolution the 80 ms PON threshold needs. This capture has five orders of
// magnitude of dynamic range (sub-ms gaps to a ~19 s wait), and a 64-bit microsecond
// counter handles that where a 15-bit duration field cannot.
//
// Accuracy budget: IRAM ISR entry latency on the ESP32-S3 is a few microseconds
// against an 80 ms threshold and a 240 ms transfer. Four orders of margin.
//
// ------------------------------------------------------------------- WIRING
//
//   M5Paper Color HY2.0-4P          M5Capsule HY2.0-4P
//     GND     (black)      <------->  GND     (black)
//     G4 mark              <------->  G13
//     G5 run               <------->  G15
//     5V      (red)         --XX--    5V      (red)     DO NOT CONNECT
//
// Both boards drive 5V on the Grove red wire -- the M5Paper Color through a
// current-limited switch, the Capsule from its own battery/USB rail. Tying two
// driven 5V rails together back-feeds one into the other. Remove or cut the red
// conductor, or use three individual jumpers for GND and the two signals.
//
// Both parts are 3.3 V logic, so no level shifting is needed.

#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"

// Grove pins on the M5Capsule. Note these collide numerically with the M5Paper
// Color's EPD MOSI (13) and SCLK (15) -- different board, no conflict, but do not
// confuse the two pin maps.
#define PIN_MARK GPIO_NUM_13 // <- M5Paper Color G4, toggles at each instant
#define PIN_RUN GPIO_NUM_15  // <- M5Paper Color G5, high for the whole refresh

#define MARKS_PER_RUN 11
#define MAX_MARKS 32 // room to see an over-count rather than silently truncating

typedef struct {
    int64_t us;
    uint8_t from_run_pin;
    uint8_t level;
} evt_t;

static QueueHandle_t s_events;

static void IRAM_ATTR edge_isr(void *arg)
{
    const uint32_t pin = (uint32_t)arg;
    // Read the port register directly rather than gpio_get_level(): fewer cycles in
    // the ISR, and no dependency on where the driver function was linked.
    const evt_t e = {
        .us = esp_timer_get_time(),
        .from_run_pin = (pin == PIN_RUN),
        .level = (uint8_t)((GPIO.in >> pin) & 1u),
    };
    BaseType_t higher_woke = pdFALSE;
    xQueueSendFromISR(s_events, &e, &higher_woke);
    if (higher_woke) {
        portYIELD_FROM_ISR();
    }
}

// The ten intervals between the eleven marks, in order. Names match the harness's
// CSV columns where they correspond; the gaps are the stock fixed delays, which this
// makes externally visible for the first time.
static const char *const INTERVAL_NAMES[MARKS_PER_RUN - 1] = {
    "t_reset_ms", "t_init_ms", "t_xfer_ms", "gap_xfer_pon_ms", "t_pon_ms",
    "delay_pon_drf_ms", "t_drf_ms", "gap_drf_pof_ms", "t_pof_ms", "tail_ms",
};

static void emit(uint32_t run, const int64_t *marks, int n, int64_t run_us)
{
    if (n != MARKS_PER_RUN) {
        printf("# obs run %u: %d marks, expected %d -- refresh aborted, or an edge\n"
               "#   was missed. Intervals below are NOT aligned to the phase names.\n",
               (unsigned)run, n, MARKS_PER_RUN);
    }

    printf("%u,%d", (unsigned)run, n);
    for (int i = 1; i < n && i < MARKS_PER_RUN; i++) {
        printf(",%.3f", (double)(marks[i] - marks[i - 1]) / 1000.0);
    }
    for (int i = n; i < MARKS_PER_RUN; i++) {
        printf(",");
    }
    printf(",%.3f\n", (double)run_us / 1000.0);
    fflush(stdout);
}

void app_main(void)
{
    printf("\n# M5Capsule trace observer\n");
    printf("# PIN_MARK=G%d  PIN_RUN=G%d  (do not connect the Grove 5V wire)\n",
           PIN_MARK, PIN_RUN);
    printf("# Compare t_xfer_ms / t_pon_ms / t_drf_ms / t_pof_ms against the\n"
           "# harness CSV. Tolerance is RELATIVE: within 200 ppm of the interval,\n"
           "# or 50 us, whichever is larger. Two independent crystals can differ by\n"
           "# ~80 ppm, which is 1.5 ms on the 18.5 s DRF phase -- an absolute\n"
           "# microsecond window would fail two healthy boards.\n"
           "# 'marks' must read %d. A short count means the refresh aborted and the\n"
           "# intervals are NOT aligned to the column names.\n",
           MARKS_PER_RUN);

    s_events = xQueueCreate(64, sizeof(evt_t));

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_MARK) | (1ULL << PIN_RUN),
        .mode = GPIO_MODE_INPUT,
        // No pull. Both lines are actively driven by the other board, and a pull
        // would fight it and slow the edges.
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_MARK, edge_isr, (void *)PIN_MARK));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_RUN, edge_isr, (void *)PIN_RUN));

    printf("obs_run,marks");
    for (int i = 0; i < MARKS_PER_RUN - 1; i++) {
        printf(",%s", INTERVAL_NAMES[i]);
    }
    printf(",t_total_ms\n");

    int64_t marks[MAX_MARKS];
    int n = 0;
    int64_t run_start = 0;
    bool collecting = false;
    uint32_t run = 0;

    for (;;) {
        evt_t e;
        if (xQueueReceive(s_events, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (e.from_run_pin) {
            if (e.level) {
                collecting = true;
                run_start = e.us;
                n = 0;
            } else if (collecting) {
                collecting = false;
                emit(++run, marks, n, e.us - run_start);
            }
            continue;
        }

        if (collecting && n < MAX_MARKS) {
            marks[n++] = e.us;
        } else if (collecting) {
            // Over-count: keep going so emit() can report it rather than wrapping.
            n++;
        }
    }
}
