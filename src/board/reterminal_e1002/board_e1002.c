// The reTerminal E1002's implementation of board.h.
//
// WRITTEN WITHOUT THE BOARD. Every register, address, pin and settle time is transcribed from
// refs/esp32-photoframe (MIT, HEAD bf02982), a third party's firmware that ships an E1002
// build -- not Seeed's driver, not a datasheet, and not measured here. It compiles and it has
// never run. Ticket 63 is the bring-up, and until it has run nothing in this file may be cited
// as a fact about the hardware.
//
// What differs from the M5Paper Color, and it is more than pin numbers:
//
//   * NO PM1. There is no power-manager IC mediating rails. The panel has no switchable rail
//     at all, the card's is a plain GPIO, and battery voltage comes from the SoC's own ADC.
//   * TWO I2C buses, because the charger is only fitted from hardware revision V1.2 and lives
//     on its own. The revision cannot be chosen at purchase (operator, 2026-09-15), so it is
//     PROBED. A pre-V1.2 unit carries a non-I2C ETA6003 and costs exactly one thing: it cannot
//     tell external power from battery. Everything else works.
//   * NO CARD DETECT. See board_card_present().
//   * NO BATTERY-BACKED RTC RAM. The PCF8563 has none, so board_state_*() is NVS. Ticket 43
//     seam 4; board.h has the arithmetic.

#include "board.h"

#ifdef BOARD_RETERMINAL_E1002

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "board_e1002";

#define I2C_TIMEOUT_MS 100

// PCF8563 register map, from refs/esp32-photoframe/components/rtc_driver_pcf8563/src/pcf8563.c.
#define PCF8563_REG_VL_SECONDS 0x02 // bit7 = VL, "the time is not trustworthy"
#define PCF8563_VL_BIT 0x80
#define PCF8563_CENTURY_BIT 0x80 // in the months register

static i2c_master_bus_handle_t s_bus;
static i2c_master_bus_handle_t s_charger_bus;
static i2c_master_dev_handle_t s_sht40;
static i2c_master_dev_handle_t s_rtc;
static i2c_master_dev_handle_t s_charger; // NULL on a pre-V1.2 unit

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;   // NULL if the unit has no eFuse calibration
static SemaphoreHandle_t s_bat_lock;   // ticket 73 A1; see battery_mv()
static SemaphoreHandle_t s_sht40_lock; // ticket 76; see sht40_sequence()

// ------------------------------------------------------------------- bring-up

static esp_err_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr,
                        i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static void battery_adc_init(void)
{
    const gpio_config_t en = {
        .pin_bit_mask = 1ULL << BOARD_BAT_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&en);
    // Off by default: the divider is 200k across the cell, and leaving it enabled is a
    // continuous drain on a board meant to run for weeks.
    gpio_set_level(BOARD_BAT_EN_GPIO, 0);

    // ~80 bytes of INTERNAL RAM, which is this project's scarce resource -- small, not free, and
    // the reason ticket 73 chose this scope over a board-wide lock over the SHT40 and the RTC too.
    s_bat_lock = xSemaphoreCreateMutex();
    if (s_bat_lock == NULL) {
        ESP_LOGW(TAG, "no battery mutex; concurrent reads can race (ticket 73)");
    }

    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = BOARD_BAT_ADC_UNIT};
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_new_unit failed; battery voltage unavailable");
        s_adc = NULL;
        return;
    }
    const adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc, BOARD_BAT_ADC_CHANNEL, &chan);

    // Curve fitting is the S3's scheme. Without it the fallback below is a nominal
    // 3300/4095 scaling, which is worse but not useless -- say which one is in force, because
    // a battery reading that is quietly 5 % out reads exactly like a battery that is 5 % down.
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = BOARD_BAT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_adc_cali) != ESP_OK) {
        s_adc_cali = NULL;
    }
    printf("# board: battery ADC on unit %d ch %d, calibration %s\n", (int)BOARD_BAT_ADC_UNIT,
           (int)BOARD_BAT_ADC_CHANNEL, s_adc_cali ? "curve-fitting (eFuse)" : "NONE (nominal)");
}

