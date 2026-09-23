// A wall clock, synced from a public NTP server once a day (ticket 41).
//
// Nothing in this application had ever asked what time it is: `sntp`, `esp_netif_sntp`,
// `settimeofday`, `time(NULL)`, `localtime` and `strftime` were zero hits across src/ before
// this file, so the system clock was never set and time(NULL) was time since boot. What
// changed is ticket 42 -- "stop refreshing when nobody is there" is a statement about local
// hours, and there is no other way to get one.
//
// TWO THINGS THIS IS NOT, both of which reading the header it sits next to would suggest:
//
//   * It does NOT write the RX8130. board_rtc_read() exists and board_rtc_write() does not,
//     and the time-register base is marked UNVERIFIED for reading -- writing a wrong register
//     on the PM1's shared I2C bus is a different risk class, and it belongs in its own arm
//     with the owner present. So the clock does not survive a power cut, and ticket 42
//     treats "no clock yet" as a first-class state instead.
//   * It does NOT make the RTC a timing reference. docs/timing-instrumentation.md §3-4 gives
//     the two jobs the RX8130 has, and a one-second calendar clock is not the measurement
//     reference for a 15.6 s refresh.
//
// **The harness builds must not contain this file.** The gross-error tripwire in
// harness_main.c compares ELAPSED esp_timer against ELAPSED RTC seconds; a sync landing
// inside a run would step one of those intervals and report a percent-level error that is not
// one. env:frame and env:m5papercolor are separate builds, so honouring that costs nothing --
// but under espidf `build_src_filter` does nothing (docs/build-system.md), so the
// mechanism is that only frame_main.c calls app_clock_init() and --gc-sections drops the rest.
//
// **CHECKED IN THE ELF rather than argued, 2026-09-10**, because "the linker will drop it" is
// exactly the kind of claim this build system has falsified before:
//
//     xtensa-esp32s3-elf-nm .pio/build/m5papercolor/firmware.elf | grep -iE 'sntp|app_clock'
//
// returns nothing for env:m5papercolor, against `app_clock_init`, `esp_netif_sntp_init`,
// `esp_sntp_init` and `sntp_init` all present in env:frame. Re-run it if this file ever gains
// a caller outside frame_main.c.

#ifndef APP_CLOCK_H
#define APP_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool synced;              // a sync has landed since boot
    bool trying;              // the SNTP client is started right now
    uint16_t attempts;        // attempts begun since boot, successful or not
    int32_t last_step_sec;    // what the last sync moved the clock by; 0 if none
    uint32_t since_sync_s;    // seconds since the last sync; UINT32_MAX if never
    int local_hour;           // 0..23, or APP_CLOCK_NO_HOUR
} app_clock_status_t;

#define APP_CLOCK_NO_HOUR (-1)

// Configures the SNTP client and arms the first attempt. Never blocks and never fails the
// boot: a frame with no uplink, or a stale Wi-Fi password, must still show photographs, so
// every failure here is a log line and nothing else.
//
// Call after board_wifi_init(). Safe to call once only.
esp_err_t app_clock_init(void);

bool app_clock_synced(void);

// Local hour 0..23 from the synced system clock plus the tz_offset_minutes setting. False
// when no sync has landed, or when the clock reads before 2020 -- which would mean synced was
// set by something that did not actually set the time. Callers pass the failure straight to
// app_schedule_active() as APP_SCHEDULE_NO_CLOCK.
bool app_clock_local_hour(int *out_hour);

// The local calendar date from the same clock, the same tz offset and the same sanity floor as the
// hour above, so the two can never disagree about which day it is.
//
// **NOT `board_rtc_read()`, and that distinction cost a hardware run.** The RX8130 is a calendar
// RTC that nothing here sets from SNTP -- board.h says outright that the system clock does not come
// from it -- so its date is whatever it was last given. On 2026-09-19 it read **02-04** while this
// clock read the 19th, and the matte band's first hardware run duly printed a date four months
// stale (ticket 64). The RTC's job is run provenance that survives a reflash; it is not the wall
// clock.
bool app_clock_local_date(int *out_year, int *out_month, int *out_day);

// The local day of the week, 0 = Sunday .. 6 = Saturday, from the same clock, offset and sanity
// floor as the two above. False when there is no usable clock -- and a caller that schedules
// something weekly must treat that as "not today" rather than as day 0, or an unsynced frame would
// run it every night. Ticket 68.
bool app_clock_local_wday(int *out_wday);

void app_clock_get_status(app_clock_status_t *out);

// Ticket 42's question, composed: the master switch (`low_power_mode`), the local hour, and
// the window, run through app_schedule_active(). True means behave normally.
//
// It lives here rather than in app_schedule.c because that file is deliberately free of
// esp_* and of app_settings, which is what lets `pio test -e native` reach the predicate --
// and the predicate is the part with a wrong answer in it. This function is the wiring.
//
// **Fails open in every direction**: schedule off, no clock, or a window that cannot be read
// all return true. See app_schedule.h.
bool app_clock_schedule_active(void);

#endif // APP_CLOCK_H
