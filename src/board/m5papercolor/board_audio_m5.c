// The M5Paper Color's sound: an ES8311 codec on the shared I2C bus and a speaker amplifier, fed
// 44.1 kHz 16-bit stereo over I2S0. Ticket 16, FR-1.2 and FR-6.4.
//
// POWERED ONLY WHILE PLAYING. Each sound raises the codec and amplifier enables, writes the
// codec's eight registers, creates the I2S channel, plays, drains, and then deletes the channel
// and drops both enables. The reason is internal RAM: the channel's DMA buffers come out of it,
// and a frame that kept a speaker ready around the clock would hold them forever to save a few
// milliseconds per press. The DMA is kept small (2 x 240 frames). A whole sound measures 2,148 B
// of internal RAM at the median. A press can land in the middle of an SMB fetch, when dma_largest
// is at its lowest, so a sound first WAITS for internal RAM (wait_for_ram()), and it is dropped
// if the RAM does not come back. If the channel still cannot be allocated, the sound is skipped
// and a line is logged. Nothing else is affected.
//
// Everything below except the boot chime itself (see play_boot_sound()) is transcribed from what
// the shipping firmware does through M5Unified. None of it is tuned by ear:
//   * enables and codec registers: refs/M5Unified/src/M5Unified.cpp:637-658 (DAC at +16 dB);
//   * pins, port, rate and stereo: M5Unified.cpp:2970-2977;
//   * slot format: Speaker_Class.cpp:226-239, which is IDF's Philips 16-bit stereo default;
//   * LEVEL. Speaker_Class.cpp scales a sample by 2 x magnification x master^2 x channel^2 / 2^36
//     (:549, :668, :757-761, then >> 8 at :957). The magnification is 1 (:2975) and the channel
//     volume is 255. That leaves the master volume, which is 200 for the boot sound
//     (UserDemo main.cpp:29) and 120 for the press tones (local_photo_slideshow.cpp:120). The two
//     Q16 gains below are those products.
//
// **The press tone reproduces what the shipping firmware PLAYS, not what its source appears to
// ask for.** audio.cpp's play_tone() builds an interleaved stereo buffer and hands it to
// playRaw(), whose `stereo` defaults to false. So each sample is played twice in a row: the tone
// comes out an octave below its MIDI note, for twice its stated 80 ms, as a two-sample staircase.
// MIDI 119 is therefore ~3.95 kHz for 160 ms. tone_frame() below builds that same waveform.

#include "board_audio.h"

#ifdef BOARD_M5PAPER_COLOR

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_pm1.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SAMPLE_RATE 44100
#define DMA_DESC_NUM 2
#define DMA_FRAME_NUM 240
#define WRITE_TIMEOUT_MS 500

// 2 x 1 x 200^2 x 255^2 / 2^36 = 0.07570 and 2 x 1 x 120^2 x 255^2 / 2^36 = 0.02725, in Q16.
#define GAIN_BOOT_Q16 4961
#define GAIN_TONE_Q16 1786

// audio.cpp: 32767 / 5 peak, and a linear fade over the buffer's last 200 samples.
#define TONE_AMPLITUDE (32767.0f / 5.0f)
#define TONE_FADE_SAMPLES 200
#define TONE_SECONDS 0.08f

static const uint8_t ES8311_INIT[][2] = {
    {0x00, 0x80}, // RESET: CSM power on
    {0x01, 0xB5}, // CLOCK_MANAGER: MCLK taken from BCLK
    {0x02, 0x18}, // CLOCK_MANAGER: MULT_PRE = 3
    {0x0D, 0x01}, // SYSTEM: power up the analogue circuitry
    {0x12, 0x00}, // SYSTEM: power up the DAC
    {0x13, 0x10}, // SYSTEM: enable the output to the HP drive
    {0x32, 0xCF}, // DAC volume, +16 dB
    {0x37, 0x08}, // DAC: bypass the equaliser
};

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_ready;

// One DMA frame's worth of stereo samples, 960 B. It is not on the stack because
// board_audio_beep() runs on the button task, whose stack is 3,072 bytes. It is not in .bss
// either, because that is internal RAM: board_audio_init() takes it from PSRAM once.
// i2s_channel_write() copies out of it into the DMA buffers. s_lock serialises every use.
#define CHUNK_BYTES (DMA_FRAME_NUM * 2 * sizeof(int16_t))
static int16_t *s_chunk;

