// Ticket 18: where does the panel's reply to a register read actually arrive?
//
// epd_read_temperature() has never returned a real number. It returned 0 C in every
// bring-up run and -1 C from env:storage, which are the decodes of 0x00 and 0xFF --
// the two values an undriven line takes, not two temperatures.
//
// Three candidate causes, and this firmware separates them instead of choosing one:
//
//   pin     the driver reads MISO (GPIO14), but the schematic's J5 table
//           (reference-source-review.md 12.4) gives the panel one data pin, SI0 =
//           GPIO13 = MOSI. Variants A/B read the same command on each.
//   state   the module manual marks SPI invalid until RST_N has risen, and nothing
//           has ever reset the panel before a temperature read. Hence three phases.
//   sensor  TSE, or the internal sensor itself. Variant C reads FLG (0x71), a second
//           readable register that does not involve the sensor at all, so a dead
//           mechanism and a dead sensor stop looking alike.
//
// It prints raw hex and never a temperature. A derived verdict in first-cut
// instrumentation is a second thing that can be wrong, and on this board finding out
// costs a rebuild, a flash and another run.

#ifdef BUILD_TEMPPROBE

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "board_spi.h"
#include "epd_panel.h"
#include "trace.h"

static const char *TAG = "tempprobe";

#define PROBE_SAMPLES 10

