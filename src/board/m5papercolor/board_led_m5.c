// The M5Paper Color's light: two WS2812B-4020 on GPIO21, driven over RMT.
//
// Everything about WHEN it lights and in what pattern is board_led.c's. This file is the two
// functions of board_led_hw.h and nothing else.
//
// Two facts about the hardware, both from docs/board-pinmap.md and refs/_pm1.txt:
//
//   * The LEDs are on GPIO21, two of them, WS2812B-4020, driven over RMT.
//   * They are powered from the PM1's 3.3 V LDO and gated by its LED_EN pin -- PWR_CFG (0x06)
//     bits [2] and [4]. Both are 1 in the register's 0x17 reset value, so they are usually
//     already on; this module reads the register, sets them if they are not, and logs the raw
//     byte either way rather than assuming.

#include "board_led_hw.h"

#ifdef BOARD_M5PAPER_COLOR

#include <stdio.h>

#include "esp_log.h"
#include "led_strip.h"

#include "board_pm1.h"

static const char *TAG = "led";

#define LED_COUNT 2

// PWR_CFG (0x06). Bit 2 enables the 3.3 V LDO the LEDs hang off; bit 4 drives LED_EN. Both are
// set in the register's 0x17 default (refs/_pm1.txt:735-745), so this is a check rather than a
// fix -- but "the LED stays dark until this is on" (docs/board-pinmap.md) is not a thing to
// leave to a default.
#define PM1_REG_PWR_CFG 0x06
#define PWR_CFG_LDO_EN (1u << 2)
#define PWR_CFG_LED_EN (1u << 4)

static led_strip_handle_t s_strip;

static esp_err_t enable_rail(void)
{
    uint8_t cfg = 0;
    esp_err_t err = board_pm1_read_reg(PM1_REG_PWR_CFG, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR_CFG unreadable: %s", esp_err_to_name(err));
        return err;
    }
    printf("# led: PWR_CFG=0x%02X (ldo=%d led_en=%d)\n", cfg, (cfg & PWR_CFG_LDO_EN) ? 1 : 0,
           (cfg & PWR_CFG_LED_EN) ? 1 : 0);

    const uint8_t want = (uint8_t)(cfg | PWR_CFG_LDO_EN | PWR_CFG_LED_EN);
    if (want != cfg) {
        err = board_pm1_write_reg(PM1_REG_PWR_CFG, want);
        printf("# led: PWR_CFG 0x%02X -> 0x%02X (%s)\n", cfg, want, esp_err_to_name(err));
    }
    return err;
}

esp_err_t board_led_hw_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }
    // Deliberately not fatal: a bus error here leaves the frame with a dark LED, which is
    // worse than a lit one and better than no frame.
    enable_rail();

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {.invert_out = false},
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = false},
    };

    const esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    led_strip_clear(s_strip);
    return ESP_OK;
}

// Only writes when the colour actually changes. A WS2812 refresh is a blocking RMT
// transaction; at a 20 ms tick, repainting an unchanged colour fifty times a second would be
// waste on a device counting milliamps.
void board_led_hw_paint(uint8_t r, uint8_t g, uint8_t b)
{
    static int16_t last_r = -1, last_g = -1, last_b = -1;
    if (!s_strip || (r == last_r && g == last_g && b == last_b)) {
        return;
    }
    last_r = r;
    last_g = g;
    last_b = b;
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, r, g, b);
    }
    led_strip_refresh(s_strip);
}

bool board_led_hw_has_colour(void)
{
    return true;
}

const char *board_led_hw_describe(void)
{
    return "2x WS2812B on GPIO21 over RMT, full colour";
}

#endif // BOARD_M5PAPER_COLOR
