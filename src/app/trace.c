#include "trace.h"

volatile uint32_t g_trace_mark_level = 0;

void trace_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << TRACE_PIN_MARK) | (1ULL << TRACE_PIN_RUN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    // Both idle low, so the observer's first edge of a session is unambiguous.
    GPIO.out_w1tc = (1u << TRACE_PIN_MARK) | (1u << TRACE_PIN_RUN);
    g_trace_mark_level = 0;
}
