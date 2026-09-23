#include "app_clock.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include "app_schedule.h"
#include "app_settings.h"
#include "board_wifi.h"

static const char *TAG = "clock";

// Scoped by the owner: a well-known public server, no custom or self-hosted one.
#define NTP_SERVER "time.google.com"

// The first attempt waits for the station to associate rather than racing it. board_wifi_init()
// has returned by the time app_clock_init() runs, but association is asynchronous and takes a
// few seconds more.
#define FIRST_ATTEMPT_MS 30000

// One sync a day, which is the whole of the owner's request. The RX8130's own rate is
// -0.004 % measured over 4774 s (docs/phase0-measurement-harness.md), so a day of drift is
// about three seconds -- there is nothing here that needs better.
#define RESYNC_MS (24 * 60 * 60 * 1000)

// While unsynced, try again on this cadence. Not one minute -- a frame on its own AP has no
// uplink at all and would emit for ever for nothing. Not one hour either: the frame works
// perfectly well with no clock, but ticket 42's schedule is inert until one arrives, so a
// network that comes back should be noticed inside a quarter of an hour rather than an hour.
#define RETRY_MS (15 * 60 * 1000)

// How long one attempt is allowed to hold a socket. Bounded on purpose: an SNTP client left
// started is one more thing holding a socket while the mirror has httpd down, and lwIP's own
// retry cadence inside this window is already several tries.
#define ATTEMPT_MS 120000

// Anything earlier than this means the clock was never really set. 2020-01-01.
#define SANE_EPOCH_S 1577836800

static esp_timer_handle_t s_due_timer;
static esp_timer_handle_t s_stop_timer;

// Written by the esp_timer task before an attempt starts, read by the lwIP callback while that
// attempt runs. One writer, and the reader only runs between the write and the stop.
static int64_t s_attempt_wall_s;
static int64_t s_attempt_mono_us;

// Set by the lwIP callback, consumed by the esp_timer task. A bool and an int32 rather than a
// 64-bit field precisely so that the cross-task hand-off needs no lock on this 32-bit core.
static volatile bool s_sync_flag;
static volatile int32_t s_step_sec;

// Owned by the esp_timer task alone.
static bool s_synced;
static bool s_trying;
static uint16_t s_attempts;
static int64_t s_last_sync_mono_us = INT64_MIN;
static bool s_warned_no_uplink;

// Called from the lwIP context when settimeofday() has happened. Keep it short: one line, no
// locks, no allocation.
static void on_sync(struct timeval *tv)
{
    // What the clock WOULD have said had the sync not landed, so the log carries the step
    // rather than a plausible-looking absolute value. This is the difference between "the
    // frame has a clock" and "the frame has the right clock", and only the step shows it.
    const int64_t elapsed_us = esp_timer_get_time() - s_attempt_mono_us;
    const int64_t expected_s = s_attempt_wall_s + (elapsed_us / 1000000);
    const int64_t step = (int64_t)tv->tv_sec - expected_s;
    s_step_sec = (int32_t)step;
    s_sync_flag = true;

    const time_t t = (time_t)tv->tv_sec;
    struct tm utc;
    gmtime_r(&t, &utc);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &utc);
    printf("# ntp sync ok utc=\"%s\" step=%lld s tz=%+d min attempt=%u\n", stamp,
           (long long)step, (int)app_settings_tz_offset_minutes(), (unsigned)s_attempts);
    fflush(stdout);

    // END THE ATTEMPT NOW rather than letting it run out ATTEMPT_MS, and this is a fix rather
    // than an optimisation. `s_synced` is published by stop_attempt(), so until that runs
    // app_clock_synced() still says false -- and app_clock_local_hour() with it, which means
    // ticket 42's schedule keeps failing open for two minutes AFTER the clock is correct.
    // Measured on hardware 2026-09-10 before this line existed: the sync landed at t=17 s and
    // the heartbeat still read `synced=0 hour=-1` at t=130 s, with the clock right the whole
    // time. It also gives back ~118 s of a socket held for nothing, which is the cost this
    // module's shape is chosen to avoid.
    //
    // esp_timer_start_once() from the lwIP task is fine -- it is a task context, not an ISR --
    // and both handles exist before the client can ever be started.
    esp_timer_stop(s_stop_timer);
    esp_timer_start_once(s_stop_timer, 1000);
}

