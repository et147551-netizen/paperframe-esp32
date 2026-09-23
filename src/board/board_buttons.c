// The three side buttons: poll, debounce, click, long press, hold tick.
//
// Ticket .scratch/digital-frame/issues/14; satisfies FR-6.1 and FR-6.2. Without M5Unified this
// module owns debouncing, click detection and long-press timing. Board-independent -- the pins
// come from board_buttons_hw.h.

#include "board_buttons.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_buttons_hw.h"

static const char *TAG = "buttons";

#define POLL_MS 10
#define DEBOUNCE_SAMPLES 3 // 30 ms of agreement before a level is believed
#define LONG_PRESS_MS 5000 // FR-6.3
#define HOLD_TICK_MS 200   // the LED blink cadence the reference uses while counting

typedef struct {
    bool down; // debounced
    bool raw_last;
    uint8_t stable; // consecutive samples agreeing with raw_last
    int64_t down_us;
    bool long_fired;
    int64_t last_tick_us;
} button_t;

static button_t s_buttons[BOARD_BUTTON_COUNT];
static board_button_cb_t s_cb;

const char *board_button_name(board_button_t button)
{
    return button < BOARD_BUTTON_COUNT ? BOARD_BUTTONS[button].name : "?";
}

int board_button_gpio(board_button_t button)
{
    return button < BOARD_BUTTON_COUNT ? BOARD_BUTTONS[button].gpio : -1;
}

bool board_button_is_down(board_button_t button)
{
    return button < BOARD_BUTTON_COUNT ? s_buttons[button].down : false;
}

static void buttons_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int64_t now = esp_timer_get_time();

        for (int i = 0; i < BOARD_BUTTON_COUNT; i++) {
            button_t *b = &s_buttons[i];
            const int gpio = BOARD_BUTTONS[i].gpio;
            const char *name = BOARD_BUTTONS[i].name;
            const bool raw = gpio_get_level(gpio) == 0; // active low

            if (raw == b->raw_last) {
                if (b->stable < DEBOUNCE_SAMPLES) {
                    b->stable++;
                }
            } else {
                b->raw_last = raw;
                b->stable = 1;
            }

            const bool settled = b->stable >= DEBOUNCE_SAMPLES;
            if (settled && raw != b->down) {
                b->down = raw;
                if (raw) {
                    b->down_us = now;
                    b->long_fired = false;
                    b->last_tick_us = now;
                    // Every edge carries its GPIO as well as its position name: the two were
                    // settled by pressing the buttons once, and a log that repeats both is
                    // what keeps a future remap honest -- which now includes a remap onto a
                    // different board.
                    printf("# button %s (GPIO%d) down\n", name, gpio);
                } else {
                    const uint32_t held = (uint32_t)((now - b->down_us) / 1000);
                    printf("# button %s (GPIO%d) up after %u ms\n", name, gpio,
                           (unsigned)held);
                    // A press that already fired its long-press is not also a click --
                    // otherwise holding TOP for the AP would toggle the orientation on
                    // release as well.
                    if (!b->long_fired && s_cb) {
                        s_cb((board_button_t)i, BOARD_BUTTON_CLICK, held);
                    }
                }
                fflush(stdout);
            }

            if (b->down) {
                const uint32_t held = (uint32_t)((now - b->down_us) / 1000);
                if (!b->long_fired && held >= LONG_PRESS_MS) {
                    b->long_fired = true;
                    printf("# button %s (GPIO%d) long press\n", name, gpio);
                    fflush(stdout);
                    if (s_cb) {
                        s_cb((board_button_t)i, BOARD_BUTTON_LONG_PRESS, held);
                    }
                } else if (!b->long_fired &&
                           (now - b->last_tick_us) >= HOLD_TICK_MS * 1000) {
                    b->last_tick_us = now;
                    if (s_cb) {
                        s_cb((board_button_t)i, BOARD_BUTTON_HOLD_TICK, held);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t board_buttons_init(board_button_cb_t cb)
{
    s_cb = cb;

    uint64_t mask = 0;
    for (int i = 0; i < BOARD_BUTTON_COUNT; i++) {
        mask |= 1ULL << BOARD_BUTTONS[i].gpio;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        // Both boards have external pull-ups; the internal one is belt and braces and costs
        // nothing while the button is up.
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    // Seed the debounce state from the pins as they are, so a button already held at boot does
    // not read as a fresh press.
    printf("# buttons: ");
    for (int i = 0; i < BOARD_BUTTON_COUNT; i++) {
        const bool raw = gpio_get_level(BOARD_BUTTONS[i].gpio) == 0;
        s_buttons[i].raw_last = raw;
        s_buttons[i].down = raw;
        s_buttons[i].stable = DEBOUNCE_SAMPLES;
        printf("%s=GPIO%d(%d) ", BOARD_BUTTONS[i].name, BOARD_BUTTONS[i].gpio, (int)raw);
    }
    printf("(active low, level at boot in brackets)\n");

    // 3 KB. The measured high-water mark says 2048 would do -- but it was measured with nobody
    // pressing anything, and a stack trimmed against a path that has never run is exactly what
    // put the display task 16 bytes from the end and the device in a reboot loop (ticket 21).
    // Re-trim once someone has actually used the buttons.
    if (xTaskCreate(buttons_task, "buttons", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
