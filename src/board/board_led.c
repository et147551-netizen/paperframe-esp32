// The status-LED state machine: patterns, timings, and the 20 ms tick everything else
// borrows. Board-independent -- the light itself is two functions behind board_led_hw.h.
//
// The patterns are the shipping firmware's, transcribed from
// refs/M5PaperColor-UserDemo/main/hal/hal.cpp:725-820. Nothing here is ever steadily lit; see
// board_led.h for why that is a power decision and not a style one.

#include "board_led.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_led_hw.h"

#define TICK_MS 20

// The reference drives full-scale colours through a 0-100 brightness control; these are its
// two levels expressed directly: "brightness 100" for an attention blink, "brightness 50" for
// the resting heartbeat.
#define LEVEL_FULL 200
#define LEVEL_HALF 100

// Resting heartbeat: 50 ms of light every 2 s, which is the shipping firmware's
// LED_STATE_SUCCESS_STEADY_BLINK. 2.5 % duty on two LEDs averages about a milliamp -- the
// reason nothing here is ever steadily lit on a frame meant to run for weeks.
#define HEARTBEAT_PERIOD_MS 2000
#define HEARTBEAT_ON_MS 50

#define BLINK_MS 100 // the four-blink attention bursts
#define BLINK_COUNT 4
#define ERROR_FADE_MS 1050
#define HOLD_PERIOD_MS 200 // FR-6.3's "blinks white every 200 ms while counting"

static bool s_started;
static volatile board_led_state_t s_requested = BOARD_LED_OFF;
// What the transient states fall back to. IDLE until something says otherwise; the two
// assignments below read this instead of the BOARD_LED_IDLE literal they used to carry, which
// is the whole of ticket 55's fix -- see board_led.h for what that literal cost.
static volatile board_led_state_t s_resting = BOARD_LED_IDLE;
// Installed by the application, called on every tick. See the call site in led_task().
static void (*volatile s_tick_hook)(void);

// True during the lit part of the resting heartbeat.
static bool heartbeat_on(uint32_t elapsed_ms)
{
    return (elapsed_ms % HEARTBEAT_PERIOD_MS) < HEARTBEAT_ON_MS;
}

// BOARD_LED_FAULT's lit windows. One 50 ms flash where a colour can carry the meaning, two 25 ms
// flashes 150 ms apart where it cannot.
//
// **The total on-time is the same 50 ms either way, and that is the whole point of the shape.**
// board_led.h calls the 2.5 % duty the power budget, and ticket 55 §3 chose IDLE and FAULT costing
// the same on purpose; splitting one flash into two of half the length keeps both promises while
// making the two states tell apart by eye on a board with one monochrome LED (ticket 68 §11 is why
// that became necessary).
//
// Two caveats on the timing, neither of which changes the comparison because IDLE is subject to
// both equally. The 20 ms tick QUANTISES these windows, so the lit time is 2-3 ticks rather than
// exactly 50 ms — and each 25 ms window is wider than one tick interval, so neither flash can be
// missed entirely. And 150 ms of dark is far above flicker fusion, so two flashes read as two.
#define FAULT_FLASH_MS 25
#define FAULT_GAP_MS 150

static bool fault_on(uint32_t elapsed_ms)
{
    if (board_led_hw_has_colour()) {
        return heartbeat_on(elapsed_ms);
    }
    const uint32_t phase = elapsed_ms % HEARTBEAT_PERIOD_MS;
    const uint32_t second = FAULT_FLASH_MS + FAULT_GAP_MS;
    return phase < FAULT_FLASH_MS || (phase >= second && phase < second + FAULT_FLASH_MS);
}

// True during the lit part of a four-blink burst; `done` says the burst has finished.
static bool burst_on(uint32_t elapsed_ms, bool *done)
{
    const uint32_t phase = elapsed_ms / BLINK_MS;
    *done = phase >= BLINK_COUNT * 2;
    return !*done && (phase % 2) == 0;
}