static void arm_due(uint32_t after_ms)
{
    esp_timer_stop(s_due_timer);  // harmless when not running
    const esp_err_t err = esp_timer_start_once(s_due_timer, (uint64_t)after_ms * 1000);
    if (err != ESP_OK) {
        // Cannot re-arm, so this is the end of the clock for this boot. Said out loud because
        // the alternative is a frame that silently never syncs again.
        ESP_LOGE(TAG, "cannot re-arm the sync timer: %s -- no further attempts this boot",
                 esp_err_to_name(err));
    }
}

// Ends the attempt: stop the client, take the callback's result if one landed, and decide when
// to try next.
static void stop_attempt(void *arg)
{
    (void)arg;
    if (s_trying) {
        // esp_sntp_stop() posts to the tcpip thread via tcpip_callback() and does not wait,
        // which is why it is used here rather than esp_netif_sntp_deinit(): this runs in the
        // esp_timer task, and blocking that task delays every other timer in the system.
        esp_sntp_stop();
        s_trying = false;
    }
    if (s_sync_flag) {
        s_sync_flag = false;
        s_synced = true;
        s_last_sync_mono_us = esp_timer_get_time();
        arm_due(RESYNC_MS);
        return;
    }
    ESP_LOGW(TAG, "attempt %u reached no server in %d s; retrying in %d min",
             (unsigned)s_attempts, ATTEMPT_MS / 1000, RETRY_MS / 60000);
    arm_due(RETRY_MS);
}

static void start_attempt(void *arg)
{
    (void)arg;

    // No station link means no uplink: the frame's own access point has no route to the
    // internet. Skip rather than emit, and say so once rather than every quarter of an hour.
    board_wifi_status_t st;
    board_wifi_status(&st);
    if (!st.sta_connected) {
        if (!s_warned_no_uplink) {
            s_warned_no_uplink = true;
            ESP_LOGI(TAG, "no station link; the clock waits for one (retrying quietly)");
        }
        arm_due(RETRY_MS);
        return;
    }
    s_warned_no_uplink = false;

    struct timeval now;
    gettimeofday(&now, NULL);
    s_attempt_wall_s = (int64_t)now.tv_sec;
    s_attempt_mono_us = esp_timer_get_time();
    s_attempts++;
    s_sync_flag = false;

    // esp_sntp_init() rather than esp_netif_sntp_start(): the latter goes through
    // esp_netif_tcpip_exec(), which blocks this task until the tcpip thread services it, and
    // the two are otherwise identical -- esp_netif's start_api is `sntp_stop(); sntp_init();`.
    // The servers and the operating mode were set by esp_netif_sntp_init() at boot.
    esp_sntp_init();
    s_trying = true;

    esp_timer_stop(s_stop_timer);
    const esp_err_t err = esp_timer_start_once(s_stop_timer, (uint64_t)ATTEMPT_MS * 1000);
    if (err != ESP_OK) {
        // Without the stop timer the client would stay started for ever, which is the one
        // thing this design is shaped to avoid. Stop now and try again later.
        ESP_LOGE(TAG, "cannot arm the attempt timeout: %s", esp_err_to_name(err));
        esp_sntp_stop();
        s_trying = false;
        arm_due(RETRY_MS);
    }
}

