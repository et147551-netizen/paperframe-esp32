// The active-hours predicate of ticket 42: is the frame supposed to be changing its
// picture right now?
//
// Its own translation unit, with no esp_* dependency, for the same reason img_dims.c is:
// the wrapping window is where an obvious implementation is wrong, and a pure function is
// reachable from `pio test -e native` while a decision buried in the slideshow loop is not.
//
// THIS IS NOT FR-7. FR-7 powers the device off after each refresh and schedules an RTC wake
// (FR-7.1-7.4); that needs a PM1 SYS_CMD power-off, which is forbidden here because VBUS
// does not boot this board, and PM1 GPIO2's wake line is unverified. Ticket 15 owns FR-7 and
// is unbuilt. This is the reachable half of the same intent: do not refresh, and do not
// sync. Nothing powers off, so the cost of getting it wrong is a stale picture rather than a
// device nobody can switch back on.
//
// **Every uncertain input fails OPEN -- towards refreshing.** A schedule that failed closed
// would show one frozen picture all day on a frame whose only fault is that it never reached
// an NTP server, and the user has no way to tell that apart from a dead frame. So a missing
// clock, an out-of-range hour and a nonsensical window all mean "refresh", not "hold".

#ifndef APP_SCHEDULE_H
#define APP_SCHEDULE_H

#include <stdbool.h>

// Pass as `hour` when there is no wall clock: an unsynced boot, a failed sync, a Wi-Fi
// password that went stale months ago. app_clock_local_hour() failing is exactly this case.
#define APP_SCHEDULE_NO_CLOCK (-1)

// True when the frame should behave normally (advance the slideshow, open mirror windows).
// False only when the schedule is on, the clock is known, and the local hour is outside the
// window.
//
// `start_hour` == `end_hour` is a window with no width, which could equally mean "always" or
// "never". It is read as "always" -- see the fail-open rule above -- so that a half-filled
// settings form cannot stop the frame.
//
// A window wraps midnight when `start_hour` > `end_hour`: 22..6 is active from 22:00 through
// 05:59. The window is closed at the start and open at the end, so 7..23 holds at 23:00 and
// resumes at 07:00.
bool app_schedule_active(bool schedule_on, int hour, int start_hour, int end_hour);

#endif // APP_SCHEDULE_H
