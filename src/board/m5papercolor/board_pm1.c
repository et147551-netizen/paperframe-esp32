#include "board_pm1.h"

#ifdef BOARD_M5PAPER_COLOR

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pm1";

// One number, two headers. See board_pm1.h's note on RX8130_RAM_SIZE.
_Static_assert(BOARD_STATE_BYTES == RX8130_RAM_SIZE,
               "BOARD_STATE_BYTES must equal the RTC's usable RAM on this board");

#define I2C_TIMEOUT_MS 100

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pm1;
static i2c_master_dev_handle_t s_sht40;

// Ticket 76. NOT a bus lock: the IDF i2c_master driver already has one of those and takes it per
// transaction (s_i2c_synchronous_transaction(), esp_driver_i2c/i2c_master.c:930), so the PM1's
// register reads and the RTC's are atomic without help. This guards the one thing that lock cannot
// -- board_sht40_read()'s write-wait-read SEQUENCE, which is open for ~20 ms with the bus released
// in the middle. See the comment at the function.
static SemaphoreHandle_t s_sht40_lock;

esp_err_t board_i2c_init(void)
{
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

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PM1_I2C_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_pm1);
    if (err != ESP_OK) {
        return err;
    }

    dev_cfg.device_address = SHT40_I2C_ADDR;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_sht40);
    if (err != ESP_OK) {
        return err;
    }

    s_sht40_lock = xSemaphoreCreateMutex();
    if (s_sht40_lock == NULL) {
        ESP_LOGW(TAG, "no sht40 mutex; concurrent reads can lose a conversion (ticket 76)");
    }
    return ESP_OK;
}

i2c_master_bus_handle_t board_pm1_bus(void)
{
    return s_bus;
}