esp_err_t app_clock_init(void)
{
    if (s_due_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.start = false;          // driven by the timers below, not at init
    cfg.wait_for_sync = false;  // nothing here blocks on a sync, so the semaphore is waste
    cfg.sync_cb = on_sync;
    const esp_err_t ierr = esp_netif_sntp_init(&cfg);
    if (ierr != ESP_OK) {
        ESP_LOGE(TAG, "sntp init: %s -- the frame runs without a clock", esp_err_to_name(ierr));
        return ierr;
    }

    const esp_timer_create_args_t due_args = {
        .callback = start_attempt,
        .name = "clk_due",
        .dispatch_method = ESP_TIMER_TASK,
    };
    const esp_timer_create_args_t stop_args = {
        .callback = stop_attempt,
        .name = "clk_stop",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_err_t err = esp_timer_create(&due_args, &s_due_timer);
    if (err == ESP_OK) {
        err = esp_timer_create(&stop_args, &s_stop_timer);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer create: %s -- the frame runs without a clock",
                 esp_err_to_name(err));
        return err;
    }

    // A SOFTWARE RESET DOES NOT LOSE THE CLOCK, which this module was written assuming it did.
    // Measured 2026-09-10: after a reflash (`boot=11`, `ESP_RST_USB`) the next sync reported
    // **step=1 s** — the system clock was already right, because IDF keeps the boot-time offset
    // in the RTC domain and a CPU reset preserves it. What did not survive is `s_synced`, a
    // plain static in BSS, so the frame held a correct clock and reported `hour=-1` for the 30 s
    // until the first sync landed, and ticket 42's schedule failed open for that whole window.
    //
    // So trust the clock rather than the flag. A power cut is the case this cannot rescue and
    // does not try to — the coin cell backs the RX8130, not the SoC's RTC domain — and there
    // `time(NULL)` is near zero and the check correctly declines.
    //
    // It does NOT set `s_last_sync_mono_us`: this boot has not synced, and `since=-1` saying so
    // is the truth. Nor does it cancel the first attempt, because a clock inherited across a
    // reset is still a clock nobody has checked against a server this boot.
    if ((int64_t)time(NULL) >= SANE_EPOCH_S) {
        s_synced = true;
        printf("# clock: already set across the reset (epoch %lld), syncing anyway\n",
               (long long)time(NULL));
    }

    arm_due(FIRST_ATTEMPT_MS);
    return ESP_OK;
}

bool app_clock_synced(void)
{
    return s_synced;
}

// The local broken-down time, and the ONE place the shifted-epoch rule lives. Two copies of it
// would be two places for a timezone to be applied once, twice or not at all.
static bool local_tm(struct tm *out)
{
    if (!s_synced) {
        return false;
    }
    const time_t utc = time(NULL);
    if ((int64_t)utc < SANE_EPOCH_S) {
        // synced is set by having observed a sync callback, so this should be unreachable.
        // Checked anyway: the cost of being wrong is the panel holding one picture all day,
        // and the check is one comparison.
        return false;
    }
    // gmtime_r on a shifted epoch, not localtime_r: the offset IS the timezone here, and
    // there is deliberately no TZ string and no tzset() in this build (app_settings.h).
    const time_t local = utc + (time_t)app_settings_tz_offset_minutes() * 60;
    gmtime_r(&local, out);
    return true;
}

bool app_clock_local_hour(int *out_hour)
{
    struct tm tmv;
    if (!local_tm(&tmv)) {
        return false;
    }
    if (tmv.tm_hour < 0 || tmv.tm_hour > 23) {
        return false;
    }
    if (out_hour) {
        *out_hour = tmv.tm_hour;
    }
    return true;
}

bool app_clock_local_date(int *out_year, int *out_month, int *out_day)
{
    struct tm tmv;
    if (!local_tm(&tmv)) {
        return false;
    }
    const int year = tmv.tm_year + 1900;
    if (year < 2020 || year > 2100 || tmv.tm_mon < 0 || tmv.tm_mon > 11 || tmv.tm_mday < 1 ||
        tmv.tm_mday > 31) {
        return false;
    }
    if (out_year) {
        *out_year = year;
    }
    if (out_month) {
        *out_month = tmv.tm_mon + 1;
    }
    if (out_day) {
        *out_day = tmv.tm_mday;
    }
    return true;
}

bool app_clock_local_wday(int *out_wday)
{
    struct tm tmv;
    if (!local_tm(&tmv)) {
        return false;
    }
    // local_tm() has already refused an unsynced clock and a pre-SANE_EPOCH_S one; the range check
    // is the same shape as its two neighbours', and it matters here because tm_wday is perfectly
    // well-formed on a nonsense date and so cannot itself be the check.
    if (tmv.tm_wday < 0 || tmv.tm_wday > 6) {
        return false;
    }
    if (out_wday) {
        *out_wday = tmv.tm_wday;
    }
    return true;
}

bool app_clock_schedule_active(void)
{
    if (!app_settings_low_power_mode()) {
        return true;
    }
    int hour = APP_CLOCK_NO_HOUR;
    if (!app_clock_local_hour(&hour)) {
        hour = APP_SCHEDULE_NO_CLOCK;
    }
    uint8_t start_hour = 0, end_hour = 0;
    app_settings_active_hours(&start_hour, &end_hour);
    return app_schedule_active(true, hour, (int)start_hour, (int)end_hour);
}

void app_clock_get_status(app_clock_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->synced = s_synced;
    out->trying = s_trying;
    out->attempts = s_attempts;
    out->last_step_sec = s_step_sec;
    out->since_sync_s = UINT32_MAX;
    if (s_last_sync_mono_us != INT64_MIN) {
        out->since_sync_s =
            (uint32_t)((esp_timer_get_time() - s_last_sync_mono_us) / 1000000);
    }
    int hour = APP_CLOCK_NO_HOUR;
    out->local_hour = app_clock_local_hour(&hour) ? hour : APP_CLOCK_NO_HOUR;
}