static void enables(bool on)
{
    // Amplifier off before the codec, and on after it, so the speaker never sees the codec
    // coming out of reset.
    if (on) {
        gpio_set_level(BOARD_CODEC_EN_GPIO, 1);
        gpio_set_level(BOARD_SPK_EN_GPIO, 1);
    } else {
        gpio_set_level(BOARD_SPK_EN_GPIO, 0);
        gpio_set_level(BOARD_CODEC_EN_GPIO, 0);
    }
}

static esp_err_t codec_init(void)
{
    i2c_master_bus_handle_t bus = board_pm1_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_I2C_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }
    // Three tries per register, as M5Unified's in_i2c_bulk_write(..., 3): the codec has only
    // just been released from reset.
    for (size_t i = 0; i < sizeof(ES8311_INIT) / sizeof(ES8311_INIT[0]) && err == ESP_OK; i++) {
        for (int attempt = 0; attempt < 3; attempt++) {
            err = i2c_master_transmit(dev, ES8311_INIT[i], 2, 100);
            if (err == ESP_OK) {
                break;
            }
        }
    }
    i2c_master_bus_rm_device(dev);
    return err;
}

// A sound YIELDS to the network. Before anything is allocated, wait until both internal-RAM
// figures are at least AUDIO_RAM_MIN_BYTES, polling every 50 ms; after AUDIO_RAM_WAIT_MS the
// sound is dropped. Speed is not a requirement here, and a late beep is better than a starved
// lwIP.
//
// The threshold rests on two premises, and it is wrong if either moves:
//   * A sound's internal-RAM cost is measured at a median of 2,148 B, range 1,592-2,180 B, over
//     20 sounds. Every one of them was back to its starting figure after teardown.
//     (.scratch/captures/audio-mem-20260922-223120.log, ticket 16.)
//   * lwIP silently drops frames once dma_largest is below ~2 KB
//     (docs/agents/board-and-storage.md).
// If the channel came out of the largest block, keeping 2 KB clear needs ~4.2 KB. 8 KB is about
// twice that. It also sits above the 5,888 B that an on-demand SMB fetch's trough reaches, so a
// press at that moment waits the fetch out.
//
// The printf is on the caller's (button task's) stack. Only the rare wait and skip paths print.
#ifndef AUDIO_RAM_MIN_BYTES
#define AUDIO_RAM_MIN_BYTES 8192 // a build flag may override it, which is how the skip path is tested
#endif
#define AUDIO_RAM_WAIT_MS 2000
#define AUDIO_RAM_POLL_MS 50

static bool wait_for_ram(void)
{
    for (int waited = 0;; waited += AUDIO_RAM_POLL_MS) {
        const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        if (int_free >= AUDIO_RAM_MIN_BYTES && dma_largest >= AUDIO_RAM_MIN_BYTES) {
            if (waited > 0) {
                printf("# audio: waited %d ms for internal RAM\n", waited);
            }
            return true;
        }
        if (waited >= AUDIO_RAM_WAIT_MS) {
            printf("# audio: skipped, internal RAM low for %d ms (int_free=%u dma_largest=%u "
                   "need=%u)\n", waited, (unsigned)int_free, (unsigned)dma_largest,
                   (unsigned)AUDIO_RAM_MIN_BYTES);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_RAM_POLL_MS));
    }
}

// Everything a sound needs, up to a channel ready to take samples. On failure, everything this
// raised is already back down.
static esp_err_t session_open(i2s_chan_handle_t *out)
{
    enables(true);
    vTaskDelay(2); // 10-20 ms at 100 Hz. The codec has just left reset; no settle time is documented
    esp_err_t err = codec_init();
    if (err != ESP_OK) {
        printf("# audio: es8311 init failed: %s\n", esp_err_to_name(err));
        enables(false);
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear = true; // an underrun sends silence, not the last buffer again
    i2s_chan_handle_t tx = NULL;
    err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err == ESP_OK) {
        const i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                            I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = BOARD_I2S_MCLK_GPIO,
                .bclk = BOARD_I2S_BCK_GPIO,
                .ws = BOARD_I2S_WS_GPIO,
                .dout = BOARD_I2S_DOUT_GPIO,
                .din = I2S_GPIO_UNUSED,
            },
        };
        err = i2s_channel_init_std_mode(tx, &std_cfg);
        if (err == ESP_OK) {
            err = i2s_channel_enable(tx);
        }
        if (err != ESP_OK) {
            i2s_del_channel(tx);
        }
    }
    if (err != ESP_OK) {
        printf("# audio: i2s channel failed: %s -- sound skipped\n", esp_err_to_name(err));
        enables(false);
        return err;
    }
    *out = tx;
    return ESP_OK;
}