esp_err_t board_i2c_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }
    err = add_dev(s_bus, SHT40_I2C_ADDR, &s_sht40);
    if (err != ESP_OK) {
        return err;
    }
    err = add_dev(s_bus, PCF8563_I2C_ADDR, &s_rtc);
    if (err != ESP_OK) {
        return err;
    }

    // The charger bus, and the revision probe. A failure here is NOT a failure of board
    // bring-up: on a pre-V1.2 unit there is nothing at 0x6B and that is a supported
    // configuration, so it is logged and the handle stays NULL.
    const i2c_master_bus_config_t chg_cfg = {
        .i2c_port = BOARD_CHARGER_I2C_PORT,
        .sda_io_num = BOARD_CHARGER_I2C_SDA,
        .scl_io_num = BOARD_CHARGER_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&chg_cfg, &s_charger_bus) == ESP_OK) {
        if (i2c_master_probe(s_charger_bus, SY6974B_I2C_ADDR, I2C_TIMEOUT_MS) == ESP_OK &&
            add_dev(s_charger_bus, SY6974B_I2C_ADDR, &s_charger) == ESP_OK) {
            printf("# board: SY6974B charger at 0x%02X -- hardware revision V1.2 or later\n",
                   SY6974B_I2C_ADDR);
        } else {
            s_charger = NULL;
            printf("# board: nothing at 0x%02X on I2C%d -- pre-V1.2 unit with the non-I2C "
                   "ETA6003. External-power detection is unavailable; everything else works\n",
                   SY6974B_I2C_ADDR, BOARD_CHARGER_I2C_PORT);
        }
    } else {
        ESP_LOGW(TAG, "charger I2C%d bus failed to open", BOARD_CHARGER_I2C_PORT);
    }

    battery_adc_init();

    // Ticket 76, and NOT a bus lock -- the IDF driver has one of those already. This guards
    // board_sht40_read()'s write-wait-read sequence, which that lock cannot span.
    s_sht40_lock = xSemaphoreCreateMutex();
    if (s_sht40_lock == NULL) {
        ESP_LOGW(TAG, "no sht40 mutex; concurrent reads can lose a conversion (ticket 76)");
    }
    return ESP_OK;
}

