// The reTerminal E1002's light: one monochrome LED on GPIO6, active low, plain GPIO.
//
// READ, NOT MEASURED: GPIO6 and the active-low sense come from
// refs/esp32-photoframe/components/board_hal/include/board_seeedstudio_reterminal_e1002.h
// (BOARD_HAL_LED_PIN, BOARD_HAL_LED_INVERTED true).
//
// WHAT THIS BOARD CANNOT DO. board_led.c's patterns all survive -- the 50 ms/2 s resting
// heartbeat, the four-blink bursts, the one-second error fade (as a blink, since brightness is
// not available either), the 200 ms hold flash. What does not survive is the COLOUR CODING, and
// two of board_led.h's promises rest on it:
//
//   * BOARD_LED_REFRESH is cyan "so working and done differ at a glance during the 15.6 s in
//     which the panel still shows the old image". Here they differ by pattern only -- and the
//     four-blink bursts of REFRESH and SUCCESS are the same pattern, so during a refresh they
//     do not differ at all until the burst ends.
//   * BOARD_LED_REFRESH and BOARD_LED_SUCCESS share the four-blink burst, so during a refresh
//     they do not differ at all until the burst ends.
//
// **The second one was BOARD_LED_FAULT, and it is fixed as of 2026-09-20.** It read as IDLE here,
// because red at half against green at half is the same light — stated rather than worked around,
// on the grounds that a pattern invented to stand in for a colour would change the 2.5 % duty
// board_led.h chose deliberately, and inventing one silently is worse than a documented gap.
// **The owner then made the LED's blink state the designated way to tell a resting frame from a
// broken one** (ticket 68 §11), at which point a fault that looks like resting stopped being a
// documented gap and became a wrong answer. So `board_led_hw_has_colour()` returns false here and
// board_led.c gives FAULT a **double pulse at the same total on-time** — the duty argument is
// honoured rather than overruled. See the FAULT case there for the arithmetic.

#include "board_led_hw.h"

#ifdef BOARD_RETERMINAL_E1002

#include "driver/gpio.h"

static bool s_ready;

esp_err_t board_led_hw_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    board_led_hw_paint(0, 0, 0);
    return ESP_OK;
}

void board_led_hw_paint(uint8_t r, uint8_t g, uint8_t b)
{
    // Any non-zero channel is "lit". That is the only reading a single monochrome LED can
    // give, and it is why the duty cycles survive and the colours do not.
    const bool lit = (r | g | b) != 0;

    static int8_t last = -1;
    if (!s_ready || (int8_t)lit == last) {
        return;
    }
    last = (int8_t)lit;
#if BOARD_LED_ACTIVE_LOW
    gpio_set_level(BOARD_LED_GPIO, lit ? 0 : 1);
#else
    gpio_set_level(BOARD_LED_GPIO, lit ? 1 : 0);
#endif
}

bool board_led_hw_has_colour(void)
{
    return false;
}

const char *board_led_hw_describe(void)
{
    return "1x monochrome LED on GPIO6, active low -- no colour, so FAULT is a double pulse";
}

#endif // BOARD_RETERMINAL_E1002