static void led_task(void *arg)
{
    (void)arg;
    board_led_state_t state = BOARD_LED_OFF;
    uint32_t state_start_ms = 0;

    for (;;) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        const board_led_state_t requested = s_requested;
        if (requested != state) {
            state = requested;
            state_start_ms = now;
        }
        const uint32_t elapsed = now - state_start_ms;

        switch (state) {
        case BOARD_LED_OFF:
            board_led_hw_paint(0, 0, 0);
            break;

        case BOARD_LED_IDLE:
            board_led_hw_paint(0, heartbeat_on(elapsed) ? LEVEL_HALF : 0, 0);
            break;

        case BOARD_LED_REFRESH: {
            bool done = false;
            const bool on = burst_on(elapsed, &done);
            if (!done) {
                board_led_hw_paint(0, on ? LEVEL_FULL : 0, on ? LEVEL_FULL : 0);
            } else {
                const bool beat = heartbeat_on(elapsed - BLINK_COUNT * 2 * BLINK_MS);
                board_led_hw_paint(0, beat ? LEVEL_HALF : 0, beat ? LEVEL_HALF : 0);
            }
            break;
        }

        case BOARD_LED_SUCCESS: {
            bool done = false;
            const bool on = burst_on(elapsed, &done);
            if (!done) {
                board_led_hw_paint(0, on ? LEVEL_FULL : 0, 0);
            } else {
                s_requested = s_resting; // transient: falls back by itself
            }
            break;
        }

        case BOARD_LED_ERROR:
            if (elapsed < ERROR_FADE_MS) {
                // Full to dark over a second, the reference's fade in 50 ms steps.
                const uint32_t step = (elapsed / 50) * (LEVEL_FULL / 21);
                board_led_hw_paint(step >= LEVEL_FULL ? 0 : (uint8_t)(LEVEL_FULL - step), 0,
                                   0);
            } else {
                s_requested = s_resting;
            }
            break;

        case BOARD_LED_FAULT:
            // IDLE's pulse in red rather than green: same 50 ms every 2 s, same half
            // brightness, so the power budget is unchanged and only the colour says anything.
            //
            // ON A MONOCHROME BOARD THAT MADE IT INDISTINGUISHABLE FROM IDLE, because red at
            // half against green at half is the same light. That was accepted as a documented
            // gap, on the grounds that changing the duty here would spend a power budget ticket
            // 55 §3 chose deliberately -- and **the gap is closed as of 2026-09-20 without
            // spending it**, because fault_on() splits the same 50 ms into two flashes rather
            // than lengthening it. The operator made the blink state the designated way to tell
            // a resting frame from a broken one (ticket 68 §11), so a fault that looks like
            // resting became a wrong answer rather than a missing nicety.
            //
            // The colour still carries it wherever there is one, so nothing changes on the
            // M5Paper Color. `led=` on the heartbeat remains what an unattended run reads on
            // either board -- the flatbed photographs the panel, not the LEDs (ticket 55 §3).
            board_led_hw_paint(fault_on(elapsed) ? LEVEL_HALF : 0, 0, 0);
            break;

        case BOARD_LED_HOLD: {
            const bool on = (elapsed % HOLD_PERIOD_MS) < (HOLD_PERIOD_MS / 2);
            board_led_hw_paint(on ? LEVEL_FULL : 0, on ? LEVEL_FULL : 0,
                               on ? LEVEL_FULL : 0);
            break;
        }
        }

        // Whoever wanted a 20 ms tick and did not want to pay for a task to get one. Ticket
        // 47's internal-RAM sampler is the caller this exists for: a task of its own would
        // take its stack out of the very pool it is there to watch. Called with the LED's own
        // work already done, so a slow hook delays the next tick and not this one's paint.
        //
        // Nothing board-specific may be assumed about it -- this file does not include the
        // header of whatever installs it, which is what keeps it on the portable side of
        // ticket 43's border.
        if (s_tick_hook) {
            s_tick_hook();
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t board_led_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    const esp_err_t err = board_led_hw_init();
    if (err != ESP_OK) {
        return err;
    }

    // 2 KB. It was 1536 with a measured spare of 548 bytes across a night of blinking, and the
    // tick hook now runs a stranger's call frame on top of that -- heap_caps_get_free_size()
    // walks the allocator's block list under its own lock. 512 bytes is the cheapest honest
    // margin, and `stacks led=` on the heartbeat is what says whether it was enough.
    if (xTaskCreate(led_task, "led", 2048, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    printf("# led: %s, %u ms pulse every %u ms when idle\n", board_led_hw_describe(),
           (unsigned)HEARTBEAT_ON_MS, (unsigned)HEARTBEAT_PERIOD_MS);
    return ESP_OK;
}

void board_led_set(board_led_state_t state)
{
    s_requested = state;
}

void board_led_set_resting(board_led_state_t state)
{
    s_resting = state;
    // Take effect now unless a transient is mid-play: those end by assigning s_resting
    // themselves, so they will land here anyway, and interrupting a four-blink burst or a fade
    // half way through would look like a glitch rather than a state.
    const board_led_state_t now = s_requested;
    if (now != BOARD_LED_SUCCESS && now != BOARD_LED_ERROR && now != BOARD_LED_HOLD &&
        now != BOARD_LED_REFRESH) {
        s_requested = state;
    }
}

board_led_state_t board_led_get(void)
{
    return s_requested;
}

void board_led_set_tick_hook(void (*hook)(void))
{
    s_tick_hook = hook;
}