esp_err_t board_i2c_scan(void)
{
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    printf("# I2C0 scan on SDA %d / SCL %d:\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        found++;
        const char *name = "unknown";
        if (addr == SHT40_I2C_ADDR) {
            name = "SHT4x temp/humidity";
        } else if (addr == PCF8563_I2C_ADDR) {
            name = "PCF8563 RTC";
        }
        printf("#   0x%02X  %s\n", addr, name);
    }
    if (s_charger_bus != NULL) {
        printf("# I2C1 scan on SDA %d / SCL %d (charger bus):\n", BOARD_CHARGER_I2C_SDA,
               BOARD_CHARGER_I2C_SCL);
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(s_charger_bus, addr, I2C_TIMEOUT_MS) == ESP_OK) {
                found++;
                printf("#   0x%02X  %s\n", addr,
                       addr == SY6974B_I2C_ADDR ? "SY6974B charger (V1.2+)" : "unknown");
            }
        }
    }
    if (found == 0) {
        printf("#   nothing responded -- neither bus is working\n");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

// ----------------------------------------------------------------- panel power

esp_err_t board_epd_power(bool on)
{
    // NOTHING TO DO, and that is read rather than assumed: the reference board file sets
    // `.pin_enable = -1` for this board, where its E1003 sibling names an EPD_VCC_EN pin. The
    // panel is powered whenever the board is. Returning ESP_OK rather than
    // ESP_ERR_NOT_SUPPORTED is deliberate -- six entry points ESP_ERROR_CHECK() this call, and
    // "there is no rail to raise" is success, not a fault.
    (void)on;
    return ESP_OK;
}

esp_err_t board_epd_power_state(bool *on)
{
    if (on != NULL) {
        *on = true;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- power status

#ifdef BOARD_BAT_DEBUG
// Ticket 73's discriminator, and it is instrumentation rather than a feature: `avg = sum / taken`
// is a derived value in a first-cut instrument, and it is what hid a reading whose eight samples
// disagreed. These two statics are read and written only under this flag, which lives in
// git-ignored platformio_local.ini for the same reason -DFRAME_PAIR_CODE_LOG does.
static uint32_t s_bat_entries;
static int64_t s_bat_last_down_us;
// The print below names its eight fields one at a time, so it is wrong the moment the constant
// moves. Fail the build instead of printing four of six samples.
_Static_assert(BOARD_BAT_SAMPLES == 8, "the bat= debug line prints exactly eight samples");
#endif

static esp_err_t battery_mv(uint16_t *out)
{
    if (s_adc == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Ticket 73 §4 option A1: the narrowest scope that closes the race, held from EN up to EN down
    // and released before the arithmetic and the print. Two tasks reach here -- the display task
    // once per render for the matte band's battery icon, and the HTTP worker per request -- and
    // without this the loser samples a divider node the winner has already dropped. That race was
    // never observed (183 instrumented entries, no two closer than 2 ms), so this closes a
    // certainty about the code rather than a measured fault. `s_bat_lock == NULL` only if the
    // mutex could not be allocated, and an unlocked reading beats no reading at all.
    if (s_bat_lock != NULL) {
        xSemaphoreTake(s_bat_lock, portMAX_DELAY);
    }

    gpio_set_level(BOARD_BAT_EN_GPIO, 1);
#ifdef BOARD_BAT_DEBUG
    const int64_t t_pre = esp_timer_get_time();
#endif
    // **`+ 1` IS THE FIX FOR TICKET 73, and it is the whole of it.** CONFIG_FREERTOS_HZ is 100, so
    // pdMS_TO_TICKS(10) is ONE tick, and a one-tick vTaskDelay() returns at the next tick edge --
    // anywhere from ~0 to 10 ms, depending only on where the call landed inside the tick period.
    // Measured 2026-09-21: 20 of 183 entries settled for under 1 ms, every one of them read low
    // (216-3842 mV against a cell at 4132), and every one showed its eight ADC counts still rising.
    // The extra tick makes the delay 10-20 ms, so the constant's own claim becomes the floor.
    //
    // The constant is now measured rather than guessed: the node is within 1 LSB by ~2 ms (mean
    // 4131.4 mV for a 2-3 ms settle against 4118.3 for 1-2 ms), so 10 ms carries 5x margin.
    vTaskDelay(pdMS_TO_TICKS(BOARD_BAT_SETTLE_MS) + 1);
#ifdef BOARD_BAT_DEBUG
    const int64_t t_post = esp_timer_get_time();
#endif

    int sum = 0;
    int taken = 0;
    // Per-sample rather than into `sum` alone: the average is what hid ticket 73, and the
    // accumulate below reads the array so it stays live in a build without BOARD_BAT_DEBUG.
    int raw_v[BOARD_BAT_SAMPLES];
    for (int i = 0; i < BOARD_BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BOARD_BAT_ADC_CHANNEL, &raw) == ESP_OK) {
            raw_v[i] = raw;
            sum += raw_v[i];
            taken++;
        } else {
            raw_v[i] = -1;
        }
    }
#ifdef BOARD_BAT_DEBUG
    const int64_t t_done = esp_timer_get_time();
#endif
    gpio_set_level(BOARD_BAT_EN_GPIO, 0);

#ifdef BOARD_BAT_DEBUG
    const int64_t gap_us = s_bat_last_down_us > 0 ? t_pre - s_bat_last_down_us : -1;
    s_bat_last_down_us = esp_timer_get_time();
    const uint32_t n = ++s_bat_entries;
#endif

    // Released here rather than at the end: everything below works on locals, and the debug print
    // costs ~10 ms of UART that no other caller should wait behind.
    if (s_bat_lock != NULL) {
        xSemaphoreGive(s_bat_lock);
    }

    if (taken == 0) {
        return ESP_FAIL;
    }
    const int avg = sum / taken;

    float pin_mv;
    if (s_adc_cali != NULL) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_adc_cali, avg, &mv) != ESP_OK) {
            return ESP_FAIL;
        }
        pin_mv = (float)mv;
    } else {
        pin_mv = (float)avg * (3300.0f / 4095.0f);
    }
    *out = (uint16_t)(pin_mv * BOARD_BAT_DIVIDER);

