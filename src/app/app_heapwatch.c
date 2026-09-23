#include "app_heapwatch.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

// No mutex, and that is a decision rather than an omission. The writers are one bit each from
// three different tasks and the reader is the LED task at 20 ms; a torn read costs one sample's
// flags, while a lock in this path costs the fidelity the sampler exists for. `volatile` because
// the compiler must not hoist these out of led_task's loop.
static volatile uint32_t s_activity;
static volatile int64_t s_http_last_us = INT64_MIN;

// Written only by app_heapwatch_sample() from the LED task, read by the heartbeat from the main
// task. A reader can see a record mid-update; the fields it would mix are all from the same
// sample within one 20 ms tick, and the alternative is a mutex in the sampling path.
static app_heapwatch_t s_watch;

void app_heapwatch_set_activity(uint32_t flag, bool on)
{
    if (on) {
        s_activity |= flag;
    } else {
        s_activity &= ~flag;
    }
}

void app_heapwatch_note_request(void)
{
    s_http_last_us = esp_timer_get_time();
}

void app_heapwatch_sample(void)
{
    const uint32_t int_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t dma_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const int64_t now_us = esp_timer_get_time();
    const int64_t now_ms = now_us / 1000;

    const int64_t last = s_http_last_us;
    const int64_t http_age_ms = (last == INT64_MIN) ? -1 : (now_us - last) / 1000;

    uint32_t flags = s_activity;
    if (http_age_ms >= 0 && http_age_ms <= HEAPWATCH_HTTP_RECENT_MS) {
        flags |= HEAPWATCH_F_HTTP;
    } else {
        flags &= ~HEAPWATCH_F_HTTP;
    }

    s_watch.samples++;
    // The exact-depth record. A monotone quantity, so this note() fires exactly on the tick that
    // notices a fall and the flags are at most one tick stale -- which is 20 ms against the 10 s
    // the heartbeat used to give. See app_heapwatch.h for the arm that made this necessary.
    if (heapwatch_note(&s_watch.wm_low,
                       (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), int_free,
                       now_ms, flags, http_age_ms)) {
        // A ring keeping the LAST eight, which because the watermark is monotone are also the
        // DEEPEST eight -- the tail of the descent, where the interesting excursions are.
        //
        // The first version of this kept the first eight instead, on the argument that the
        // beginning of a descent is where the cause is. It is not: boot's own initialisation walks
        // the watermark down from 172 KB to 107 KB in the first 348 ms, and a run of this
        // instrument on 2026-09-11 recorded `falls=54` whose kept eight were all inside that ramp.
        // Fifty-four falls and not one of them was worth a line.
        s_watch.falls[s_watch.fall_count % HEAPWATCH_FALLS] = s_watch.wm_low;
        s_watch.fall_count++;
    }
    // Each pool's own record, with the other pool's figure carried as context. int_free can rise
    // while dma_largest collapses -- the DMA pool is its own arena and fragments separately, which
    // is the whole reason the heartbeat prints both -- so one record could not hold the two events.
    heapwatch_note(&s_watch.int_low, int_free, dma_largest, now_ms, flags, http_age_ms);
    heapwatch_note(&s_watch.dma_low, dma_largest, int_free, now_ms, flags, http_age_ms);
}

void app_heapwatch_get(app_heapwatch_t *out)
{
    if (out) {
        *out = s_watch;
    }
}
