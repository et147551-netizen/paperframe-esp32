// The reTerminal E1002's sound: an MLT-8530 magnetic buzzer on GPIO45 (BUZZER_EN), driven by LEDC
// as a 50 % square wave. Ticket 16, FR-1.2 and FR-6.4.
//
// A magnetic transducer, not a self-oscillating buzzer: it sounds at whatever frequency it is
// driven at, and loudest near its ~2.7 kHz resonance. That is why every pitch here sits near
// 2.7 kHz rather than copying the M5Paper Color's tones. Those come out at ~4 kHz, and the MIDI
// notes they are named after are 7.9-8.9 kHz, well off this part's peak. A buzzer also cannot play
// a recording, so the boot sound is a three-note chime.
//
// READ, NOT MEASURED: the pin and part are from the V1.2 schematic (docs/board-pinmap.md). The
// drive polarity is NOT read from it -- the transistor stage between the pin and the coil is not
// in the extracted net list. So "idle LOW is silent and draws nothing" is the assumption here, and
// the first run on hardware is what checks it.
//
// GPIO45 is a strapping pin (VDD_SPI voltage). It is only sampled at reset, so driving it
// afterwards is safe; board_audio_init() is the first thing that touches it.

#include "board_audio.h"

#ifdef BOARD_RETERMINAL_E1002

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BUZZ_MODE LEDC_LOW_SPEED_MODE
#define BUZZ_TIMER LEDC_TIMER_0
#define BUZZ_CHANNEL LEDC_CHANNEL_0
#define BUZZ_RES LEDC_TIMER_10_BIT
#define BUZZ_DUTY_ON (1u << (10 - 1)) // 50 %

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_ready;

// One note. The durations are in FreeRTOS ticks underneath, so they are 10 ms granular at
// CONFIG_FREERTOS_HZ=100, and a one-tick delay can be as short as 0 ms. Every duration here is
// several ticks long for that reason.
static void note(uint32_t hz, uint32_t ms)
{
    ledc_set_freq(BUZZ_MODE, BUZZ_TIMER, hz);
    ledc_set_duty(BUZZ_MODE, BUZZ_CHANNEL, BUZZ_DUTY_ON);
    ledc_update_duty(BUZZ_MODE, BUZZ_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(BUZZ_MODE, BUZZ_CHANNEL, 0);
    ledc_update_duty(BUZZ_MODE, BUZZ_CHANNEL);
}

esp_err_t board_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    const ledc_timer_config_t timer = {
        .speed_mode = BUZZ_MODE,
        .timer_num = BUZZ_TIMER,
        .duty_resolution = BUZZ_RES,
        .freq_hz = 2700,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_BUZZER_GPIO,
        .speed_mode = BUZZ_MODE,
        .channel = BUZZ_CHANNEL,
        .timer_sel = BUZZ_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

void board_audio_play_boot(void)
{
    if (!s_ready) {
        return;
    }
    // C7-E7-G7, a rising chime in the buzzer's loud band.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    note(2093, 100);
    note(2637, 100);
    note(3136, 150);
    xSemaphoreGive(s_lock);
}

void board_audio_beep(board_button_t button)
{
    if (!s_ready) {
        return;
    }
    // Rising a semitone at a time in the M5Paper Color's order (TOP, DOWN, UP), centred on the
    // resonance.
    uint32_t hz = 2700;
    if (button == BOARD_BUTTON_DOWN) {
        hz = 2860;
    } else if (button == BOARD_BUTTON_UP) {
        hz = 3030;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    note(hz, 60);
    xSemaphoreGive(s_lock);
}

const char *board_audio_describe(void)
{
    return "MLT-8530 buzzer on GPIO45 via LEDC; boot: chime C7-E7-G7";
}

#endif // BOARD_RETERMINAL_E1002