#ifdef BOARD_BAT_DEBUG
    // One line per entry: the counter and the task name say whether two callers overlapped, the
    // two durations say whether the settle actually happened, the eight counts say whether the
    // divider node was still rising while they were taken, and gap_us says how long EN had been
    // low beforehand. No verdict is computed here -- that is analysis's job, where it is free to
    // be wrong twice.
    printf("# bat n=%lu task=%s settle_us=%lld sample_us=%lld gap_us=%lld raw=%d,%d,%d,%d,%d,%d,%d,%d"
           " avg=%d out=%u\n",
           (unsigned long)n, pcTaskGetName(NULL), (long long)(t_post - t_pre),
           (long long)(t_done - t_post), (long long)gap_us, raw_v[0], raw_v[1], raw_v[2], raw_v[3],
           raw_v[4], raw_v[5], raw_v[6], raw_v[7], avg, (unsigned)*out);
    fflush(stdout);
#endif
    return ESP_OK;
}

esp_err_t board_power_read(board_power_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    // vinout_present stays false: it is the PM1's 5 V boost output and this board has no
    // equivalent. board.h says a caller must not read a false field as a measurement.

    if (s_charger != NULL) {
        uint8_t stat = 0;
        const uint8_t reg = SY6974B_REG08_STAT0;
        if (i2c_master_transmit_receive(s_charger, &reg, 1, &stat, 1, I2C_TIMEOUT_MS) ==
            ESP_OK) {
            out->vin_present = (stat & SY6974B_STAT0_PG_BIT) != 0;
            const uint8_t chrg = (uint8_t)((stat >> SY6974B_CHRG_STAT_SHIFT) &
                                           SY6974B_CHRG_STAT_MASK);
            // A charger that is charging implies a cell is connected. It does NOT follow that
            // "not charging" means no cell, so bat_present stays false rather than being
            // guessed -- app_server.c reads vbat_mv, not this field.
            out->bat_present = (chrg != 0);
        }
    }

    return battery_mv(&out->vbat_mv);
}

// --------------------------------------------------------------------- charger