// Interleaved by phase rather than run in blocks, so a slow drift in the room or in
// the panel cannot masquerade as a difference between variants: every variant is
// sampled within a few milliseconds of every other one.
static void probe_phase(const char *phase)
{
    for (int i = 0; i < PROBE_SAMPLES; i++) {
        epd_probe_sample_t s;
        const esp_err_t err = epd_probe_sample(0, &s);

        float air_c = 0.0f;
        float rh = 0.0f;
        const bool sht_ok = (board_sht40_read(&air_c, &rh) == ESP_OK);

        // One line per sample, one column per variant, so the log is analysable by
        // column without the firmware deciding anything.
        printf("S,%s,%d,%s,%02X%02X,%02X%02X,%02X%02X,%02X%02X,%02X%02X,",
               phase, i, esp_err_to_name(err),
               s.tsc_miso[0], s.tsc_miso[1],
               s.tsc_3wire[0], s.tsc_3wire[1],
               s.flg_3wire[0], s.flg_3wire[1],
               s.tsc_3wire_no_tse[0], s.tsc_3wire_no_tse[1],
               s.flg_miso[0], s.flg_miso[1]);
        if (sht_ok) {
            printf("%.2f,%.1f\n", air_c, rh);
        } else {
            printf(",\n");
        }
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    printf("\n# ticket 18 temperature read probe\n");
    printf("# columns: S,phase,n,err,A_tsc_miso,B_tsc_3wire,C_flg_3wire,"
           "D_tsc_3wire_no_tse,E_flg_miso,sht40_c,sht40_rh\n");
    printf("# every value is the raw two bytes of the read, hex, undecoded\n");

    trace_init();
    ESP_ERROR_CHECK(board_i2c_init());

    ESP_ERROR_CHECK(board_epd_power(true));
    bool powered = false;
    ESP_ERROR_CHECK(board_epd_power_state(&powered));
    printf("# epd rail readback: %s\n", powered ? "on" : "OFF");

    board_power_t pwr = {0};
    if (board_power_read(&pwr) == ESP_OK) {
        printf("# power: vin=%d (%u mV) vinout=%d bat=%d (%u mV)\n",
               (int)pwr.vin_present, (unsigned)pwr.vin_mv, (int)pwr.vinout_present,
               (int)pwr.bat_present, (unsigned)pwr.vbat_mv);
        if (!pwr.vin_present) {
            printf("# WARNING: running on battery. Do not start a long run.\n");
        }
    } else {
        printf("# power: PM1 did not answer\n");
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(board_spi_init());
    ESP_ERROR_CHECK(epd_init());

    // Phase 1 reproduces the historical condition exactly: rail on, device added,
    // no reset. This is the state every 0x00 and 0xFF reading was taken in.
    probe_phase("power");

    esp_err_t err = epd_probe_reset();
    printf("# reset: %s\n", esp_err_to_name(err));
    probe_phase("reset");

    err = epd_probe_configure(EPD_FRS_STOCK);
    printf("# configure (init list + TRES, no PON): %s\n", esp_err_to_name(err));
    probe_phase("config");

    // The public API, not the probe path: this is what every entry point calls,
    // and it is the thing that must not return a number the panel did not give.
    // It also exercises spi_device_acquire_bus(), which the probe path skips.
    printf("R,n,err,temp_c\n");
    for (int i = 0; i < 10; i++) {
        int temp_c = -999;
        const esp_err_t rerr = epd_read_temperature(0, &temp_c);
        printf("R,%d,%s,%d\n", i, esp_err_to_name(rerr), temp_c);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // The first read of a phase was stale twice: 0x0000 at power-on before any
    // conversion existed, and the previous phase's 0x2580 after a 300 s idle. So TSE
    // starts a conversion and TSC returns whatever the last one produced. Stepping
    // TSE's offset field by a known +7 C says how long the new one takes, without
    // needing the panel's temperature to be moving.
    printf("# settle sweep: TSE offset stepped 0 -> +7 -> 0, TSC read after N ms\n");
    printf("D,settle_ms,at_zero,at_plus7,back_to_zero\n");
    static const uint32_t settle_ms[] = {0, 1, 2, 5, 10, 20, 50, 100, 200, 500};
    for (size_t i = 0; i < sizeof(settle_ms) / sizeof(settle_ms[0]); i++) {
        uint8_t a[2] = {0}, b[2] = {0}, c[2] = {0};
        const uint32_t d = settle_ms[i];
        esp_err_t e1 = epd_probe_offset_step(0, d, a);
        esp_err_t e2 = epd_probe_offset_step(7, d, b);
        esp_err_t e3 = epd_probe_offset_step(0, d, c);
        printf("D,%u,%02X%02X,%02X%02X,%02X%02X,%s\n", (unsigned)d,
               a[0], a[1], b[0], b[1], c[0], c[1],
               (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) ? "ok" : "ERR");
        fflush(stdout);
    }


    // TSE +7 moved the reading by +3.5 C, not +7 C, and the low byte carries 0x80
    // at half degrees. Both fit one model: TSC is signed Q8.8 degrees and TSE's
    // TO[3:0] steps in units of 0.5 C. That is inferred from a single offset, so
    // here is the whole ladder -- if the model holds, raw rises by 0x0080 per step
    // and the negative codes 0x08..0x0F come back below the offset-0 reading.
    printf("# TSE offset ladder, raw TSC after each\n");
    printf("L,offset_steps,raw\n");
    static const int ladder[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -4, -8, 0};
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        uint8_t raw[2] = {0};
        // Two reads: the first settles any gap effect, the second is the datum.
        epd_probe_offset_step(ladder[i], 0, raw);
        const esp_err_t e = epd_probe_offset_step(ladder[i], 0, raw);
        printf("L,%d,%02X%02X,%s\n", ladder[i], raw[0], raw[1],
               esp_err_to_name(e));
        fflush(stdout);
    }

    // The settle sweep ruled out conversion time: a TSE offset step shows up in
    // the very next TSC read with zero delay, while reads are flowing. So the
    // stale first sample is about the GAP.
    //
    // Waiting for the panel to cool cannot test this -- on a thermally flat panel
    // a stale byte and a fresh one are the same byte. The offset field can: park
    // at offset 0, idle, then ask for +7 and read five times. A stale first read
    // still carries the offset-0 value from before the gap; a fresh one does not.
    // No thermal change required, and the expected step is known in advance.
    printf("# staleness across an idle gap, using a TSE offset step\n");
    printf("G,gap_s,n,raw,err\n");
    static const uint32_t gap_s[] = {30, 60, 120};
    for (size_t g = 0; g < sizeof(gap_s) / sizeof(gap_s[0]); g++) {
        // Park at offset 0 with reads flowing, so the pre-gap value is known.
        uint8_t park[2] = {0};
        for (int i = 0; i < 3; i++) {
            epd_probe_offset_step(0, 0, park);
        }
        printf("G,%u,park,%02X%02X,ESP_OK\n", (unsigned)gap_s[g],
               park[0], park[1]);
        fflush(stdout);

        vTaskDelay(pdMS_TO_TICKS(gap_s[g] * 1000));

        for (int i = 0; i < 5; i++) {
            uint8_t raw[2] = {0};
            const esp_err_t e = epd_probe_offset_step(7, 0, raw);
            printf("G,%u,%d,%02X%02X,%s\n", (unsigned)gap_s[g], i,
                   raw[0], raw[1], esp_err_to_name(e));
            fflush(stdout);
        }
    }

    // Direct proof of the one-conversion lag, and of the fix.
    //
    // Two earlier attempts at this were blind: on a thermally flat panel a stale
    // byte and a fresh one are the SAME byte, so "first read equals second read"
    // proved nothing. The gap has to contain a real change. So: heat the panel
    // with three refreshes, read it while it is hot, then idle 60 s while it
    // cools and read twice back to back.
    //
    // Under the one-behind model the first read still carries the hot value and
    // the second carries the cooled one; the API, which now reads twice, should
    // agree with the SECOND.
    printf("# one-conversion lag across a gap the temperature moved in\n");
    printf("P,rep,hot_raw,after_gap_first,after_gap_second,api_temp_c\n");
    for (int rep = 0; rep < 2; rep++) {
        for (int i = 0; i < 3; i++) {
            epd_timings_t t = {0};
            epd_refresh_solid(i % 2 ? EPD_COLOR_WHITE : EPD_COLOR_BLACK,
                              EPD_FRS_STOCK, &t);
        }
        uint8_t hot[2] = {0};
        for (int i = 0; i < 3; i++) {
            epd_probe_offset_step(0, 0, hot); // flowing reads: known hot value
        }

        vTaskDelay(pdMS_TO_TICKS(60000));

        uint8_t r1[2] = {0}, r2[2] = {0};
        epd_probe_offset_step(0, 0, r1);
        epd_probe_offset_step(0, 0, r2);
        int api = -999;
        const esp_err_t e = epd_read_temperature(0, &api);
        printf("P,%d,%02X%02X,%02X%02X,%02X%02X,%d,%s\n", rep,
               hot[0], hot[1], r1[0], r1[1], r2[0], r2[1], api,
               esp_err_to_name(e));
        fflush(stdout);
    }

#ifdef TEMPPROBE_HEAT
    // Does the byte move? A constant 0x23 would be as fabricated as a constant 0x00.
    // A hand on the glass is the ticket's suggested step and there is nobody here to
    // provide one, so the panel heats itself: six back-to-back refreshes at the
    // spacing ticket 17 already characterised. This is a smaller and less controlled
    // step than a hand -- it confirms the reading tracks, it does not calibrate it.
    for (int i = 0; i < 6; i++) {
        epd_timings_t t = {0};
        err = epd_refresh_solid(i % 2 ? EPD_COLOR_WHITE : EPD_COLOR_BLACK,
                                EPD_FRS_STOCK, &t);
        printf("# refresh %d: %s drf_ms=%.1f\n", i, esp_err_to_name(err),
               (double)t.drf_us / 1000.0);
        fflush(stdout);
    }
    probe_phase("hot");

    // And back down again. If it rises with the refreshes and falls afterwards, it
    // is measuring something; a stuck constant cannot do both.
    printf("# cooling for 300 s\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300000));
    probe_phase("cool");

#endif // TEMPPROBE_HEAT

    // BUSY is the panel's other output, and it is wired to its own pin rather than
    // to the SPI bus. If it reads idle the panel is alive and answering something,
    // which separates "this panel is dead" from "this read path is wrong".
    printf("# last setup step: %s\n", esp_err_to_name(err));
    printf("# busy pin reads %s\n", epd_busy_is_idle() ? "idle" : "busy");

    ESP_LOGI(TAG, "probe complete");
    printf("@@DONE\n");
    printf("# done\n");
    fflush(stdout);
}

#endif // BUILD_TEMPPROBE