static void session_write(i2s_chan_handle_t tx, size_t frames)
{
    size_t written;
    i2s_channel_write(tx, s_chunk, frames * 2 * sizeof(int16_t), &written,
                      pdMS_TO_TICKS(WRITE_TIMEOUT_MS));
}

static void session_close(i2s_chan_handle_t tx)
{
    // Push the last real samples out of the DMA ring with a ring's worth of silence, then allow
    // one more buffer's time (5.4 ms) for the one being shifted out.
    memset(s_chunk, 0, CHUNK_BYTES);
    for (int i = 0; i < DMA_DESC_NUM; i++) {
        session_write(tx, DMA_FRAME_NUM);
    }
    vTaskDelay(2);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    enables(false);
}

static inline int16_t scale(int32_t sample, int32_t gain_q16)
{
    return (int16_t)((sample * gain_q16) >> 16);
}

// Frame `k` of the shipping press tone: audio.cpp's sample k/2, per the note at the top.
static int16_t tone_frame(float hz, size_t k, size_t stated_samples)
{
    const size_t i = k >> 1;
    float amp = TONE_AMPLITUDE;
    if (i >= stated_samples - TONE_FADE_SAMPLES) {
        amp *= (float)(stated_samples - i) / TONE_FADE_SAMPLES;
    }
    return (int16_t)(amp * sinf(2.0f * (float)M_PI * hz * (float)i / SAMPLE_RATE));
}

static void play_tone(i2s_chan_handle_t tx, float hz, float stated_seconds, int32_t gain_q16)
{
    const size_t stated = (size_t)(SAMPLE_RATE * stated_seconds);
    const size_t total = stated * 2;
    for (size_t done = 0; done < total;) {
        size_t n = total - done;
        if (n > DMA_FRAME_NUM) {
            n = DMA_FRAME_NUM;
        }
        for (size_t f = 0; f < n; f++) {
            const int16_t v = scale(tone_frame(hz, done + f, stated), gain_q16);
            s_chunk[f * 2] = v;
            s_chunk[f * 2 + 1] = v;
        }
        session_write(tx, n);
        done += n;
    }
}

static float midi_hz(int midi)
{
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

// FR-1.2's sound: a three-note rising chime, C6-E6-G6, each note built the way a press tone is.
//
// **NOT the shipping firmware's boot sound, by the operator's choice (2026-09-22).** That is a
// 1.8 s recording (refs/M5PaperColor-UserDemo/main/assets/boot_sfx.h). It was ported and played on
// this board, then compared by ear against this chime, both on the device and as PC renders of
// this file's exact digital path. The chime won, and the recording and its +318 KB of flash went.
// Ticket 16 has the account. The chime is ~11 dB below the recording's peak at the same gain.
static void play_boot_sound(i2s_chan_handle_t tx)
{
    static const int NOTES[] = {84, 88, 91};
    for (size_t i = 0; i < sizeof(NOTES) / sizeof(NOTES[0]); i++) {
        // The staircase halves the pitch, so ask for an octave up to hear the note named.
        play_tone(tx, midi_hz(NOTES[i] + 12), 0.06f, GAIN_BOOT_Q16);
    }
}

#define BOOT_SOUND_NAME "chime C6-E6-G6"

esp_err_t board_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (s_chunk == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_CODEC_EN_GPIO) | (1ULL << BOARD_SPK_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    enables(false);
    s_ready = true;
    return ESP_OK;
}

void board_audio_play_boot(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2s_chan_handle_t tx;
    if (wait_for_ram() && session_open(&tx) == ESP_OK) {
        play_boot_sound(tx);
        session_close(tx);
    }
    xSemaphoreGive(s_lock);
}

void board_audio_beep(board_button_t button)
{
    if (!s_ready) {
        return;
    }
    // local_photo_slideshow.cpp:388-390: button C (TOP, GPIO1) 119, B (DOWN, GPIO9) 120,
    // A (UP, GPIO10) 121.
    int midi = 119;
    if (button == BOARD_BUTTON_DOWN) {
        midi = 120;
    } else if (button == BOARD_BUTTON_UP) {
        midi = 121;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2s_chan_handle_t tx;
    if (wait_for_ram() && session_open(&tx) == ESP_OK) {
        play_tone(tx, midi_hz(midi), TONE_SECONDS, GAIN_TONE_Q16);
        session_close(tx);
    }
    xSemaphoreGive(s_lock);
}

const char *board_audio_describe(void)
{
    return "ES8311 + speaker on I2S0, powered per sound; boot: " BOOT_SOUND_NAME;
}

#endif // BOARD_M5PAPER_COLOR