static esp_err_t charger_read_reg(uint8_t reg, uint8_t *value)
{
    if (s_charger == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return i2c_master_transmit_receive(s_charger, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

esp_err_t board_charger_read(board_charger_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_charger == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    // ONE REGISTER PER TRANSACTION, not a 12-byte burst from REG00. Whether this part
    // auto-increments its address pointer is exactly the kind of thing the missing datasheet
    // would say, and a burst that silently returns twelve copies of REG00 would look like a
    // reading rather than like a bug. Twelve transactions at 100 kHz is under 5 ms.
    for (uint8_t i = 0; i < SY6974B_REG_COUNT; i++) {
        const esp_err_t err = charger_read_reg(i, &out->regs[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    const uint8_t stat = out->regs[SY6974B_REG08_STAT0];
    out->power_good = (stat & SY6974B_STAT0_PG_BIT) != 0;
    out->chrg_stat = (uint8_t)((stat >> SY6974B_CHRG_STAT_SHIFT) & SY6974B_CHRG_STAT_MASK);

    const uint8_t vreg = (uint8_t)((out->regs[SY6974B_REG04_VREG] & SY6974B_REG04_VREG_MASK) >>
                                  SY6974B_REG04_VREG_SHIFT);
    out->vreg_mv = (uint16_t)(SY6974B_VREG_BASE_MV + (unsigned)vreg * SY6974B_VREG_STEP_MV);

    return ESP_OK;
}

// The ceiling, captured from the first successful read and never written above. 0 = not yet
// known, which makes every write refuse rather than guess -- board_charger_read() runs at boot
// from frame_main.c's dump, so by the time any policy runs this is set.
static uint16_t s_vreg_ceiling_mv;

static esp_err_t charger_write_reg(uint8_t reg, uint8_t value)
{
    if (s_charger == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_charger, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t board_charger_set_vreg_mv(uint16_t target_mv, uint16_t *applied_mv)
{
    if (s_charger == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t reg04 = 0;
    esp_err_t err = charger_read_reg(SY6974B_REG04_VREG, &reg04);
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t current_mv =
        (uint16_t)(SY6974B_VREG_BASE_MV +
                   (unsigned)((reg04 & SY6974B_REG04_VREG_MASK) >> SY6974B_REG04_VREG_SHIFT) *
                       SY6974B_VREG_STEP_MV);
    if (s_vreg_ceiling_mv == 0 || current_mv > s_vreg_ceiling_mv) {
        // The first value seen is the factory default; a later HIGHER reading means the part was
        // reset under us (its I2C watchdog is the suspect), so the ceiling follows it up rather
        // than locking us out of restoring the default afterwards.
        s_vreg_ceiling_mv = current_mv;
    }
    if (target_mv > s_vreg_ceiling_mv) {
        target_mv = s_vreg_ceiling_mv;
    }
    if (target_mv < SY6974B_VREG_BASE_MV) {
        target_mv = SY6974B_VREG_BASE_MV;
    }

    // DOWN, never up: a rounded-up target is a higher voltage than was asked for, which is the
    // direction that matters on a cell.
    const uint8_t steps = (uint8_t)((target_mv - SY6974B_VREG_BASE_MV) / SY6974B_VREG_STEP_MV);
    const uint8_t want = (uint8_t)((reg04 & (uint8_t)~SY6974B_REG04_VREG_MASK) |
                                   (uint8_t)(steps << SY6974B_REG04_VREG_SHIFT));

    if (want != reg04) {
        err = charger_write_reg(SY6974B_REG04_VREG, want);
        if (err != ESP_OK) {
            return err;
        }
    }

    uint8_t got = 0;
    err = charger_read_reg(SY6974B_REG04_VREG, &got);
    if (err != ESP_OK) {
        return err;
    }
    const uint16_t got_mv =
        (uint16_t)(SY6974B_VREG_BASE_MV +
                   (unsigned)((got & SY6974B_REG04_VREG_MASK) >> SY6974B_REG04_VREG_SHIFT) *
                       SY6974B_VREG_STEP_MV);
    if (applied_mv != NULL) {
        *applied_mv = got_mv;
    }
    if (got != want) {
        ESP_LOGW(TAG, "charger REG04 read back 0x%02X after writing 0x%02X (%u mV, wanted %u)",
                 got, want, (unsigned)got_mv, (unsigned)target_mv);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t board_charger_watchdog_disable(void)
{
    if (s_charger == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t reg05 = 0;
    esp_err_t err = charger_read_reg(SY6974B_REG05_TIMER, &reg05);
    if (err != ESP_OK) {
        return err;
    }
    if ((reg05 & SY6974B_REG05_WATCHDOG_MASK) == 0) {
        return ESP_OK;
    }
    const uint8_t want = (uint8_t)(reg05 & (uint8_t)~SY6974B_REG05_WATCHDOG_MASK);
    err = charger_write_reg(SY6974B_REG05_TIMER, want);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t got = 0;
    err = charger_read_reg(SY6974B_REG05_TIMER, &got);
    if (err != ESP_OK) {
        return err;
    }
    if ((got & SY6974B_REG05_WATCHDOG_MASK) != 0) {
        ESP_LOGW(TAG, "charger REG05 still 0x%02X after writing 0x%02X; watchdog not disabled", got,
                 want);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void board_charger_dump(void)
{
    board_charger_t c;
    const esp_err_t err = board_charger_read(&c);
    if (err != ESP_OK) {
        printf("# charger: not readable (%s)\n", esp_err_to_name(err));
        return;
    }
    printf("# charger: REG00-0B");
    for (int i = 0; i < SY6974B_REG_COUNT; i++) {
        printf(" %02X", c.regs[i]);
    }
    printf("\n");
    // Raw fields beside the decode, so the log can settle the REG08 question without a rebuild:
    // whichever of [4:3], [1:0] and [5:4] tracks the cell's actual state is the right one.
    printf("# charger: reg08=0x%02X chrg_stat[4:3]=%u alt[1:0]=%u alt[5:4]=%u pg=%d\n",
           c.regs[SY6974B_REG08_STAT0], (unsigned)c.chrg_stat,
           (unsigned)(c.regs[SY6974B_REG08_STAT0] & 0x03u),
           (unsigned)((c.regs[SY6974B_REG08_STAT0] >> 4) & 0x03u), c.power_good ? 1 : 0);
    printf("# charger: reg04=0x%02X vreg=%u mV (decode provisional) reg01=0x%02X chg_en=%d otg=%d\n",
           c.regs[SY6974B_REG04_VREG], (unsigned)c.vreg_mv, c.regs[SY6974B_REG01_PWRON],
           (c.regs[SY6974B_REG01_PWRON] & SY6974B_REG01_CHG_CONFIG_BIT) ? 1 : 0,
           (c.regs[SY6974B_REG01_PWRON] & SY6974B_REG01_OTG_CONFIG_BIT) ? 1 : 0);
}

// ------------------------------------------------------------------ microSD rail

static bool s_card_powered;

esp_err_t board_card_power(bool on)
{
    static bool configured;
    if (!configured) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BOARD_CARD_POWER_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        const esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            return err;
        }
        configured = true;
    }
    gpio_set_level(BOARD_CARD_POWER_GPIO, on ? 1 : 0);
    if (on && !s_card_powered) {
        vTaskDelay(pdMS_TO_TICKS(BOARD_CARD_POWER_SETTLE_MS));
    }
    s_card_powered = on;
    return ESP_OK;
}

bool board_card_present(void)
{
    // A DETECT LINE DOES EXIST -- ESP_IO15/SD_DET on Seeed's V1.2 schematic, read 2026-09-17.
    // This function was written the other way round because the reference's board header names
    // every other pin including the card's power rail and omits this one, so its silence was read
    // as the board's. It is not wired up yet and this still returns TRUE, "a card may be fitted, I
    // cannot tell", because the polarity is unmeasured: env:bringup_e1002 prints the level with a
    // card fitted, and the other reading needs a hand at the slot.
    //
    // That is the safe answer and not the convenient one, and the reason is in
    // board_storage.c. board_storage_select() tries the card regardless of this value and
    // falls back, so boot is correct either way. board_storage_ensure() targets the card only
    // when this is true -- so a constant false would mean the frame NEVER used a fitted card,
    // and would additionally clear sd_fallback_locked on every pass, re-running the
    // three-second frequency ladder forever. A constant true costs one thing instead: a card
    // inserted into a running frame is never noticed, because frame_main.c's poll acts on a
    // CHANGE. Removal is still caught, by the read failures that follow it.
    //
    // GPIO15 is where that change goes when both readings exist.
    return true;
}

// ---------------------------------------------------------------------- sensors

// Split out so the mutex in board_sht40_read() has one release point. Ticket 76: this sequence is
// open for ~20 ms with the bus released in the middle, and the IDF driver's own per-bus lock
// (s_i2c_synchronous_transaction(), esp_driver_i2c/i2c_master.c:930) covers a transaction and not a
// sequence. Two tasks call it -- the display task per render for the matte band, the HTTP worker for
// GET /api/system/info -- and the loser of an overlap gets a NACK and no reading. Measured on the
// M5Paper Color, whose code here is identical; not separately measured on this board, where the
// storage lock happens to space the two callers apart (ticket 76 §5.1).
static esp_err_t sht40_sequence(uint8_t *rx, size_t rx_len)
{
    const uint8_t cmd = 0xFD; // high-precision measurement
    esp_err_t err = i2c_master_transmit(s_sht40, &cmd, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    // Two ticks, not one -- the same arithmetic as board_pm1.c's copy of this, where the fault was
    // measured: at CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(10) waits 0-10 ms against a conversion
    // that needs 8.2 ms. Changed here unmeasured, because it is the identical bug in the identical
    // sequence on the identical part; the E1002's own confirmation is one Refresh on its System
    // panel.
    vTaskDelay(pdMS_TO_TICKS(20));

    return i2c_master_receive(s_sht40, rx, rx_len, I2C_TIMEOUT_MS);
}

esp_err_t board_sht40_read(float *temp_c, float *humidity_pct)
{
    if (s_sht40 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t rx[6];

    if (s_sht40_lock != NULL) {
        xSemaphoreTake(s_sht40_lock, portMAX_DELAY);
    }
    const esp_err_t err = sht40_sequence(rx, sizeof(rx));
    if (s_sht40_lock != NULL) {
        xSemaphoreGive(s_sht40_lock);
    }
    if (err != ESP_OK) {
        // Said out loud: this was silent, and a silent sensor failure is how the one-tick wait of
        // 2026-09-18 came to be diagnosed from a route's missing JSON fields rather than from here.
        ESP_LOGW(TAG, "sht40 read failed: %s", esp_err_to_name(err));
        return err;
    }
    // Datasheet conversions, the same part as the M5Paper Color's. CRC bytes (rx[2], rx[5])
    // are not checked -- this is a sanity cross-check, not a control input.
    const uint16_t t_ticks = (uint16_t)((rx[0] << 8) | rx[1]);
    const uint16_t rh_ticks = (uint16_t)((rx[3] << 8) | rx[4]);
    if (temp_c != NULL) {
        *temp_c = -45.0f + 175.0f * ((float)t_ticks / 65535.0f);
    }
    if (humidity_pct != NULL) {
        *humidity_pct = -6.0f + 125.0f * ((float)rh_ticks / 65535.0f);
    }
    return ESP_OK;
}

// -------------------------------------------------------------------------- RTC

static int bcd_to_int(uint8_t v)
{
    const int hi = (v >> 4) & 0x0F;
    const int lo = v & 0x0F;
    return (hi > 9 || lo > 9) ? -1 : hi * 10 + lo;
}

esp_err_t board_rtc_read(board_datetime_t *dt)
{
    if (s_rtc == NULL || dt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t reg = PCF8563_REG_VL_SECONDS;
    uint8_t r[7] = {0};
    const esp_err_t err =
        i2c_master_transmit_receive(s_rtc, &reg, 1, r, sizeof(r), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    // VL says the oscillator stopped at some point, so the time may be nonsense. Refuse rather
    // than report a plausible-looking wrong time -- the same discipline board_pm1.c's BCD
    // plausibility check applies, and for the same reason: a wrong clock that looks right is
    // worse than no clock.
    if (r[0] & PCF8563_VL_BIT) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int sec = bcd_to_int(r[0] & 0x7F);
    const int min = bcd_to_int(r[1] & 0x7F);
    const int hour = bcd_to_int(r[2] & 0x3F);
    const int day = bcd_to_int(r[3] & 0x3F);
    const int month = bcd_to_int(r[5] & 0x1F);
    const int year2 = bcd_to_int(r[6]);
    if (sec < 0 || min < 0 || hour < 0 || day < 0 || month < 0 || year2 < 0 || sec > 59 ||
        min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    dt->second = sec;
    dt->minute = min;
    dt->hour = hour;
    dt->day = day;
    dt->month = month;
    // The century bit is 0 for the 2000s. board_datetime_t only spans 2000-2099, so a set bit
    // is out of range and reads as an invalid response rather than silently landing in 1900.
    dt->year = (r[5] & PCF8563_CENTURY_BIT) ? -1 : 2000 + year2;
    return (dt->year < 0) ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

esp_err_t board_rtc_seconds(int64_t *secs)
{
    board_datetime_t dt;
    const esp_err_t err = board_rtc_read(&dt);
    if (err != ESP_OK) {
        return err;
    }
    if (secs != NULL) {
        *secs = (int64_t)dt.hour * 3600 + (int64_t)dt.minute * 60 + dt.second;
    }
    return ESP_OK;
}

// ------------------------------------------------------ small persistent state
//
// NVS, because the PCF8563 has no general-purpose RAM. One key per byte, deliberately: ticket
// 39 found NVS commits PER KEY, so writing only the byte that changed is what keeps the cost
// at the ~288 single-key commits a day board.h quotes rather than four times that.
//
// Opened and closed per call. Four bytes written on navigation is not a hot path, and a handle
// held open across the application's life is state that outlives its usefulness.

#define STATE_NAMESPACE "boardstate"

static void state_key(uint8_t index, char out[8])
{
    snprintf(out, 8, "s%u", (unsigned)index);
}

esp_err_t board_state_write(uint8_t index, uint8_t value)
{
    if (index >= BOARD_STATE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(STATE_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    state_key(index, key);

    // Read first and skip an unchanged write. NVS does not promise this for us, and the whole
    // reason this store is four bytes rather than a struct is that a write costs flash.
    uint8_t cur = 0;
    if (nvs_get_u8(h, key, &cur) == ESP_OK && cur == value) {
        nvs_close(h);
        return ESP_OK;
    }
    err = nvs_set_u8(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t board_state_read(uint8_t index, uint8_t *value)
{
    if (index >= BOARD_STATE_BYTES || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(STATE_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // A namespace that has never been written is ESP_ERR_NVS_NOT_FOUND. Callers treat any
        // error as "no stored value" and start from zero, which is what a first boot means --
        // app_slideshow.c's own fallback, unchanged.
        return err;
    }
    char key[8];
    state_key(index, key);
    err = nvs_get_u8(h, key, value);
    nvs_close(h);
    return err;
}

#endif // BOARD_RETERMINAL_E1002
