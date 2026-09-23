#pragma once

// Samples the two internal-RAM pools often enough to catch a trough, and remembers the moment
// and the concurrent activity of the deepest one. Ticket 47.
//
// The heartbeat has always printed heap_caps_get_minimum_free_size(), which is a since-boot
// watermark: it says how bad it got and nothing about when or why. That is why int_min=2899 --
// the lowest figure in this project -- is still unattributed after three candidates were tested
// against it, and why ticket 52's 15,487 is recorded as "not attributed" with the same stated
// cause. This is the instrument ticket 47 named as the cheapest thing that would close it.
//
// **There is no task here.** app_heapwatch_sample() is called from board_led.c's led_task, which
// already ticks every 20 ms. An instrument for internal-RAM scarcity that cost 2 KB of internal
// RAM for its own stack would be measuring itself, and docs/agents/board-and-storage.md records
// that two small tasks were once enough to make httpd_start() return ESP_ERR_HTTPD_TASK.
//
// **What that borrowing costs, stated because it bounds every conclusion drawn from this:** the
// LED task runs at priority 3, below the display task (5) and smbsync, so a trough that lives
// entirely inside an uninterrupted CPU burst by one of those is invisible here. Those tasks block
// on SPI and on sockets constantly, so a trough spanning a listing, a fetch or a decode is
// reachable. The heartbeat prints the sampled low BESIDE the true watermark for exactly this
// reason: if the watermark is lower, the sampler missed something and the gap is this
// instrument's own error, in bytes.

#include <stdbool.h>
#include <stdint.h>

#include "heapwatch.h"

// How recently a request must have begun for HEAPWATCH_F_HTTP to be set at the low. Generous on
// purpose: esp_http_server has no middleware hook and no "request finished" callback (see
// app_server.c), so the only stamp available is the start of routing. A request whose RESPONSE is
// what holds the memory -- send_file() slurping a 2 MB photograph is the case ticket 47 was
// looking at -- is still in flight long after that stamp, and one second covers it. The raw age
// in milliseconds is printed too, so a reader can apply a tighter rule at analysis time than this
// flag does.
#define HEAPWATCH_HTTP_RECENT_MS 1000

typedef struct {
    heapwatch_low_t int_low;  // lowest heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    heapwatch_low_t dma_low;  // lowest heap_caps_get_largest_free_block(MALLOC_CAP_DMA)
    // The one whose DEPTH is exact. Driven by heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
    // -- the since-boot watermark the heartbeat has always printed -- rather than by the
    // instantaneous free size.
    //
    // Arm A of 2026-09-11 is why this exists. The sampler recorded int_low=25,395 while the
    // watermark said 13,675: a gap of 11,720 bytes, so the trough was faster than a 20 ms tick at
    // priority 3 and the sampled depth was wrong by 46 %. The watermark is monotone
    // non-increasing, which is the property that fixes it -- a poll cannot miss THAT it fell, only
    // WHEN, and "when" is then bounded by one tick. So this record's `value` is exact and its
    // `at_ms` and `flags` are as of detection, up to 20 ms late.
    //
    // Which makes the two records answer different questions, and both are worth keeping:
    // int_low is what a fair sampler saw, wm_low is how bad it actually got. wm_low.value must
    // equal the heartbeat's int_min on every line; a difference is a fault in this instrument.
    heapwatch_low_t wm_low;
    uint32_t samples;  // how many times sample() ran, so a starved sampler is visible

    // The last eight falls of that watermark, which because it is monotone are also the eight
    // DEEPEST -- the tail of the boot's descent. Not the whole descent: a boot's own
    // initialisation produced 54 falls in one run, 46 of them above 107 KB and none of them
    // interesting.
    //
    // This is what ticket 47 asked for -- "a one-line ESP_LOGW when int_free crosses a floor" --
    // kept as a record for the heartbeat to print rather than logged from the sampler. The sampler
    // runs on the LED task's 1.5 KB stack beside a blocking RMT transaction, and ESP-IDF's printf
    // can want a kilobyte of it; the heartbeat runs on the main task and prints everything else on
    // this frame already.
    //
    // Why the deepest one alone was not enough, learned from arm B: it landed with flags=- 800 ms
    // before an on-demand window, which is a moment nothing in the log accounts for. One
    // unexplained sample is a hypothesis. The sequence -- what fell, when, and next to which log
    // line -- is what can be argued with.
    heapwatch_low_t falls[8];  // HEAPWATCH_FALLS, declared below for the reader's sake
    uint32_t fall_count;  // total falls, which may exceed the eight kept
} app_heapwatch_t;

#define HEAPWATCH_FALLS 8

// One sample of both pools. Cheap enough for a 20 ms tick and takes no lock: two heap queries and
// three reads of a volatile word. It deliberately does NOT call app_smb_sync_get_status() or
// app_display_busy(), both of which take a mutex -- a 50 Hz sampler blocking on a mutex held by
// the very task that is consuming the memory would distort what it is there to measure, and would
// put a priority-3 task in the way of a priority-5 one.
void app_heapwatch_sample(void);

// The two lows, for the heartbeat.
void app_heapwatch_get(app_heapwatch_t *out);

// Published by the subsystems rather than polled from them, for the locking reason above. Called
// where the state actually changes: app_smb_sync.c's window_task around its run, and
// app_display.c's display_task around a render.
void app_heapwatch_set_activity(uint32_t flag, bool on);

// Stamped once per HTTP request, from the uri_match_fn wrapper in app_server.c -- the one hook
// esp_http_server offers that fires before routing, so it catches 401s, 403s and 404s as well as
// the routes that answer.
void app_heapwatch_note_request(void);
