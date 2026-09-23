#include "app_gphotos.h"

#ifdef FRAME_GPHOTOS_PROBE

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_heapwatch.h"

// The photograph, not the album index. Both were measured at the same ~45-47 KB in isolation --
// the cost is the SESSION, not the body -- and the photograph is what the frame does once per
// advance, 24 times a day at the shipping interval, against the index's once. So this arm drives
// the frequent case. The index refresh is rarer and its isolated cost is already known.
#ifndef FRAME_GPHOTOS_URL
#define FRAME_GPHOTOS_URL ""
#endif

// Seconds between fetches. Short on purpose: a trough only matters if it COINCIDES with a
// refresh, an upload or a sync window, and docs/hardware-runs.md's lesson is that a
// sub-second event is caught by DENSITY rather than by timing. At 20 s a 15.6 s refresh is almost
// always in flight for part of a fetch, and over an hour every pairing gets many chances.
#ifndef FRAME_GPHOTOS_EVERY_S
#define FRAME_GPHOTOS_EVERY_S 20
#endif

// 8 KB, which is what env:gphotosprobe measured rather than guessed: its fetch task used 3,716
// bytes of 16,384. It is also the figure ticket 23 settled on for smbsync, so the two are budgeted
// the same way.
#ifndef FRAME_GPHOTOS_STACK
#define FRAME_GPHOTOS_STACK 8192
#endif

// A browser User-Agent, and it is not cosmetic. Measured 2026-09-12 with one variable: the album
// document is 1,099,661 B to a Chrome UA and 1,219,869 B to the probe's own UA -- spelled
// "M5PaperColor/gphotosprobe" when that reading was taken, "PaperFrame/gphotosprobe" since
// 2026-09-18 -- with the
// same four photo URLs either way. A non-browser UA buys a ~120 KB larger shell and nothing else.
#define GPHOTOS_UA \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/120.0.0.0 Safari/537.36"

// PSRAM, allocated once and kept, so a round never pays for it and internal RAM never does. The
// largest photograph this CDN returns at 400x600 measured 158,590 B; 512 KB is generous and PSRAM
// has ~6.36 MB free.
#define GPHOTOS_BODY_MAX (512 * 1024)

static volatile bool s_busy;
static uint8_t *s_body;
static int64_t s_last_us;
static uint32_t s_seq;

bool app_gphotos_busy(void)
{
    return s_busy;
}

// Raw counters only, in the same shape env:gphotosprobe prints, so the two runs are comparable
// line for line. No verdict: ticket 03 cost a re-run because a derived boolean in first-cut
// instrumentation measured something other than what its name said.
static void gp_stamp(uint32_t seq, const char *stage)
{
    printf("@@GPF %u %s t_us=%lld int_free=%u int_min=%u dma_largest=%u psram_free=%u\n",
           (unsigned)seq, stage, (long long)esp_timer_get_time(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
}

static void fetch_task(void *arg)
{
    const uint32_t seq = (uint32_t)(uintptr_t)arg;

    // Published at the point the state changes, which is what lets the 20 ms sampler attribute a
    // trough to a TLS session without taking any lock. Set BEFORE the client is created, because
    // esp_http_client_init() already allocates.
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, true);
    gp_stamp(seq, "begin");

    esp_http_client_config_t cfg = {
        .url = FRAME_GPHOTOS_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .timeout_ms = 30000,
        .user_agent = GPHOTOS_UA,
        .keep_alive_enable = false,
    };

    size_t total = 0;
    int status = 0;
    const int64_t t0 = esp_timer_get_time();
    int64_t dt_open = 0;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        printf("@@GPF %u init_failed\n", (unsigned)seq);
    } else {
        const int64_t t_open = esp_timer_get_time();
        const esp_err_t err = esp_http_client_open(c, 0);
        dt_open = esp_timer_get_time() - t_open;
        if (err != ESP_OK) {
            // A handshake that fails UNDER LOAD is this arm's whole subject, not a nuisance: it is
            // what "does not fit" looks like from the caller's side. Printed with the heap figures
            // beside it so the failure carries its own cause.
            printf("@@GPF %u open_failed rc=%s dt_us=%lld\n", (unsigned)seq,
                   esp_err_to_name(err), (long long)dt_open);
            gp_stamp(seq, "open_failed");
        } else {
            gp_stamp(seq, "handshake");
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            int r;
            while ((r = esp_http_client_read(c, (char *)s_body + total,
                                             (int)(GPHOTOS_BODY_MAX - total))) > 0) {
                total += (size_t)r;
                if (total >= GPHOTOS_BODY_MAX) {
                    break;
                }
            }
            gp_stamp(seq, "body");
            esp_http_client_close(c);
        }
        esp_http_client_cleanup(c);
    }

    printf("@@GPF %u done status=%d bytes=%u open_us=%lld total_us=%lld stack_free=%u\n",
           (unsigned)seq, status, (unsigned)total, (long long)dt_open,
           (long long)(esp_timer_get_time() - t0),
           (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    fflush(stdout);

    // Cleared last, and after the stamp, so no sample can see the flag off while the client's
    // memory is still held.
    app_heapwatch_set_activity(HEAPWATCH_F_HTTPS, false);
    gp_stamp(seq, "end");
    s_busy = false;
    vTaskDelete(NULL);
}

void app_gphotos_tick(void)
{
    if (!FRAME_GPHOTOS_URL[0]) {
        static bool said;
        if (!said) {
            said = true;
            printf("# gphotos probe: built in but no FRAME_GPHOTOS_URL, so it will never fetch\n");
        }
        return;
    }
    if (s_busy) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_last_us && (now - s_last_us) < (int64_t)FRAME_GPHOTOS_EVERY_S * 1000000) {
        return;
    }

    if (!s_body) {
        s_body = heap_caps_malloc(GPHOTOS_BODY_MAX, MALLOC_CAP_SPIRAM);
        if (!s_body) {
            printf("# gphotos probe: no PSRAM for the %d byte body buffer\n", GPHOTOS_BODY_MAX);
            return;
        }
        printf("# gphotos probe: %d byte body buffer in psram, every %d s, ua=browser\n",
               GPHOTOS_BODY_MAX, FRAME_GPHOTOS_EVERY_S);
        // The knob arm changes these, so the capture says which ones it ran under rather than
        // relying on whatever sdkconfig.frame happened to hold that day.
        printf(GPHOTOS_MBEDTLS_COND_FMT, GPHOTOS_MBEDTLS_COND_ARGS);
    }

    s_last_us = now;
    s_busy = true;
    s_seq++;
    // Transient, and its failure is a RESULT. 8 KB of internal RAM not being available here is
    // precisely the thing this arm is trying to find out about, so it is printed with the heap
    // figures rather than retried.
    if (xTaskCreate(fetch_task, "gphotos", FRAME_GPHOTOS_STACK, (void *)(uintptr_t)s_seq, 4,
                    NULL) != pdPASS) {
        printf("@@GPF %u task_create_failed stack=%d int_free=%u int_largest=%u dma_largest=%u\n",
               (unsigned)s_seq, FRAME_GPHOTOS_STACK,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        fflush(stdout);
        s_busy = false;
    }
}

#else // FRAME_GPHOTOS_PROBE

void app_gphotos_tick(void)
{
}

bool app_gphotos_busy(void)
{
    return false;
}

#endif // FRAME_GPHOTOS_PROBE