esp_err_t board_pm1_write_reg(uint8_t reg, uint8_t value)
{
#ifdef BOARD_NO_POWER_OFF
    // The one write this driver will not make. See the note in board_pm1.h -- a
    // power-off is unrecoverable without a finger on the button, so it is refused
    // here rather than left to whoever is reading the register map at the time.
    if (reg == PM1_REG_SYS_CMD) {
        ESP_LOGE(TAG, "refusing SYS_CMD write (0x%02X): BOARD_NO_POWER_OFF is set",
                 value);
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_pm1, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t board_pm1_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pm1, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

// Two bytes, little-endian, straight millivolts -- no scaling
// (M5PM1_Class.cpp:331-338).
static esp_err_t pm1_read_u16(uint8_t reg_low, uint16_t *out)
{
    uint8_t buf[2] = {0};
    const esp_err_t err =
        i2c_master_transmit_receive(s_pm1, &reg_low, 1, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *out = (uint16_t)((buf[1] << 8) | buf[0]);
    }
    return err;
}

esp_err_t board_power_read(board_power_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t src = 0;
    esp_err_t err = board_pm1_read_reg(PM1_REG_PWR_SRC, &src);
    if (err != ESP_OK) {
        return err;
    }
    out->vin_present = (src & 0x01) != 0;
    out->vinout_present = (src & 0x02) != 0;
    out->bat_present = (src & 0x04) != 0;

    err = pm1_read_u16(PM1_REG_VBAT_L, &out->vbat_mv);
    if (err != ESP_OK) {
        return err;
    }
    return pm1_read_u16(PM1_REG_VIN_L, &out->vin_mv);
}

// ----------------------------------------------------------------------- charger
//
// **Permanently unsupported on this board, and that is copper rather than missing work.** The
// PM1 does carry a charge-enable -- PWR_CFG (0x06) bit 0, R/W, and the shipping firmware writes
// it -- but pin 2 CHG_EN_PP is a no-connect on this PCB, the IP2315's own SCL/SDA (pins 8/9) are
// crossed out too, and the charge current is set by a resistor on ICHGSET. There is also no
// status to read: LED3 goes to an unfitted R14. And it would not help if all of that were
// wired, because VBAT and SYS_VBUS are the same node through R20, a 0 ohm link -- stopping the
// charge here would start a discharge. Ticket 71 §1 and §8.1; docs/board-pinmap.md §"Charging".
//
// So this is NOT a stub awaiting an implementation. Anyone tempted to write PWR_CFG bit 0 should
// read ticket 71 first: that write succeeds and changes nothing, which is how it misleads.
esp_err_t board_charger_read(board_charger_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void board_charger_dump(void)
{
    printf("# charger: none reachable on this board -- CHG_EN unconnected, charger I2C unwired, "
           "no power path (ticket 71)\n");
}

esp_err_t board_charger_set_vreg_mv(uint16_t target_mv, uint16_t *applied_mv)
{
    (void)target_mv;
    if (applied_mv != NULL) {
        *applied_mv = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_charger_watchdog_disable(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

// Two bits per pin. Pins 0-3 live in GPIO_FUNC0 / GPIO_PUPD0; pin 4 lives in the
// [1:0] field of GPIO_FUNC1 / GPIO_PUPD1. Everything else in this block is one bit
// per pin in a single register.
static esp_err_t pm1_set_field2(uint8_t reg_low, uint8_t reg_high, uint8_t pin,
                                uint8_t value)
{
    const uint8_t reg = (pin < 4) ? reg_low : reg_high;
    const uint8_t shift = (uint8_t)((pin < 4) ? (pin * 2) : 0);
    uint8_t cur;

    const esp_err_t err = board_pm1_read_reg(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    cur = (uint8_t)((cur & ~(0x03u << shift)) | ((value & 0x03u) << shift));
    return board_pm1_write_reg(reg, cur);
}

static esp_err_t pm1_set_bit(uint8_t reg, uint8_t pin, bool set)
{
    const uint8_t bit = (uint8_t)(1u << pin);
    uint8_t cur;

    const esp_err_t err = board_pm1_read_reg(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    return board_pm1_write_reg(reg, set ? (uint8_t)(cur | bit) : (uint8_t)(cur & ~bit));
}

esp_err_t board_pm1_gpio_output(uint8_t pin, bool level)
{
    if (pin > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    // Each of these is a separate transaction on purpose. The PM1's block transfers
    // may not cross its documented address ranges (0x00-0x0C, 0x10-0x19, 0x20-0x2A,
    // ...), so a single burst across 0x16 and 0x10 would be invalid.

    // 1. Function bits = 00, selecting plain GPIO. Nothing below takes effect until
    //    this is done.
    esp_err_t err = pm1_set_field2(PM1_REG_GPIO_FUNC0, PM1_REG_GPIO_FUNC1, pin, 0);
    if (err != ESP_OK) {
        return err;
    }
    // 2. Direction: output.
    err = pm1_set_bit(PM1_REG_GPIO_MODE, pin, true);
    if (err != ESP_OK) {
        return err;
    }
    // 3. Push-pull drive (GPIO_DRV defaults to 0x1F, i.e. all open-drain).
    err = pm1_set_bit(PM1_REG_GPIO_DRV, pin, false);
    if (err != ESP_OK) {
        return err;
    }
    // 4. Level.
    return pm1_set_bit(PM1_REG_GPIO_OUT, pin, level);
}

esp_err_t board_pm1_gpio_input(uint8_t pin, pm1_pull_t pull)
{
    if (pin > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pm1_set_field2(PM1_REG_GPIO_FUNC0, PM1_REG_GPIO_FUNC1, pin, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = pm1_set_bit(PM1_REG_GPIO_MODE, pin, false); // direction: input
    if (err != ESP_OK) {
        return err;
    }
    return pm1_set_field2(PM1_REG_GPIO_PUPD0, PM1_REG_GPIO_PUPD1, pin,
                          (uint8_t)pull);
}

esp_err_t board_pm1_gpio_read_all(uint8_t *bits)
{
    uint8_t reg;
    const esp_err_t err = board_pm1_read_reg(PM1_REG_GPIO_IN, &reg);
    if (err == ESP_OK && bits != NULL) {
        *bits = (uint8_t)(reg & 0x1Fu);
    }
    return err;
}

esp_err_t board_pm1_gpio_read(uint8_t pin, bool *high)
{
    if (pin > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t bits;
    const esp_err_t err = board_pm1_gpio_read_all(&bits);
    if (err == ESP_OK && high != NULL) {
        *high = (bits & (1u << pin)) != 0;
    }
    return err;
}

esp_err_t board_epd_power(bool on)
{
    return board_pm1_gpio_output(PM1_GPIO_EPD_POWER, on);
}

esp_err_t board_epd_power_state(bool *on)
{
    uint8_t reg;
    const esp_err_t err = board_pm1_read_reg(PM1_REG_GPIO_OUT, &reg);
    if (err == ESP_OK && on != NULL) {
        *on = (reg & (1u << PM1_GPIO_EPD_POWER)) != 0;
    }
    return err;
}

// Resolved by board_i2c_scan(); 0 means "not found yet".
static uint8_t s_rtc_addr;

esp_err_t board_i2c_scan(void)
{
    printf("# I2C scan on SDA %d / SCL %d:\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        found++;
        const char *name = "unknown";
        if (addr == PM1_I2C_ADDR) {
            name = "M5PM1 power manager";
        } else if (addr == SHT40_I2C_ADDR) {
            name = "SHT40 temp/humidity";
        } else if (addr == RX8130_ADDR_CANDIDATE_A ||
                   addr == RX8130_ADDR_CANDIDATE_B) {
            name = "RX8130CE RTC";
            s_rtc_addr = addr;
        }
        printf("#   0x%02X  %s\n", addr, name);
    }
    if (found == 0) {
        printf("#   nothing responded -- the bus is not working\n");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_rtc_addr == 0) {
        printf("#   no RTC at 0x%02X or 0x%02X; wall-clock provenance disabled\n",
               RX8130_ADDR_CANDIDATE_A, RX8130_ADDR_CANDIDATE_B);
    }
    return ESP_OK;
}

// One short-lived device handle per access, following board_rtc_read(): the RTC is not
// on the hot path and adding a permanent handle for four bytes is not worth the state.
static esp_err_t rtc_device(i2c_master_dev_handle_t *out)
{
    if (s_rtc_addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_rtc_addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, out);
}

esp_err_t board_state_write(uint8_t index, uint8_t value)
{
    if (index >= RX8130_RAM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t rtc;
    esp_err_t err = rtc_device(&rtc);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t buf[2] = {(uint8_t)(RX8130_RAM_BASE + index), value};
    err = i2c_master_transmit(rtc, buf, sizeof(buf), I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(rtc);
    return err;
}

esp_err_t board_state_read(uint8_t index, uint8_t *value)
{
    if (index >= RX8130_RAM_SIZE || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t rtc;
    esp_err_t err = rtc_device(&rtc);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t reg = (uint8_t)(RX8130_RAM_BASE + index);
    err = i2c_master_transmit_receive(rtc, &reg, 1, value, 1, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(rtc);
    return err;
}

static int bcd_to_int(uint8_t v)
{
    const int hi = (v >> 4) & 0x0F;
    const int lo = v & 0x0F;
    if (hi > 9 || lo > 9) {
        return -1; // not valid BCD
    }
    return hi * 10 + lo;
}

esp_err_t board_rtc_read(board_datetime_t *dt)
{
    if (s_rtc_addr == 0 || dt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t rtc;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_rtc_addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &rtc);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t reg = 0x10; // SEC. UNVERIFIED -- see the header.
    uint8_t rx[7];
    err = i2c_master_transmit_receive(rtc, &reg, 1, rx, sizeof(rx), I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(rtc);
    if (err != ESP_OK) {
        return err;
    }

    const int sec = bcd_to_int(rx[0] & 0x7F);
    const int min = bcd_to_int(rx[1] & 0x7F);
    const int hour = bcd_to_int(rx[2] & 0x3F);
    const int day = bcd_to_int(rx[4] & 0x3F);
    const int month = bcd_to_int(rx[5] & 0x1F);
    const int year = bcd_to_int(rx[6]);

    // If the register map is wrong, this is where it announces itself. Better a hard
    // error than a plausible-looking wrong timestamp on every row of a 5-hour sweep.
    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23 ||
        day < 1 || day > 31 || month < 1 || month > 12 || year < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    dt->second = sec;
    dt->minute = min;
    dt->hour = hour;
    dt->day = day;
    dt->month = month;
    dt->year = 2000 + year;
    return ESP_OK;
}

esp_err_t board_rtc_seconds(int64_t *secs)
{
    board_datetime_t dt;
    const esp_err_t err = board_rtc_read(&dt);
    if (err == ESP_OK && secs != NULL) {
        *secs = (int64_t)dt.hour * 3600 + dt.minute * 60 + dt.second;
    }
    return err;
}

// The three bus operations, split out so the mutex below has exactly one release point. Ticket 76:
// two tasks call board_sht40_read() -- the display task once per render for the matte band's climate
// string, and the HTTP worker for GET /api/system/info -- and this sequence is open for ~20 ms with
// the bus released in the middle. A second 0xFD arriving inside another task's conversion is NACKed,
// so the loser's read fails and `temp_c` is absent from its response. **Measured on this board
// 2026-09-21: 6 of 3,037 probes over 20 renders, against 0 of 400 with no render in flight.**
static esp_err_t sht40_sequence(uint8_t *rx, size_t rx_len)
{
    const uint8_t cmd = 0xFD; // high-precision measurement
    esp_err_t err = i2c_master_transmit(s_sht40, &cmd, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    // The conversion takes ~8.2 ms and THE WAIT HAS TO BE TWO TICKS, not one. At
    // CONFIG_FREERTOS_HZ=100 a tick is 10 ms, so pdMS_TO_TICKS(10) is vTaskDelay(1), which sleeps
    // until the next tick EDGE -- anywhere from ~0 to 10 ms depending on where the call landed in
    // the tick. Under 8.2 ms the receive below finds no result and fails.
    //
    // Measured on hardware 2026-09-18, when GET /api/system/info became the first caller to read
    // this sensor repeatedly: 1 of 8 requests returned a temperature and the other 7 came back
    // with the fields absent. Every earlier caller reads it once at boot or once every few
    // seconds with a console write in between, which is why a wait that is right less than half
    // the time went unnoticed since the sensor was added. 20 ms is 10-20 ms of real wait.
    vTaskDelay(pdMS_TO_TICKS(20));

    return i2c_master_receive(s_sht40, rx, rx_len, I2C_TIMEOUT_MS);
}

esp_err_t board_sht40_read(float *temp_c, float *humidity_pct)
{
    uint8_t rx[6];

    if (s_sht40_lock != NULL) {
        xSemaphoreTake(s_sht40_lock, portMAX_DELAY);
    }
    const esp_err_t err = sht40_sequence(rx, sizeof(rx));
    if (s_sht40_lock != NULL) {
        xSemaphoreGive(s_sht40_lock);
    }
    if (err != ESP_OK) {
        // SAID OUT LOUD, because it was silent before and that is how the one-tick wait of
        // 2026-09-18 came to be diagnosed from a route's missing JSON fields instead of from the
        // sensor. A failure here is rare enough that a WARN costs nothing and names itself.
        ESP_LOGW(TAG, "sht40 read failed: %s", esp_err_to_name(err));
        return err;
    }

    // Datasheet conversions. CRC bytes (rx[2], rx[5]) are not checked -- this is a
    // sanity cross-check, not a control input.
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

// ------------------------------------------------------------------ microSD rail
//
// These two were inside board_storage.c until the layering pass, which made that file --
// otherwise generic ESP-IDF mount, frequency-ladder and medium-switch logic that travels
// unchanged -- the only reason board_storage.c had to know what a PM1 was.

static bool s_card_powered;

esp_err_t board_card_power(bool on)
{
    const esp_err_t err = board_pm1_gpio_output(PM1_GPIO_SD_POWER, on);
    if (err == ESP_OK) {
        if (on && !s_card_powered) {
            vTaskDelay(pdMS_TO_TICKS(BOARD_CARD_POWER_SETTLE_MS));
        }
        s_card_powered = on;
    }
    return err;
}

bool board_card_present(void)
{
    // Arm the detect, then read it. "low = present" is transcribed from hal.cpp:481-483 and
    // was confirmed against a fitted card on 2026-09-04: the two armed conditions read GPIO1
    // low with a card in and high without one.
    if (board_pm1_gpio_output(PM1_GPIO_SD_DET_EN, true) != ESP_OK) {
        return false;
    }
    if (board_pm1_gpio_input(PM1_GPIO_SD_DETECT, PM1_PULL_UP) != ESP_OK) {
        return false;
    }
    bool high = true;
    if (board_pm1_gpio_read(PM1_GPIO_SD_DETECT, &high) != ESP_OK) {
        return false;
    }
    return !high;
}

#endif // BOARD_M5PAPER_COLOR
